/*
 * MCL rendezvous coordinator. See include/mcl/rendezvous.h for what this layer
 * is for and why mcl_node_t alone was not enough.
 *
 * THE SHAPE OF THIS FILE
 *
 * One function per stage, each doing exactly one thing and returning to the
 * pump. There is no inner loop and no recursion: a coordinator that could
 * advance two stages inside one call is a coordinator whose timing cannot be
 * reasoned about from the outside, and every deadline here is compared against
 * the caller's clock rather than against a count of iterations.
 *
 * TIME
 *
 * now_ms() is monotonic and may wrap. Every comparison is written as a
 * SUBTRACTION against the deadline rather than as `now >= deadline`, because
 * the subtraction is correct across the 2^32 wrap and the comparison is not.
 * At 49.7 days of uptime the naive form stops firing deadlines entirely, which
 * is the kind of defect that only appears in the field.
 */

#include "mcl/rendezvous.h"

#include <string.h>

/* Built-in timings, used where the config leaves a field zero. */
#define DEFAULT_ANNOUNCE_INTERVAL_MS 1500u
#define DEFAULT_RESPONSE_TIMEOUT_MS  3000u
#define DEFAULT_REPLY_SLOT_MS         400u
#define DEFAULT_MAX_ANNOUNCEMENTS      10u
#define DEFAULT_MAX_OFFER_RETRIES       2u
#define DEFAULT_PRESENCE_TTL           60u

/* True when `now` has reached or passed `deadline`, wrap-safe. */
static int elapsed(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t now_of(const mcl_rdv_t *rdv)
{
    return rdv->platform.now_ms(rdv->platform.user);
}

static uint16_t or_default(uint16_t value, uint16_t fallback)
{
    return (value != 0u) ? value : fallback;
}

static uint8_t or_default8(uint8_t value, uint8_t fallback)
{
    return (value != 0u) ? value : fallback;
}

static void queue(mcl_rdv_t *rdv, mcl_rdv_event_kind_t kind,
                  uint8_t transport_id, uint8_t profile_id)
{
    /*
     * One event is held at a time and a second one does NOT overwrite it.
     *
     * The alternative -- keeping the newest -- silently drops the earlier
     * event, and the events this layer emits are not interchangeable: losing
     * BEARER_AGREED while keeping CONTACT_MIGRATED would tell a caller that a
     * migration completed on a bearer it was never told about. The stages that
     * can produce two events in one poll are ordered so the first is the one
     * worth keeping, and the second is regenerated on the next poll from state
     * that has not changed.
     */
    if (rdv->has_pending) {
        return;
    }
    rdv->pending.kind = kind;
    rdv->pending.transport_id = transport_id;
    rdv->pending.profile_id = profile_id;
    rdv->pending.peer_ref = rdv->peer_ref;
    rdv->pending.migration_ref = rdv->migration_ref;
    rdv->has_pending = 1u;
}

/* ---------------------------------------------------------------- transmit */

/*
 * Emit one Tier-0 object on a bearer.
 *
 * The `> 0` return from tx is "the transport cannot tell whether this went
 * out", and it is NOT treated as a failure here. For a bootstrap emission that
 * distinction barely matters -- a lost PRESENCE is retried -- but the contract
 * is the same one the handoff path depends on, where treating "unknown" as
 * "not sent" is what strands a contact, so the two paths agree.
 */
static mcl_rdv_status_t emit(mcl_rdv_t *rdv, uint8_t transport_id,
                             const mcl_wire_tier0_t *object)
{
    uint8_t buffer[MCL_WIRE_TIER0_MAX_SIZE];
    size_t written = 0u;
    int32_t rc;

    if (rdv->platform.tx == NULL) {
        return MCL_RDV_ERR_CONFIG;
    }
    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, object,
                                       buffer, sizeof(buffer), &written)
        != MCL_WIRE_OK) {
        return MCL_RDV_ERR_DECODE;
    }
    rc = rdv->platform.tx(rdv->platform.user, transport_id, buffer, written);
    if (rc < 0) {
        return MCL_RDV_ERR_TRANSPORT;
    }
    return MCL_RDV_OK;
}

static void fill_presence(const mcl_rdv_t *rdv, mcl_wire_tier0_t *object)
{
    memset(object, 0, sizeof(*object));
    object->kind = MCL_WIRE_KIND_PRESENCE;
    object->priority = 1u;
    object->source_ref = rdv->config.source_ref;
    object->body.presence.capability_tag = rdv->config.capability_tag;
    object->body.presence.ttl =
        or_default8(rdv->config.presence_ttl, DEFAULT_PRESENCE_TTL);
}

static void fill_offer(const mcl_rdv_t *rdv, mcl_wire_tier0_t *object,
                       uint8_t index)
{
    memset(object, 0, sizeof(*object));
    object->kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
    object->priority = 1u;
    object->source_ref = rdv->config.source_ref;
    object->body.transport_offer.migration_ref = rdv->migration_ref;
    object->body.transport_offer.transport_id =
        rdv->config.bearer_transport_id[index];
    object->body.transport_offer.profile_id =
        rdv->config.bearer_profile_id[index];
    /*
     * endpoint_token is left zero here.
     *
     * It is the offering peer's reachability hint on the candidate bearer, and
     * only the integrator knows what that is -- an IP node's is derived from a
     * socket it owns, a BLE node's from an advertisement it controls. The SDK
     * inventing one would be inventing a rendezvous address, which is exactly
     * the prearrangement V1_SCOPE section 5.10 forbids the acceptance campaign
     * from relying on. A builder that has a token sets it on the object before
     * it goes out; a builder that has none offers without one, which is legal
     * and means "reach me by the profile's own discovery".
     */
    object->body.transport_offer.endpoint_token = 0u;
    object->body.transport_offer.validity =
        or_default8(rdv->config.presence_ttl, DEFAULT_PRESENCE_TTL);
}

/* ------------------------------------------------------------------ public */

mcl_rdv_status_t mcl_rdv_init(mcl_rdv_t *rdv,
                              const mcl_rdv_config_t *config,
                              const mcl_rdv_platform_t *platform,
                              mcl_node_t *node)
{
    uint8_t i;

    if (rdv == NULL || config == NULL || platform == NULL || node == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    if (platform->now_ms == NULL || platform->tx == NULL) {
        return MCL_RDV_ERR_CONFIG;
    }
    if (config->bearer_count == 0u ||
        config->bearer_count > MCL_RDV_MAX_BEARERS) {
        return MCL_RDV_ERR_CONFIG;
    }
    /*
     * A bearer offered with transport_id 0 is not a bearer: zero is reserved
     * in the transport-id registry so an uninitialised field never names a
     * medium. Catching it here rather than on the air is the difference
     * between a configuration error and a peer receiving a meaningless offer.
     */
    for (i = 0u; i < config->bearer_count; ++i) {
        if (config->bearer_transport_id[i] == 0u) {
            return MCL_RDV_ERR_CONFIG;
        }
    }
    if (config->bootstrap_transport_id == 0u) {
        return MCL_RDV_ERR_CONFIG;
    }

    memset(rdv, 0, sizeof(*rdv));
    rdv->config = *config;
    rdv->platform = *platform;
    rdv->node = node;
    rdv->state = MCL_RDV_STATE_IDLE;
    return MCL_RDV_OK;
}

mcl_rdv_status_t mcl_rdv_start(mcl_rdv_t *rdv)
{
    if (rdv == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    if (rdv->state != MCL_RDV_STATE_IDLE) {
        return MCL_RDV_ERR_STATE;
    }
    rdv->state = MCL_RDV_STATE_ANNOUNCING;
    rdv->announcements = 0u;
    rdv->bearer_index = 0u;
    rdv->offer_retries = 0u;
    rdv->deadline_ms = now_of(rdv);   /* announce on the next poll */
    return MCL_RDV_OK;
}

mcl_rdv_state_t mcl_rdv_state(const mcl_rdv_t *rdv)
{
    return (rdv == NULL) ? MCL_RDV_STATE_CLOSED : rdv->state;
}

uint16_t mcl_rdv_reply_delay_ms(const mcl_rdv_t *rdv)
{
    uint16_t slot;
    uint8_t byte = 0u;

    if (rdv == NULL) {
        return 0u;
    }
    slot = or_default(rdv->config.reply_slot_ms, DEFAULT_REPLY_SLOT_MS);
    if (rdv->platform.random != NULL &&
        rdv->platform.random(rdv->platform.user, &byte, 1u) == 0) {
        return (uint16_t)(((uint32_t)byte * slot) / 256u);
    }
    /*
     * No randomness available. Derived from source_ref so two peers land in
     * different slots reliably; three or more collide at a rate this cannot
     * bound, which is why the header says so out loud rather than leaving a
     * builder to find it with three machines in a room.
     */
    return (uint16_t)((rdv->config.source_ref % (uint32_t)slot));
}

/* ------------------------------------------------------------------- stages */

static void stage_announcing(mcl_rdv_t *rdv, uint32_t now)
{
    mcl_wire_tier0_t object;

    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    if (rdv->announcements >=
        or_default8(rdv->config.max_announcements, DEFAULT_MAX_ANNOUNCEMENTS)) {
        /*
         * Nobody answered. This is NOT "no common bearer": no bearer was ever
         * tried, because no peer was ever heard. Reporting the stronger
         * outcome here would tell an operator their bearers are incompatible
         * when in fact the room was empty.
         */
        rdv->state = MCL_RDV_STATE_IDLE;
        rdv->announcements = 0u;
        return;
    }

    fill_presence(rdv, &object);
    (void)emit(rdv, rdv->config.bootstrap_transport_id, &object);
    rdv->announcements++;
    rdv->deadline_ms = now + or_default(rdv->config.announce_interval_ms,
                                        DEFAULT_ANNOUNCE_INTERVAL_MS);
}

static void stage_heard(mcl_rdv_t *rdv, uint32_t now)
{
    mcl_wire_tier0_t object;

    /* The reply slot: see mcl_rdv_reply_delay_ms. */
    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    if (rdv->bearer_index >= rdv->config.bearer_count) {
        rdv->state = MCL_RDV_STATE_EXHAUSTED;
        queue(rdv, MCL_RDV_EVENT_NO_COMMON_BEARER, 0u, 0u);
        return;
    }

    /*
     * migration_ref correlates one transport-change transaction. Zero is
     * reserved, so a counter that starts at zero must skip it -- an offer
     * carrying migration_ref 0 names no transaction and a delayed acceptance
     * of it could not be told from the acceptance of any other.
     */
    if (rdv->migration_ref == 0u) {
        rdv->migration_ref = rdv->config.source_ref | 1u;
    }

    fill_offer(rdv, &object, rdv->bearer_index);
    /*
     * Recorded on the contact BEFORE it goes out. The contact layer owns the
     * migration state machine, and the collision tiebreaker it applies when
     * the peer offers at the same moment can only run against an offer it
     * knows about. A coordinator that emitted first and recorded later would
     * have a window in which a simultaneous peer offer looked uncontested.
     */
    (void)mcl_contact_record_offer(mcl_node_get_contact(rdv->node),
                                   rdv->migration_ref,
                                   object.body.transport_offer.transport_id,
                                   object.body.transport_offer.profile_id,
                                   object.body.transport_offer.endpoint_token,
                                   object.body.transport_offer.validity);
    (void)emit(rdv, rdv->config.bootstrap_transport_id, &object);
    rdv->state = MCL_RDV_STATE_OFFERING;
    rdv->deadline_ms = now + or_default(rdv->config.response_timeout_ms,
                                        DEFAULT_RESPONSE_TIMEOUT_MS);
}

static void stage_offering(mcl_rdv_t *rdv, uint32_t now)
{
    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    /*
     * Silence. Retry the SAME bearer up to the bound before moving on: on a
     * bootstrap bearer a lost offer and an unsupported bearer look identical,
     * and Experiment 011 measured the 17-byte TRANSPORT_OFFER failing often
     * enough on a real acoustic path that a single unanswered offer is not
     * evidence about the peer at all.
     */
    if (rdv->offer_retries <
        or_default8(rdv->config.max_offer_retries, DEFAULT_MAX_OFFER_RETRIES)) {
        rdv->offer_retries++;
        rdv->state = MCL_RDV_STATE_HEARD;
        rdv->deadline_ms = now;
        return;
    }

    rdv->offer_retries = 0u;
    rdv->bearer_index++;
    rdv->migration_ref = 0u;
    if (rdv->bearer_index >= rdv->config.bearer_count) {
        rdv->state = MCL_RDV_STATE_EXHAUSTED;
        queue(rdv, MCL_RDV_EVENT_NO_COMMON_BEARER, 0u, 0u);
        return;
    }
    rdv->state = MCL_RDV_STATE_HEARD;
    rdv->deadline_ms = now;
}

mcl_rdv_status_t mcl_rdv_poll(mcl_rdv_t *rdv, mcl_rdv_event_t *out)
{
    uint32_t now;

    if (rdv == NULL || out == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    now = now_of(rdv);

    switch (rdv->state) {
    case MCL_RDV_STATE_ANNOUNCING: stage_announcing(rdv, now); break;
    case MCL_RDV_STATE_HEARD:      stage_heard(rdv, now);      break;
    case MCL_RDV_STATE_OFFERING:   stage_offering(rdv, now);   break;
    default: break;
    }

    if (rdv->has_pending) {
        *out = rdv->pending;
        rdv->has_pending = 0u;
        rdv->last_event_ms = now;
    } else {
        memset(out, 0, sizeof(*out));
        out->kind = MCL_RDV_EVENT_NONE;
    }
    return MCL_RDV_OK;
}

mcl_rdv_status_t mcl_rdv_deliver(mcl_rdv_t *rdv,
                                 uint8_t transport_id,
                                 const uint8_t *data,
                                 size_t size)
{
    mcl_wire_tier0_t object;
    size_t consumed = 0u;
    uint32_t now;

    if (rdv == NULL || data == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    if (rdv->state == MCL_RDV_STATE_IDLE ||
        rdv->state == MCL_RDV_STATE_CLOSED) {
        return MCL_RDV_ERR_STATE;
    }
    /*
     * Anything at all arrives on a public bootstrap bearer. A refusal here is
     * a normal outcome and not an error condition: returning a hard failure
     * for undecodable bytes would make a caller treat noise as a fault.
     */
    if (mcl_wire_tier0_decode(data, size, &object, &consumed) != MCL_WIRE_OK) {
        return MCL_RDV_OK;
    }
    /* Our own emission, heard back. Nothing to do, and specifically not a
       peer: a node that answered its own PRESENCE would rendezvous with
       itself. */
    if (object.source_ref == rdv->config.source_ref) {
        return MCL_RDV_OK;
    }

    now = now_of(rdv);

    switch (object.kind) {
    case MCL_WIRE_KIND_PRESENCE:
        if (rdv->state == MCL_RDV_STATE_ANNOUNCING) {
            rdv->peer_ref = object.source_ref;
            rdv->peer_seen = 1u;
            rdv->state = MCL_RDV_STATE_HEARD;
            /* Reply in a slot, never immediately: contention is protocol. */
            rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
            queue(rdv, MCL_RDV_EVENT_PEER_DISCOVERED, transport_id, 0u);
        }
        break;

    case MCL_WIRE_KIND_TRANSPORT_OFFER:
        /*
         * The peer offered first. Whether to accept is not decided here beyond
         * one question the SDK can answer: is the offered bearer one this
         * deployment mandates? A bearer outside the deployment's set is
         * refused by silence, because accepting it would put the contact
         * somewhere the deployment profile does not describe.
         */
        if (rdv->state == MCL_RDV_STATE_ANNOUNCING ||
            rdv->state == MCL_RDV_STATE_HEARD ||
            rdv->state == MCL_RDV_STATE_OFFERING) {
            uint8_t i;

            /*
             * GLARE. Two autonomous machines will eventually offer at the same
             * moment, and the wrong answer here is the obvious one: if each
             * simply accepts the other, both believe they are migrating and
             * two transactions are live on one contact.
             *
             * The tiebreaker is NOT reinvented here. mcl_contact_resolve_offer_
             * collision already defines it -- the larger (source_ref,
             * migration_ref) key wins the controller role, and an exact tie
             * aborts both, because a tie means the keys cannot be distinguished
             * and continuing would leave the peers disagreeing about who is in
             * charge. Both machines compare the same two values and reach the
             * same conclusion with no extra round trip.
             *
             * On LOCAL_WINS we stay in OFFERING and wait for the peer's
             * acceptance of OUR offer; the peer, having lost, sends it.
             */
            if (rdv->state == MCL_RDV_STATE_OFFERING) {
                mcl_contact_collision_t outcome = MCL_CONTACT_COLLISION_LOCAL_WINS;
                mcl_link_status_t lst;

                lst = mcl_contact_resolve_offer_collision(
                          mcl_node_get_contact(rdv->node),
                          object.source_ref,
                          object.body.transport_offer.migration_ref,
                          object.body.transport_offer.transport_id,
                          object.body.transport_offer.profile_id,
                          object.body.transport_offer.endpoint_token,
                          object.body.transport_offer.validity,
                          &outcome);
                if (lst != MCL_LINK_OK) {
                    return MCL_RDV_OK;
                }
                if (outcome == MCL_CONTACT_COLLISION_LOCAL_WINS) {
                    return MCL_RDV_OK;   /* keep ours; the peer will accept it */
                }
                if (outcome != MCL_CONTACT_COLLISION_PEER_WINS) {
                    /*
                     * TIE_ABORT. Both transactions are gone; go back to trying
                     * this bearer from the top rather than pretending an offer
                     * is still outstanding.
                     */
                    rdv->migration_ref = 0u;
                    rdv->state = MCL_RDV_STATE_HEARD;
                    rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
                    return MCL_RDV_OK;
                }
            }

            for (i = 0u; i < rdv->config.bearer_count; ++i) {
                if (rdv->config.bearer_transport_id[i] ==
                    object.body.transport_offer.transport_id) {
                    mcl_wire_tier0_t reply;

                    rdv->peer_ref = object.source_ref;
                    rdv->migration_ref =
                        object.body.transport_offer.migration_ref;

                    memset(&reply, 0, sizeof(reply));
                    reply.kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
                    reply.priority = 1u;
                    reply.source_ref = rdv->config.source_ref;
                    reply.body.transport_accept.migration_ref =
                        rdv->migration_ref;
                    reply.body.transport_accept.transport_id =
                        object.body.transport_offer.transport_id;
                    reply.body.transport_accept.profile_id =
                        object.body.transport_offer.profile_id;
                    reply.body.transport_accept.session_ref =
                        rdv->config.source_ref;
                    (void)emit(rdv, rdv->config.bootstrap_transport_id, &reply);

                    rdv->state = MCL_RDV_STATE_AGREED;
                    queue(rdv, MCL_RDV_EVENT_BEARER_AGREED,
                          object.body.transport_offer.transport_id,
                          object.body.transport_offer.profile_id);
                    break;
                }
            }
        }
        break;

    case MCL_WIRE_KIND_TRANSPORT_ACCEPT:
        /*
         * Only an acceptance of the transaction we are actually running counts.
         * migration_ref exists precisely so a delayed acceptance of an
         * abandoned offer is not mistaken for this one -- transport_id and
         * profile_id are usually identical across a retry, so they cannot
         * tell them apart.
         */
        if (rdv->state == MCL_RDV_STATE_OFFERING &&
            object.body.transport_accept.migration_ref == rdv->migration_ref) {
            rdv->peer_ref = object.source_ref;
            rdv->state = MCL_RDV_STATE_AGREED;
            queue(rdv, MCL_RDV_EVENT_BEARER_AGREED,
                  object.body.transport_accept.transport_id,
                  object.body.transport_accept.profile_id);
        }
        break;

    default:
        /* A kind this stage has no use for. Refused by doing nothing. */
        break;
    }
    return MCL_RDV_OK;
}

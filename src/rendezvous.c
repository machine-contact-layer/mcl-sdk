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
#define DEFAULT_BACKOFF_SLOTS          16u
#define DEFAULT_BACKOFF_SLOT_MS       250u
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

/*
 * A FRESH transaction reference, never the previous one.
 *
 * wire.h: migration_ref "correlates one transport-change transaction, and
 * nothing else". The first version computed `source_ref | 1`, a constant for
 * the life of the node, so an abandoned transaction and its successor carried
 * the identical value -- and the stale-acceptance check that exists precisely
 * to tell them apart was comparing a number against itself.
 *
 * Randomness is used when present. The counter fallback is deterministic but
 * still monotone within one node, which is what this field needs; uniqueness
 * ACROSS nodes is not required, because an acceptance is now matched on peer,
 * transport and profile as well.
 */
/*
 * The contact's session reference, generated once by the accepting side.
 *
 * Non-zero by construction: zero is the reserved "no session" value and
 * mcl_contact_agree refuses it. Distinct from source_ref by construction too,
 * because the two are different references that merely share a width.
 */
static void stage_admitting(mcl_rdv_t *rdv, uint32_t now);
static void stage_validating(mcl_rdv_t *rdv, uint32_t now);
static void stage_committing(mcl_rdv_t *rdv, uint32_t now);

static uint32_t fresh_session_ref(mcl_rdv_t *rdv)
{
    uint32_t value;

    rdv->transaction_counter++;
    value = (rdv->config.source_ref ^ 0xA5A5A5A5u)
            + (rdv->transaction_counter * 0x85EBCA6Bu);
    if (value == 0u || value == rdv->config.source_ref) {
        value = 0x5EED0001u + rdv->transaction_counter;
    }
    return value;
}

static uint32_t fresh_migration_ref(mcl_rdv_t *rdv)
{
    uint32_t value = 0u;
    unsigned attempt;

    /*
     * A COUNTER, NOT RANDOMNESS.
     *
     * This field is transaction identity for correlation, not a nonce and not
     * a secret -- see mcl-link's note that the Link sequence is not a nonce
     * either. Randomness gives a probabilistic guarantee where a counter gives
     * an absolute one: a monotonically advancing non-zero counter cannot
     * repeat a reference within the contact until 32-bit exhaustion, whereas
     * random draws collide with birthday probability for no benefit.
     *
     * Randomness is still used where it is actually needed and cannot be
     * replaced by a counter: contention backoff, where the point is that two
     * uncoordinated builders must not choose alike, and path validation
     * challenges, where the point is that the value must be unpredictable to
     * the peer before it arrives.
     */
    for (attempt = 0u; attempt < 4u; ++attempt) {
        rdv->transaction_counter++;
        value = rdv->config.source_ref
                ^ (rdv->transaction_counter * 0x9E3779B9u);
        /* Zero is reserved: it names no transaction. */
        if (value == 0u) {
            continue;
        }
        if (value != rdv->last_migration_ref) {
            break;
        }
    }
    if (value == 0u) {
        value = rdv->last_migration_ref + 1u;
        if (value == 0u) value = 1u;
    }
    rdv->last_migration_ref = value;
    return value;
}

/* Non-zero when the medium is known to be busy. A platform that cannot sense
   reports idle, which is why a shared-medium deployment must supply this. */
static int medium_busy(const mcl_rdv_t *rdv)
{
    if (rdv->platform.medium_busy == NULL) {
        return 0;
    }
    return rdv->platform.medium_busy(rdv->platform.user) != 0;
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
    rdv->pending.peer_endpoint_token = rdv->peer_endpoint_token;
    rdv->pending.session_ref = rdv->session_ref;
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
     * The offering peer's OWN reachability hint on the candidate bearer, taken
     * from configuration.
     *
     * This used to be hardcoded to zero with a comment saying a builder "sets
     * it on the object before it goes out". A builder could not: the object is
     * constructed and emitted inside this module and never surfaces in between,
     * so the documented hook did not exist. It lives in the config now, which
     * is where the integrator is.
     *
     * Zero stays legal and means "reach me by the profile's own discovery". It
     * is NOT prearrangement -- V1_SCOPE section 5.10 forbids a peer being TOLD
     * the other's address in advance, and a token this machine publishes about
     * ITSELF, carried inside the bootstrap exchange, is the opposite of that.
     */
    object->body.transport_offer.endpoint_token =
        rdv->config.bearer_endpoint_token[index];
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
    /*
     * A shared-medium deployment must be able to draw randomness and to sense
     * the medium, and the configuration is REFUSED without both.
     *
     * Neither is a nicety. Without randomness the backoff becomes a function of
     * source_ref, which carries no uniqueness property, so two builders that
     * legally chose the same value collide on every round forever. Without
     * sensing a backoff can only delay, and delay alone cannot separate
     * transmissions longer than the window: a 17-byte TRANSPORT_OFFER occupies
     * 786.7 ms of air. Accepting the configuration and behaving badly at
     * runtime would hide an unmeetable claim behind a working two-node test.
     */
    if (config->shared_medium != 0u) {
        if (platform->random == NULL || platform->medium_busy == NULL) {
            return MCL_RDV_ERR_CONFIG;
        }
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

uint16_t mcl_rdv_reply_delay_ms(mcl_rdv_t *rdv)
{
    uint16_t width;
    uint8_t slots;
    uint8_t byte = 0u;
    uint32_t chosen;

    if (rdv == NULL) {
        return 0u;
    }
    width = or_default(rdv->config.backoff_slot_ms, DEFAULT_BACKOFF_SLOT_MS);
    slots = or_default8(rdv->config.backoff_slots, DEFAULT_BACKOFF_SLOTS);

    if (rdv->platform.random != NULL &&
        rdv->platform.random(rdv->platform.user, &byte, 1u) == 0) {
        chosen = (uint32_t)byte % (uint32_t)slots;
    } else {
        /*
         * No randomness. This is refused outright for a shared medium at
         * mcl_rdv_init(); reaching here means a point-to-point deployment, so
         * a fixed slot is adequate and honest. It is NOT a contention scheme
         * and is not described as one.
         */
        chosen = 0u;
    }
    return (uint16_t)(chosen * (uint32_t)width);
}

/* ------------------------------------------------------------------- stages */

static void stage_announcing(mcl_rdv_t *rdv, uint32_t now)
{
    mcl_wire_tier0_t object;

    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    /*
     * DEFER ON A BUSY MEDIUM, INCLUDING FOR PRESENCE.
     *
     * Announcement was the earlier of the two collision problems and the less
     * obvious one. Every coordinator enters ANNOUNCING with its first deadline
     * set to "now", so machines powered on together all transmit immediately;
     * a fixed 1500 ms interval then keeps them phase-locked and they collide
     * again on every round. Deferring breaks the lock, and re-drawing a backoff
     * afterwards keeps it broken.
     */
    if (medium_busy(rdv)) {
        rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
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
    /* Interval plus a fresh backoff, so two machines that once announced
       together do not keep doing so. */
    rdv->deadline_ms = now
                       + or_default(rdv->config.announce_interval_ms,
                                    DEFAULT_ANNOUNCE_INTERVAL_MS)
                       + mcl_rdv_reply_delay_ms(rdv);
}

static void stage_heard(mcl_rdv_t *rdv, uint32_t now)
{
    mcl_wire_tier0_t object;

    /* The backoff slot: see mcl_rdv_reply_delay_ms. */
    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    /* Slot expired: transmit only if the medium is idle, else redraw. This
       deferral is the half a pure delay cannot provide. */
    if (medium_busy(rdv)) {
        rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
        return;
    }
    if (rdv->bearer_index >= rdv->config.bearer_count) {
        rdv->state = MCL_RDV_STATE_EXHAUSTED;
        queue(rdv, MCL_RDV_EVENT_NO_COMMON_BEARER, 0u, 0u);
        return;
    }

    /*
     * A fresh transaction reference, unless this is a RETRY of the same offer.
     * The distinction is the whole point of the field: a retry must reuse the
     * reference so a late acceptance of the first attempt still matches, and a
     * new bearer or a resumed transaction must not.
     */
    if (rdv->migration_ref == 0u) {
        rdv->migration_ref = fresh_migration_ref(rdv);
    }

    fill_offer(rdv, &object, rdv->bearer_index);
    /* Remember exactly what went out, so an acceptance can be matched against
       it rather than against the transaction reference alone. */
    rdv->offered_transport = object.body.transport_offer.transport_id;
    rdv->offered_profile = object.body.transport_offer.profile_id;
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
    /* A different bearer is a different transaction: drop the reference so the
       next offer draws a fresh one, and a late acceptance of the abandoned
       transaction can no longer match. */
    rdv->migration_ref = 0u;
    rdv->offered_transport = 0u;
    rdv->offered_profile = 0u;
    if (rdv->bearer_index >= rdv->config.bearer_count) {
        rdv->state = MCL_RDV_STATE_EXHAUSTED;
        queue(rdv, MCL_RDV_EVENT_NO_COMMON_BEARER, 0u, 0u);
        return;
    }
    rdv->state = MCL_RDV_STATE_HEARD;
    rdv->deadline_ms = now;
}

/* ------------------------------------------------------- validate + migrate
 *
 * THE STAGES THE HEADER PROMISED AND THE IMPLEMENTATION DID NOT HAVE.
 *
 * rendezvous.h says this layer drives detection through to contact migration,
 * and CANDIDATE_VALIDATED and CONTACT_MIGRATED were declared events. The poll
 * switch handled ANNOUNCING, HEARD and OFFERING only, so the machine stopped at
 * AGREED and the two events could never be emitted. A builder reading the
 * header would have integrated against a promise the code did not keep.
 *
 * Nothing here invents protocol. The controls, their ordering and their
 * irrevocability are mcl-link's; this drives them.
 */

/*
 * THE LINK LIFECYCLE HAS TO BE DRIVEN TOO, AND FORGETTING IT COSTS NOTHING
 * VISIBLE UNTIL MIGRATION.
 *
 * sdk.h gates handoff on the link being ESTABLISHED or HANDOFF -- that is the
 * single place where the two state machines cross, and the SDK enforces it. A
 * coordinator that drove only the CONTACT reached AGREED, called
 * mcl_node_send_handoff, had it refused, and sat in VALIDATING forever with
 * ZERO bytes on the candidate bearer. Nothing failed loudly; the migration just
 * never happened.
 *
 * Ordinary frames are deliberately NOT gated this way -- first contact
 * necessarily precedes establishment -- which is exactly why the omission
 * survived every test up to the point where a handoff was attempted.
 */
static void establish_link(mcl_rdv_t *rdv)
{
    mcl_link_t *link = mcl_node_get_link(rdv->node);
    mcl_link_state_t state = link->state;

    if (state == MCL_LINK_STATE_ESTABLISHED ||
        state == MCL_LINK_STATE_HANDOFF) {
        return;
    }
    /* Walk the lifecycle rather than jumping: the transition rules exist to
       stop a link claiming a state it never passed through. */
    if (state == MCL_LINK_STATE_IDLE) {
        (void)mcl_node_link_transition(rdv->node, MCL_LINK_STATE_DISCOVERED);
        state = link->state;
    }
    if (state == MCL_LINK_STATE_DISCOVERED) {
        (void)mcl_node_link_transition(rdv->node, MCL_LINK_STATE_CAPABILITIES);
        state = link->state;
    }
    if (state == MCL_LINK_STATE_CAPABILITIES) {
        (void)mcl_node_link_transition(rdv->node, MCL_LINK_STATE_NEGOTIATING);
        state = link->state;
    }
    if (state == MCL_LINK_STATE_NEGOTIATING) {
        (void)mcl_node_link_transition(rdv->node, MCL_LINK_STATE_ESTABLISHED);
    }
}

/* Send one handoff control on the transport the contact chooses. */
static mcl_rdv_status_t send_control(mcl_rdv_t *rdv, mcl_handoff_op_t op,
                                     const uint8_t *challenge)
{
    mcl_handoff_control_t control;
    uint8_t scratch[MCL_LINK_FRAME_MIN_SIZE + MCL_LINK_FRAME_MAX_OPTIONAL + 32u];
    size_t sent = 0u;

    memset(&control, 0, sizeof(control));
    control.operation = op;
    control.migration_ref = rdv->migration_ref;
    control.session_ref = rdv->session_ref;
    if (challenge != NULL) {
        memcpy(control.challenge, challenge, MCL_CONTACT_CHALLENGE_SIZE);
        control.challenge_present = 1u;
    }
    if (mcl_node_send_handoff(rdv->node, &control,
                              MCL_LINK_FLAG_SESSION | MCL_LINK_FLAG_FRAME_CHECK,
                              scratch, sizeof(scratch), &sent) != MCL_SDK_OK) {
        return MCL_RDV_ERR_TRANSPORT;
    }
    return MCL_RDV_OK;
}

/*
 * AGREED: the controller proves the candidate path before committing to it.
 *
 * Only the controller challenges. The accepting peer answers, and answering is
 * driven by arrival rather than by the clock, so it has no stage of its own.
 */
static void stage_agreed(mcl_rdv_t *rdv, uint32_t now)
{
    (void)now;
    /*
     * Nothing happens here any more.
     *
     * AGREED means the bearer is chosen, not that it is usable. The caller
     * has been handed BEARER_AGREED with the peer's endpoint token and must
     * open the candidate before anything is sent on it; mcl_rdv_candidate_ready()
     * is what moves this forward. See MCL_RDV_STATE_CANDIDATE_PENDING.
     */
    (void)rdv;
}

/*
 * VALIDATING and COMMITTING: retransmit the SAME control, never a new one.
 *
 * The handoff protocol below is idempotent by construction -- a duplicate
 * PATH_CHALLENGE, PATH_RESPONSE, COMMIT or CONFIRM is defined to be safe, and
 * that property was demonstrated at the Link layer over a hundred and four
 * switches. The coordinator sent each control exactly once and therefore threw
 * that away: a single lost PATH_RESPONSE left the controller in VALIDATING for
 * ever, with recovery machinery it never invoked.
 *
 * A retry MUST carry the same challenge and the same migration_ref. Generating
 * fresh ones would put a second transaction on the wire racing the first, and
 * the stale-acceptance rules exist precisely to make that detectable rather
 * than to make it routine.
 */
static void retransmit(mcl_rdv_t *rdv, uint32_t now, mcl_handoff_op_t op,
                       const uint8_t *challenge, uint32_t timeout)
{
    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    if (rdv->retries >= MCL_RDV_MAX_HANDOFF_RETRIES) {
        rdv->state = MCL_RDV_STATE_CLOSED;
        queue(rdv, MCL_RDV_EVENT_CONTACT_LOST,
              rdv->offered_transport, rdv->offered_profile);
        return;
    }
    rdv->retries++;
    if (send_control(rdv, op, challenge) != MCL_RDV_OK) {
        /* The transport refused outright. That is not the same as a lost
           frame and is not retried faster because of it; the deadline below
           still applies. */
        rdv->deadline_ms = now + timeout;
        return;
    }
    rdv->deadline_ms = now + timeout;
}

/*
 * ADMITTING: ask local policy, once.
 *
 * The event queue holds ONE pending event, so the controller cannot queue
 * CANDIDATE_VALIDATED and POLICY_REQUIRED in the same breath -- the second
 * would overwrite the first and the caller would never learn the path had been
 * proven. Raising the question from a stage puts the two on consecutive polls,
 * in the order they actually happen.
 *
 * Asked once per contact, not once per retransmission: a peer that had to
 * resend a challenge has not become a different stranger.
 */
static void stage_admitting(mcl_rdv_t *rdv, uint32_t now)
{
    (void)now;
    if (rdv->policy_raised) {
        return;
    }
    rdv->policy_raised = 1u;
    queue(rdv, MCL_RDV_EVENT_POLICY_REQUIRED,
          rdv->offered_transport, rdv->offered_profile);
}

static void stage_validating(mcl_rdv_t *rdv, uint32_t now)
{
    if (!rdv->is_controller) {
        return;
    }
    retransmit(rdv, now, MCL_HANDOFF_OP_PATH_CHALLENGE, rdv->challenge,
               or_default(rdv->config.response_timeout_ms,
                          DEFAULT_RESPONSE_TIMEOUT_MS));
}

static void stage_committing(mcl_rdv_t *rdv, uint32_t now)
{
    if (!rdv->is_controller) {
        return;
    }
    retransmit(rdv, now, MCL_HANDOFF_OP_COMMIT, NULL,
               or_default(rdv->config.response_timeout_ms,
                          DEFAULT_RESPONSE_TIMEOUT_MS));
}

/*
 * Begin path validation. Called from mcl_rdv_candidate_ready(), never from a
 * timer: the caller decides when the candidate is open.
 */
static mcl_rdv_status_t begin_validation(mcl_rdv_t *rdv, uint32_t now)
{
    /*
     * THE CHALLENGE MUST BE UNPREDICTABLE, AND THERE IS NO FALLBACK.
     *
     * It is not a security mechanism and is not described as one. But it does
     * have to demonstrate that the FORWARD direction of the candidate path
     * carried a frame, and the old fallback derived it from migration_ref --
     * a value the peer already holds. A peer that can compute the challenge
     * without receiving it can answer without having received it, and its
     * response then demonstrates the return direction only.
     *
     * So a coordinator with no randomness cannot validate a path, and says so
     * rather than validating something weaker under the same name.
     */
    if (rdv->platform.random == NULL) {
        return MCL_RDV_ERR_STATE;
    }
    memset(rdv->challenge, 0, sizeof(rdv->challenge));
    if (rdv->platform.random(rdv->platform.user, rdv->challenge,
                             MCL_CONTACT_CHALLENGE_SIZE) != 0) {
        return MCL_RDV_ERR_STATE;
    }
    if (mcl_contact_validation_begin(mcl_node_get_contact(rdv->node),
                                     rdv->challenge) != MCL_LINK_OK) {
        return MCL_RDV_ERR_STATE;
    }
    rdv->retries = 0u;
    if (send_control(rdv, MCL_HANDOFF_OP_PATH_CHALLENGE,
                     rdv->challenge) != MCL_RDV_OK) {
        return MCL_RDV_ERR_TRANSPORT;
    }
    rdv->state = MCL_RDV_STATE_VALIDATING;
    rdv->deadline_ms = now + or_default(rdv->config.response_timeout_ms,
                                        DEFAULT_RESPONSE_TIMEOUT_MS);
    return MCL_RDV_OK;
}

/*
 * A handoff control arrived on the candidate path.
 *
 * mcl_node_receive_handoff checks that it arrived on the transport the contact
 * expects, which is the one check path validation depends on: a PATH_RESPONSE
 * fed in from the OLD path would otherwise validate a candidate that had never
 * carried a byte.
 */
static mcl_rdv_status_t deliver_handoff(mcl_rdv_t *rdv, uint8_t transport_id,
                                        const uint8_t *data, size_t size)
{
    mcl_link_frame_t frame;
    mcl_handoff_control_t control;
    mcl_handoff_action_t action = MCL_HANDOFF_ACTION_NONE;
    size_t consumed = 0u;

    memset(&frame, 0, sizeof(frame));
    memset(&control, 0, sizeof(control));
    if (mcl_node_receive_handoff(rdv->node, transport_id, data, size,
                                 &frame, &control, &consumed) != MCL_SDK_OK) {
        return MCL_RDV_OK;   /* refused, and refusing is not a fault */
    }
    if (control.migration_ref != rdv->migration_ref ||
        control.session_ref != rdv->session_ref) {
        return MCL_RDV_OK;
    }
    if (mcl_node_apply_handoff(rdv->node, &control, &action) != MCL_SDK_OK) {
        return MCL_RDV_OK;
    }

    switch (action) {
    case MCL_HANDOFF_ACTION_SEND_PATH_RESPONSE:
        /*
         * The acceptor's point of no return.
         *
         * Answering asserts that this path reached us, and everything after it
         * follows. So local policy gets its say HERE, before the answer, and
         * not after COMMIT -- a refusal after commitment is not a refusal, it
         * is a broken contact. The challenge is held until admitted; if the
         * controller retries meanwhile, the retry carries the same challenge
         * and nothing is lost.
         */
        if (rdv->state != MCL_RDV_STATE_MIGRATED && !rdv->admitted) {
            memcpy(rdv->pending_challenge, control.challenge,
                   MCL_CONTACT_CHALLENGE_SIZE);
            rdv->has_pending_challenge = 1u;
            rdv->state = MCL_RDV_STATE_ADMITTING;
            break;
        }
        (void)send_control(rdv, MCL_HANDOFF_OP_PATH_RESPONSE,
                           control.challenge);
        break;
    case MCL_HANDOFF_ACTION_SEND_CONFIRM:
        /* A repeated COMMIT re-sends the SAME CONFIRM. The contact is already
           migrated and saying so again is exactly what recovers a lost one. */
        (void)send_control(rdv, MCL_HANDOFF_OP_CONFIRM, NULL);
        if (rdv->state != MCL_RDV_STATE_MIGRATED) {
            rdv->state = MCL_RDV_STATE_MIGRATED;
            queue(rdv, MCL_RDV_EVENT_CONTACT_MIGRATED,
                  rdv->offered_transport, rdv->offered_profile);
        }
        break;
    default:
        break;
    }

    if (control.operation == MCL_HANDOFF_OP_PATH_RESPONSE &&
        rdv->is_controller && rdv->state == MCL_RDV_STATE_VALIDATING) {
        queue(rdv, MCL_RDV_EVENT_CANDIDATE_VALIDATED,
              rdv->offered_transport, rdv->offered_profile);
        /*
         * Reachability is proven; whether to admit this stranger is not the
         * SDK's decision. COMMIT is irrevocable, so the policy boundary goes
         * immediately before it and never after.
         */
        rdv->state = MCL_RDV_STATE_ADMITTING;
    }
    if (control.operation == MCL_HANDOFF_OP_CONFIRM && rdv->is_controller &&
        rdv->state != MCL_RDV_STATE_MIGRATED) {
        rdv->state = MCL_RDV_STATE_MIGRATED;
        queue(rdv, MCL_RDV_EVENT_CONTACT_MIGRATED,
              rdv->offered_transport, rdv->offered_profile);
    }
    return MCL_RDV_OK;
}

mcl_rdv_status_t mcl_rdv_poll(mcl_rdv_t *rdv, mcl_rdv_event_t *out)
{
    uint32_t now;

    if (rdv == NULL || out == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    now = now_of(rdv);

    /*
     * A QUEUED EVENT IS RETURNED BEFORE THE MACHINE ADVANCES AGAIN.
     *
     * This ran the stage first, so on the very poll where the application was
     * to learn "we agreed on this bearer, here is the peer's endpoint token",
     * the coordinator had already sent PATH_CHALLENGE on it. The caller never
     * got the chance to resolve the token, open the endpoint, bind the
     * destination or apply admission policy -- and the simulator hid it,
     * because its candidate bearer was routable before anyone opened it.
     *
     * Returning the event first costs one extra poll and makes the boundary
     * real: nothing after BEARER_AGREED happens until the caller says so.
     */
    if (rdv->has_pending) {
        *out = rdv->pending;
        rdv->has_pending = 0u;
        rdv->last_event_ms = now;
        return MCL_RDV_OK;
    }

    switch (rdv->state) {
    case MCL_RDV_STATE_ANNOUNCING: stage_announcing(rdv, now); break;
    case MCL_RDV_STATE_HEARD:      stage_heard(rdv, now);      break;
    case MCL_RDV_STATE_OFFERING:   stage_offering(rdv, now);   break;
    case MCL_RDV_STATE_AGREED:     stage_agreed(rdv, now);     break;
    case MCL_RDV_STATE_ADMITTING:  stage_admitting(rdv, now);  break;
    case MCL_RDV_STATE_VALIDATING: stage_validating(rdv, now); break;
    case MCL_RDV_STATE_COMMITTING: stage_committing(rdv, now); break;
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

mcl_rdv_status_t mcl_rdv_candidate_ready(mcl_rdv_t *rdv)
{
    if (rdv == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    if (rdv->state != MCL_RDV_STATE_AGREED &&
        rdv->state != MCL_RDV_STATE_CANDIDATE_PENDING) {
        return MCL_RDV_ERR_STATE;
    }
    /*
     * Only the controller challenges. The acceptor answers when asked, so for
     * it "the candidate is open" is all this call means -- and it matters just
     * as much there, because a challenge that arrives before the acceptor has
     * opened its endpoint is a challenge it cannot answer.
     */
    if (!rdv->is_controller) {
        rdv->state = MCL_RDV_STATE_CANDIDATE_PENDING;
        return MCL_RDV_OK;
    }
    return begin_validation(rdv, now_of(rdv));
}

mcl_rdv_status_t mcl_rdv_admit(mcl_rdv_t *rdv)
{
    if (rdv == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    if (rdv->state != MCL_RDV_STATE_ADMITTING) {
        return MCL_RDV_ERR_STATE;
    }
    rdv->admitted = 1u;

    if (rdv->is_controller) {
        /* COMMIT is irrevocable. mcl_node_send_handoff owns the lifecycle
           transition, so a contact is never stranded in COMMITTING over a
           frame that was never sent. */
        rdv->retries = 0u;
        if (send_control(rdv, MCL_HANDOFF_OP_COMMIT, NULL) != MCL_RDV_OK) {
            /* Not fatal: COMMITTING retransmits the same COMMIT. */
            rdv->state = MCL_RDV_STATE_COMMITTING;
            rdv->deadline_ms = now_of(rdv)
                               + or_default(rdv->config.response_timeout_ms,
                                            DEFAULT_RESPONSE_TIMEOUT_MS);
            return MCL_RDV_OK;
        }
        rdv->state = MCL_RDV_STATE_COMMITTING;
        rdv->deadline_ms = now_of(rdv)
                           + or_default(rdv->config.response_timeout_ms,
                                        DEFAULT_RESPONSE_TIMEOUT_MS);
        return MCL_RDV_OK;
    }

    /* The acceptor answers the challenge it was holding. */
    rdv->state = MCL_RDV_STATE_VALIDATING;
    if (rdv->has_pending_challenge) {
        rdv->has_pending_challenge = 0u;
        (void)send_control(rdv, MCL_HANDOFF_OP_PATH_RESPONSE,
                           rdv->pending_challenge);
    }
    return MCL_RDV_OK;
}

mcl_rdv_status_t mcl_rdv_refuse(mcl_rdv_t *rdv)
{
    if (rdv == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    if (rdv->state != MCL_RDV_STATE_ADMITTING) {
        return MCL_RDV_ERR_STATE;
    }
    /*
     * Nothing is sent. There is no "refused" control on the wire and there
     * should not be: silence is the conformant refusal, and a peer that hears
     * nothing back learns exactly what it is entitled to learn.
     */
    rdv->has_pending_challenge = 0u;
    rdv->state = MCL_RDV_STATE_CLOSED;
    queue(rdv, MCL_RDV_EVENT_CONTACT_LOST,
          rdv->offered_transport, rdv->offered_profile);
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
     * Bytes on the CANDIDATE bearer after agreement are handoff controls, not
     * Tier-0 objects. They are Link frames and would never decode as Tier-0, so
     * they are routed by state and transport rather than by guessing.
     */
    if (rdv->state == MCL_RDV_STATE_AGREED ||
        rdv->state == MCL_RDV_STATE_CANDIDATE_PENDING ||
        rdv->state == MCL_RDV_STATE_ADMITTING ||
        rdv->state == MCL_RDV_STATE_VALIDATING ||
        rdv->state == MCL_RDV_STATE_COMMITTING ||
        rdv->state == MCL_RDV_STATE_MIGRATED) {
        if (transport_id != rdv->config.bootstrap_transport_id) {
            return deliver_handoff(rdv, transport_id, data, size);
        }
    }

    /*
     * Anything at all arrives on a public bootstrap bearer. A refusal here is
     * a normal outcome and not an error condition: returning a hard failure
     * for undecodable bytes would make a caller treat noise as a fault.
     */
    if (mcl_wire_tier0_decode(data, size, &object, &consumed) != MCL_WIRE_OK) {
        return MCL_RDV_OK;
    }
    /*
     * OUR OWN EMISSION, HEARD BACK.
     *
     * The platform is asked first, because self-echo is a physical fact: only
     * the integrator knows whether this machine's own emitter was driving the
     * medium when these bytes arrived.
     *
     * The source_ref comparison below is DEFENCE IN DEPTH, not the mechanism.
     * wire.h defines source_ref as a correlation reference for semantic origin,
     * explicitly not identity, with no uniqueness property -- so two unrelated
     * builders may legally choose the same value, and a coordinator that relied
     * on this test would then discard everything the other said. Two machines
     * in one room, both announcing, both deaf, and nothing anywhere reporting
     * why. A shared-medium deployment supplies the callback.
     */
    if (rdv->platform.self_transmitting != NULL &&
        rdv->platform.self_transmitting(rdv->platform.user) != 0) {
        return MCL_RDV_OK;
    }
    if (rdv->platform.self_transmitting == NULL &&
        object.source_ref == rdv->config.source_ref) {
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
             * PEER SCOPING. Once a peer has been selected from a PRESENCE, this
             * rendezvous belongs to that peer.
             *
             * Without this a third machine breaks the pair. If A announces and
             * both B and C hear it, B and C each set peer_ref = A and each
             * prepare an offer -- and C's offer, arriving at B, was accepted on
             * a bearer-list match alone. Three machines in a room could
             * therefore produce a B-C agreement that neither of them was trying
             * to make, which is worse than the collision the campaign was
             * looking for and would have been read as one.
             *
             * The two-node simulator could not show this: it never contains an
             * unrelated third offer.
             */
            if (rdv->peer_seen && object.source_ref != rdv->peer_ref) {
                return MCL_RDV_OK;
            }

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
                /*
                 * BOTH transport and profile must match a bearer this
                 * deployment mandates. Matching the transport alone accepts an
                 * offer to speak a profile we do not implement -- the migration
                 * then completes onto a bearer where nothing can be exchanged,
                 * and the failure surfaces later and somewhere else.
                 */
                if (rdv->config.bearer_transport_id[i] ==
                        object.body.transport_offer.transport_id &&
                    rdv->config.bearer_profile_id[i] ==
                        object.body.transport_offer.profile_id) {
                    mcl_wire_tier0_t reply;

                    rdv->peer_ref = object.source_ref;
                    rdv->peer_seen = 1u;
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
                    /*
                     * A SESSION IS NOT A SOURCE.
                     *
                     * This copied source_ref, which conflicts with the Link
                     * model: local_ref, peer_ref, migration_ref,
                     * endpoint_token and session_ref are distinct references
                     * that happen to share a width, and sharing a width is not
                     * a reason to share a value.
                     *
                     * It was also reachable as a defect rather than a
                     * tidiness point. source_ref == 0 is legal, zero is the
                     * reserved "no session" value, so a legal configuration
                     * manufactured an illegal session and mcl_contact_agree
                     * refused it -- while this path discarded the refusal.
                     *
                     * The accepting side generates it once, non-zero, and it
                     * persists across migrations for the life of the contact.
                     */
                    reply.body.transport_accept.session_ref =
                        fresh_session_ref(rdv);
                    (void)emit(rdv, rdv->config.bootstrap_transport_id, &reply);

                    rdv->session_ref = reply.body.transport_accept.session_ref;
                    rdv->peer_endpoint_token =
                        object.body.transport_offer.endpoint_token;
                    rdv->offered_transport =
                        object.body.transport_offer.transport_id;
                    rdv->offered_profile =
                        object.body.transport_offer.profile_id;
                    /* We accepted somebody else's offer, so they control the
                       migration. mcl_contact_resolve_offer_collision decides
                       this when both offered; here it is unambiguous. */
                    rdv->is_controller = 0u;
                    establish_link(rdv);
                    (void)mcl_contact_record_offer(
                        mcl_node_get_contact(rdv->node),
                        rdv->migration_ref,
                        object.body.transport_offer.transport_id,
                        object.body.transport_offer.profile_id,
                        object.body.transport_offer.endpoint_token,
                        object.body.transport_offer.validity);
                    (void)mcl_contact_agree(
                        mcl_node_get_contact(rdv->node),
                        rdv->migration_ref,
                        object.body.transport_offer.transport_id,
                        object.body.transport_offer.profile_id,
                        rdv->session_ref);
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
        /*
         * FOUR THINGS MUST MATCH, NOT ONE.
         *
         * The first version checked migration_ref alone, and separately
         * generated that reference as a constant -- so the guard was comparing
         * a value against itself and a delayed acceptance of an abandoned
         * transaction would have been taken for the current one.
         *
         * With a fresh reference per transaction the check has teeth, and the
         * other three close what it still cannot see: an acceptance naming a
         * different peer, or accepting a transport or profile we did not
         * offer, is not an acceptance of this transaction.
         */
        if (rdv->state == MCL_RDV_STATE_OFFERING &&
            rdv->peer_seen &&
            object.source_ref == rdv->peer_ref &&
            object.body.transport_accept.migration_ref == rdv->migration_ref &&
            object.body.transport_accept.transport_id ==
                rdv->offered_transport &&
            object.body.transport_accept.profile_id == rdv->offered_profile) {
            /*
             * session_ref is bound ONCE here, from the accepting peer, because
             * that is its defined meaning: the accepting peer's chosen
             * correlation reference for the continued contact. It is not a
             * secret -- it crosses an observable medium in the clear.
             */
            rdv->session_ref = object.body.transport_accept.session_ref;
            /*
             * THE OFFERER LEARNS NO ADDRESS FROM AN ACCEPTANCE.
             *
             * TRANSPORT_ACCEPT carries migration_ref, transport_id, profile_id
             * and session_ref -- and no endpoint_token. Only TRANSPORT_OFFER
             * has one, and it is the OFFERER own. So the acceptor can reach
             * the offerer on the candidate, and the offerer cannot reach the
             * acceptor.
             *
             * This coordinator used to send PATH_CHALLENGE from the offerer
             * regardless, which worked only because the test rooms candidate
             * bearer was routable in both directions before anyone opened it.
             * On a real socket or a real GATT connection it is a frame with
             * nowhere to go.
             *
             * The field is not addable at major 1: TRANSPORT_ACCEPT is 16
             * bytes and the size is fixed for the major. So the token stays
             * zero here and the integrator is told plainly, through the event,
             * that this side must resolve the peer address by other means --
             * typically the source address the transport reports for the
             * acceptance itself. mcl_rdv_candidate_ready() is the point at
             * which the caller asserts it can, and nothing is sent before it.
             */
            rdv->peer_endpoint_token = 0u;
            rdv->is_controller = 1u;
            /*
             * Both peers call mcl_contact_agree: the accepting peer when it
             * chooses the session reference, the offering peer when the
             * acceptance arrives. Without this the contact stays in OFFERED and
             * mcl_contact_validation_begin refuses -- which is exactly how the
             * migration stages silently did nothing.
             */
            if (mcl_contact_agree(mcl_node_get_contact(rdv->node),
                                  rdv->migration_ref,
                                  object.body.transport_accept.transport_id,
                                  object.body.transport_accept.profile_id,
                                  rdv->session_ref) != MCL_LINK_OK) {
                return MCL_RDV_OK;
            }
            establish_link(rdv);
            rdv->state = MCL_RDV_STATE_AGREED;
            rdv->deadline_ms = now;
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

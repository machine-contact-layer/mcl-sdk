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
/*
 * The defaults ARE the profile. They were separate numbers that happened to
 * agree in four places out of five and disagreed in the fifth: the response
 * timeout was 3000 ms against a legal maximum backoff of 3750 ms, so a peer
 * behaving perfectly could be declared silent before it was allowed to speak.
 * Deriving one set from the other removes the class of defect, not just that
 * instance of it.
 */
#define DEFAULT_ANNOUNCE_INTERVAL_MS MCL_RDV_AP1_ANNOUNCE_INTERVAL_MS
#define DEFAULT_RESPONSE_TIMEOUT_MS  MCL_RDV_AP1_RESPONSE_TIMEOUT_MS
/*
 * Kept separate from the response timeout even though the two are equal today.
 * They bound different waits and a waveform change need not move both; see the
 * note on MCL_RDV_AP1_SOLICIT_TIMEOUT_MS.
 */
#define DEFAULT_SOLICIT_TIMEOUT_MS   MCL_RDV_AP1_SOLICIT_TIMEOUT_MS
#define DEFAULT_BACKOFF_SLOTS        MCL_RDV_AP1_BACKOFF_SLOTS
#define DEFAULT_BACKOFF_SLOT_MS      MCL_RDV_AP1_BACKOFF_SLOT_MS
#define DEFAULT_MAX_ANNOUNCEMENTS    MCL_RDV_AP1_MAX_ANNOUNCEMENTS
#define DEFAULT_MAX_OFFER_RETRIES    MCL_RDV_AP1_MAX_OFFER_RETRIES
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
static void stage_accepting(mcl_rdv_t *rdv, uint32_t now);
static void stage_soliciting(mcl_rdv_t *rdv, uint32_t now);
static void stage_awaiting_challenge(mcl_rdv_t *rdv, uint32_t now);
static void stage_admitting(mcl_rdv_t *rdv, uint32_t now);
static void stage_validating(mcl_rdv_t *rdv, uint32_t now);
static void stage_committing(mcl_rdv_t *rdv, uint32_t now);

static uint32_t fresh_session_ref(mcl_rdv_t *rdv)
{
    uint32_t value;

    /*
     * THE INTEGRATOR ALLOCATES IF IT SAYS IT WILL.
     *
     * The generator below is a function of this node's own source_ref and its
     * own counter, so it distinguishes this node's transactions from each
     * other and says nothing about anybody else's. A builder running a pool of
     * contacts needs distinctness across the pool, which is not a thing this
     * layer can compute -- so it asks, and refuses rather than guessing when
     * the answer does not come. Zero is returned to mean "no session"; the
     * caller must not send an acceptance carrying it.
     */
    if (rdv->platform.allocate_session != NULL) {
        uint32_t allocated = 0u;
        if (rdv->platform.allocate_session(rdv->platform.user,
                                           &allocated) != 0) {
            return 0u;
        }
        return allocated;      /* zero here is the allocator's refusal */
    }

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
     * A COUNTER WHERE ONE NODE'S UNIQUENESS IS ENOUGH; RANDOM WHERE IT IS NOT.
     *
     * The counter form -- source_ref ^ (n * k) -- is right for a value that
     * only has to distinguish this node's transactions from each other, and it
     * beats randomness at that: it cannot repeat until 32-bit exhaustion,
     * where random draws collide with birthday probability for no benefit.
     *
     * It is WRONG on a shared medium, and the reason is the solicitation
     * epoch. TRANSPORT_OFFER is broadcast with no destination, so the ACCEPT
     * that echoes migration_ref is what selects one responder out of several.
     * A selector must not be guessable from a value with no uniqueness
     * property, and source_ref is exactly that (wire.h): two builders that
     * legally chose the same source_ref, both on their first transaction,
     * compute the SAME migration_ref -- and one ACCEPT then tells both of them
     * they were chosen.
     *
     * mcl_rdv_init() already requires platform.random for a shared medium, so
     * this adds no dependency. A retry does not come through here at all: the
     * caller only regenerates when rdv->migration_ref is zero, which is the
     * definition of a new transaction.
     */
    if (rdv->config.shared_medium != 0u && rdv->platform.random != NULL) {
        for (attempt = 0u; attempt < 8u; ++attempt) {
            uint8_t bytes[4];
            if (rdv->platform.random(rdv->platform.user, bytes,
                                     sizeof(bytes)) != 0) {
                break;
            }
            value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
                    ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
            if (value != 0u && value != rdv->last_migration_ref) {
                rdv->last_migration_ref = value;
                return value;
            }
            value = 0u;
        }
        /* The generator failed or kept returning unusable values. Fall through
           to the counter rather than emit zero, which names no transaction. */
    }
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

/*
 * How long a solicitor waits for the first response to its PRESENCE.
 *
 * NOT the response timeout, even though the profile's two numbers are equal.
 * Under MCL_RDV_PARAMETERS_PROFILE this is the profile's constant and nothing
 * else -- a builder cannot move it, because a solicitation window is medium
 * access and medium access is not discoverable from the wire.
 *
 * Under MCL_RDV_PARAMETERS_EXPERIMENTAL, where the Stranger-Contact claim has
 * already been given up, a stated response_timeout_ms scales this too. That is
 * a deliberate coupling in the one mode where interoperability is not being
 * claimed: research running on a compressed clock would otherwise stall six
 * real seconds on every round, and would work around it by editing the header.
 */
static uint32_t solicit_timeout(const mcl_rdv_t *rdv)
{
    if (rdv->config.parameters == MCL_RDV_PARAMETERS_EXPERIMENTAL &&
        rdv->config.response_timeout_ms != 0u) {
        return (uint32_t)rdv->config.response_timeout_ms;
    }
    return DEFAULT_SOLICIT_TIMEOUT_MS;
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
    if (rc > 0) {
        /* The transport cannot tell. Preserved rather than flattened into OK:
           see MCL_RDV_TX_UNCERTAIN. */
        return MCL_RDV_TX_UNCERTAIN;
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

/*
 * This machine's own reachability hint on one bearer.
 *
 * Zero is legal throughout and means "reach me by the profile's own
 * discovery". What is NOT legal is silently substituting the configured value
 * when an allocator refused: the refusal is the integrator saying it cannot be
 * reached there right now, and an offer is a claim that it can.
 */
static int resolve_local_token(mcl_rdv_t *rdv, uint8_t index)
{
    uint32_t token = 0u;

    if (rdv->platform.allocate_endpoint_token == NULL) {
        rdv->local_endpoint_token = rdv->config.bearer_endpoint_token[index];
        return 0;
    }
    if (rdv->platform.allocate_endpoint_token(
            rdv->platform.user,
            rdv->config.bearer_transport_id[index],
            rdv->config.bearer_profile_id[index], &token) != 0) {
        return -1;
    }
    rdv->local_endpoint_token = token;
    return 0;
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
    /*
     * Resolved ONCE per transaction, in stage_heard(), and held. Reading the
     * config here directly would be right for a fixed address and wrong the
     * moment an integrator mints a token per transaction, because a
     * retransmission would then carry a different one than the offer it is
     * repeating.
     */
    object->body.transport_offer.endpoint_token = rdv->local_endpoint_token;
    object->body.transport_offer.validity =
        or_default8(rdv->config.presence_ttl, DEFAULT_PRESENCE_TTL);
}

/* ------------------------------------------------------------------ public */

/* Zero means "take the profile's value"; anything else must equal it. */
static int profile_value(uint32_t configured, uint32_t profile)
{
    return configured == 0u || configured == profile;
}

mcl_rdv_status_t mcl_rdv_init(mcl_rdv_t *rdv,
                              const mcl_rdv_config_t *config,
                              const mcl_rdv_platform_t *platform,
                              mcl_node_t *node)
{
    uint8_t i;

    if (rdv == NULL || config == NULL || platform == NULL || node == NULL) {
        return MCL_RDV_ERR_NULL;
    }
    /*
     * ZEROED BEFORE VALIDATION, NOT AFTER.
     *
     * Every refusal below used to return with the caller's struct untouched,
     * so a builder who ignored the return value -- and this project's own test
     * suite was one -- then called mcl_rdv_start() on uninitialised stack and
     * jumped through a garbage function pointer. A configuration error became
     * a segmentation fault two calls later, which is the worst possible place
     * for it to surface.
     *
     * Zeroing first makes a refused init leave a coordinator that is merely
     * unusable: state IDLE, platform NULL, and the guard in mcl_rdv_start()
     * turns the mistake back into the error code it always was.
     */
    memset(rdv, 0, sizeof(*rdv));
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
        /*
         * THE TIMING PARAMETERS ARE PROTOCOL AND ARE NOT NEGOTIABLE HERE.
         *
         * A shared-medium deployment is making a Stranger-Contact claim, and
         * that claim is about meeting a machine built by someone else. Medium
         * access is the part neither machine can discover from the other:
         * nothing in PRESENCE, OFFER or ACCEPT carries a slot count, so two
         * builders with different numbers each behave correctly by their own
         * lights and contend by neither's.
         *
         * Zero means "the profile's value". A stated value must match it.
         * Anything else is refused HERE, at init, where it is a configuration
         * error the builder can see -- rather than on the air, where it is an
         * intermittent interoperability failure between two shipped products.
         */
        if (config->parameters == MCL_RDV_PARAMETERS_PROFILE) {
            if (!profile_value(config->backoff_slots, MCL_RDV_AP1_BACKOFF_SLOTS) ||
                !profile_value(config->backoff_slot_ms, MCL_RDV_AP1_BACKOFF_SLOT_MS) ||
                !profile_value(config->announce_interval_ms,
                               MCL_RDV_AP1_ANNOUNCE_INTERVAL_MS) ||
                !profile_value(config->response_timeout_ms,
                               MCL_RDV_AP1_RESPONSE_TIMEOUT_MS) ||
                !profile_value(config->max_announcements,
                               MCL_RDV_AP1_MAX_ANNOUNCEMENTS) ||
                !profile_value(config->max_offer_retries,
                               MCL_RDV_AP1_MAX_OFFER_RETRIES)) {
                return MCL_RDV_ERR_CONFIG;
            }
        } else if (config->parameters != MCL_RDV_PARAMETERS_EXPERIMENTAL) {
            return MCL_RDV_ERR_CONFIG;
        }
    }

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
    /* A refused mcl_rdv_init() leaves this NULL. Starting anyway is how an
       ignored return value became a crash rather than an error. */
    if (rdv->platform.now_ms == NULL || rdv->platform.tx == NULL) {
        return MCL_RDV_ERR_CONFIG;
    }
    rdv->state = MCL_RDV_STATE_ANNOUNCING;
    rdv->announcements = 0u;
    rdv->bearer_index = 0u;
    rdv->offer_retries = 0u;
    /*
     * THE FIRST PRESENCE IS BACKED OFF TOO.
     *
     * It announced on the very next poll. Later announcements each drew a
     * fresh slot, so the contention rule was applied everywhere EXCEPT the one
     * moment when machines are most likely to act together: three units
     * powered on from the same switch, or three processes started by the same
     * script, all announced at the same instant and collided by construction.
     *
     * Randomising the first one costs at most one backoff of latency and
     * removes the only synchronised instant in the whole lifecycle.
     */
    rdv->deadline_ms = now_of(rdv) + mcl_rdv_reply_delay_ms(rdv);
    return MCL_RDV_OK;
}

mcl_rdv_state_t mcl_rdv_state(const mcl_rdv_t *rdv)
{
    return (rdv == NULL) ? MCL_RDV_STATE_CLOSED : rdv->state;
}

/*
 * A uniform value in [0, bound), by rejection.
 *
 * `random_byte % bound` is uniform only when bound divides 256. The profile's
 * 16 does, which is why this was invisible, but the modulo was applied to a
 * CONFIGURED slot count -- so any builder choosing 10, or 24, or 100 got a
 * skewed distribution, with the low slots more likely. Skew in a contention
 * backoff is the one place it does real damage: it concentrates contenders
 * exactly where they must not be concentrated.
 *
 * Rejection sampling is exact for every bound and costs, on average, under two
 * draws. Bounded to eight attempts so a broken generator cannot spin here;
 * falling back to the modulo after that is worse than skew only in theory, and
 * a stuck loop in a rendezvous pump is not theory.
 */
static int draw_below(mcl_rdv_t *rdv, uint8_t bound, uint8_t *out)
{
    unsigned attempt;
    uint32_t limit;

    if (bound == 0u) {
        return -1;
    }
    if (bound == 1u) {
        *out = 0u;
        return 0;
    }
    /* The largest multiple of `bound` that fits in a byte. Values at or above
       it are the ones that would bias the modulo, so they are redrawn. */
    limit = (256u / (uint32_t)bound) * (uint32_t)bound;
    for (attempt = 0u; attempt < 8u; ++attempt) {
        uint8_t byte = 0u;
        if (rdv->platform.random(rdv->platform.user, &byte, 1u) != 0) {
            return -1;
        }
        if ((uint32_t)byte < limit) {
            *out = (uint8_t)((uint32_t)byte % (uint32_t)bound);
            return 0;
        }
    }
    return -1;
}

uint32_t mcl_rdv_reply_delay_ms(mcl_rdv_t *rdv)
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
        draw_below(rdv, slots, &byte) == 0) {
        chosen = (uint32_t)byte;
    } else {
        /*
         * No randomness. This is refused outright for a shared medium at
         * mcl_rdv_init(); reaching here means a point-to-point deployment, so
         * a fixed slot is adequate and honest. It is NOT a contention scheme
         * and is not described as one.
         */
        chosen = 0u;
    }
    /*
     * WIDE. slots x width is computed in 32 bits and was returned in 16, so
     * any configuration whose product exceeded 65535 ms wrapped to a short
     * delay -- silently, and worst for exactly the large-slot-count settings
     * someone would reach for after seeing collisions.
     */
    return chosen * (uint32_t)width;
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
    /*
     * A DEFINITE REFUSAL IS NOT AN ANNOUNCEMENT.
     *
     * This counted one either way, so a node whose transport was refusing
     * every call still "announced" its ten times and then went quiet
     * reporting that the room was empty. The room was never addressed.
     *
     * An UNCERTAIN send does count: the bytes may well have gone out, and
     * counting it is the conservative direction -- it can only make this node
     * quieter, never noisier, which is the right way to be wrong on a shared
     * medium.
     */
    if (emit(rdv, rdv->config.bootstrap_transport_id, &object)
        == MCL_RDV_ERR_TRANSPORT) {
        rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
        return;
    }
    rdv->announcements++;
    /*
     * THE PRESENCE IS OUT, SO THIS MACHINE NOW OWNS THE ROUND.
     *
     * It used to stay in ANNOUNCING, which meant it remained eligible to hear
     * somebody else's PRESENCE and become their responder while its own was
     * still being answered. That is the role confusion that stopped three
     * machines converging; see the enum comment in rendezvous.h.
     *
     * An UNCERTAIN send takes this transition too. The transport could not
     * tell whether the bytes went out, so the conservative reading is that
     * they did: assuming otherwise would have this node answer a peer that may
     * already be answering it.
     */
    rdv->state = MCL_RDV_STATE_SOLICITING;
    rdv->deadline_ms = now + solicit_timeout(rdv);
}

/*
 * SOLICITING: the round is ours and nobody answered it.
 *
 * There is nothing to do here but wait and then try again, because a
 * solicitor does not offer -- offers are the RESPONSE to a solicitation, and a
 * machine that both solicited and offered would be back to holding two roles
 * at once.
 *
 * Going back to ANNOUNCING through a fresh backoff is what breaks a
 * same-slot PRESENCE collision: two machines whose announcements destroyed
 * each other both arrive here, both re-draw, and the probability they collide
 * again is the probability they draw alike, not one.
 */
static void stage_soliciting(mcl_rdv_t *rdv, uint32_t now)
{
    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    rdv->state = MCL_RDV_STATE_ANNOUNCING;
    rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
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
        /*
         * And the token for it, once. A retry does not come back through
         * here, so the offer that goes out on attempt three is the same offer
         * that went out on attempt one.
         */
        if (resolve_local_token(rdv, rdv->bearer_index) != 0) {
            /*
             * The integrator declined to mint a token for this bearer.
             * Emitting the config's static value instead would put an address
             * on the air that the integrator did not sanction, so nothing is
             * sent -- and a retry is consumed, so a permanently failing
             * allocator moves to the next bearer and eventually reports
             * NO_COMMON_BEARER rather than spinning here.
             */
            rdv->migration_ref = 0u;
            rdv->state = MCL_RDV_STATE_OFFERING;
            rdv->deadline_ms = now;
            return;
        }
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
    if (mcl_contact_record_offer(mcl_node_get_contact(rdv->node),
                                 rdv->migration_ref,
                                 object.body.transport_offer.transport_id,
                                 object.body.transport_offer.profile_id,
                                 object.body.transport_offer.endpoint_token,
                                 object.body.transport_offer.validity)
        != MCL_LINK_OK) {
        /*
         * The contact layer refused to record it, so the collision tiebreaker
         * would not see this offer. Emitting anyway would put an offer on the
         * air that this node cannot reason about. An earlier defect survived
         * precisely because this return was discarded.
         *
         * Retrying the identical call is not a recovery -- the refusal is a
         * statement about the contact's state, not a transient. Consume a
         * retry so the bearer is eventually abandoned and the contact
         * released, rather than spinning in HEARD for ever. Three contending
         * machines deadlocked exactly there the first time this was checked.
         */
        rdv->state = MCL_RDV_STATE_OFFERING;
        rdv->deadline_ms = now;
        return;
    }
    /*
     * OFFERING means "an offer is outstanding". A definite refusal means there
     * is no offer outstanding, and entering the state anyway spends the
     * response timeout waiting for an answer to something never sent -- then
     * consumes a retry, and eventually reports NO_COMMON_BEARER about a peer
     * that was never asked.
     */
    if (emit(rdv, rdv->config.bootstrap_transport_id, &object)
        == MCL_RDV_ERR_TRANSPORT) {
        rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
        return;
    }
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
    /*
     * TELL THE CONTACT LAYER THE OFFER IS ABANDONED.
     *
     * It was not told. The coordinator dropped its own migration_ref and moved
     * to the next bearer while the contact stayed in OFFERED, so the next
     * mcl_contact_record_offer -- a different reference against an outstanding
     * offer -- was refused with MCL_LINK_ERR_INVALID_STATE, correctly. That
     * refusal was discarded, and the coordinator ran on with a contact that
     * disagreed with it about which migration was in flight.
     *
     * The divergence only became visible once the return value stopped being
     * thrown away, which is the argument for not throwing it away.
     */
    (void)mcl_contact_abandon_migration(mcl_node_get_contact(rdv->node));
    /* A different bearer is a different transaction: drop the reference so the
       next offer draws a fresh one, and a late acceptance of the abandoned
       transaction can no longer match. */
    rdv->migration_ref = 0u;
    rdv->local_endpoint_token = 0u;
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
 * AGREED HAS NO STAGE OF ITS OWN, AND THAT IS THE POINT.
 *
 * There was a stage_agreed() here. It had been emptied out when the builder
 * boundary went in -- AGREED means the bearer is CHOSEN, not that it is
 * usable, and nothing may be emitted on the candidate until
 * mcl_rdv_candidate_ready() says the integrator has opened it -- and what was
 * left was a function that took the coordinator and did nothing with it.
 *
 * Deleted rather than kept as a placeholder: an empty stage in the poll switch
 * reads like a step that exists, and the next person to look for where AGREED
 * advances would find it and conclude the answer is "nowhere" rather than "in
 * the caller". What the controller and the acceptor actually do from AGREED is
 * in mcl_rdv_candidate_ready() and stage_awaiting_challenge() respectively.
 */

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
/*
 * ACCEPTING: an acceptance is prepared and waiting for its slot.
 *
 * The same rule the offer path already followed -- wait out the backoff, then
 * transmit only into an idle medium. BEARER_AGREED is raised only once the
 * acceptance has actually left, because until then there is nothing for the
 * offerer to have agreed with.
 */
static void stage_accepting(mcl_rdv_t *rdv, uint32_t now)
{
    mcl_rdv_status_t rc;

    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    if (medium_busy(rdv)) {
        /* Defer by one fresh slot rather than by a fixed wait: two machines
           deferring to the same busy medium must not resume together. */
        rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
        return;
    }
    if (!rdv->has_pending_accept) {
        rdv->state = MCL_RDV_STATE_AGREED;
        return;
    }
    rc = emit(rdv, rdv->config.bootstrap_transport_id, &rdv->pending_accept);
    if (rc == MCL_RDV_ERR_TRANSPORT) {
        /* Definitely not sent. The offerer is still waiting, and its response
           timeout is derived to allow for exactly this. */
        rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
        return;
    }
    rdv->has_pending_accept = 0u;
    rdv->state = MCL_RDV_STATE_AGREED;
    /*
     * Long enough for the CONTROLLER'S WHOLE retransmission schedule.
     *
     * One response timeout is wrong here, and wrong in the dangerous
     * direction: the controller is allowed MCL_RDV_MAX_HANDOFF_RETRIES
     * attempts at a lost PATH_CHALLENGE, each a full timeout apart, so an
     * acceptor that gave up after one would abandon a migration that was still
     * being driven correctly. Both lost-control mutations caught it
     * immediately.
     */
    rdv->deadline_ms = now
                       + (MCL_RDV_MAX_HANDOFF_RETRIES + 1u)
                         * or_default(rdv->config.response_timeout_ms,
                                      DEFAULT_RESPONSE_TIMEOUT_MS);
    queue(rdv, MCL_RDV_EVENT_BEARER_AGREED,
          rdv->offered_transport, rdv->offered_profile);
}

/*
 * THE ACCEPTOR CANNOT WAIT FOR EVER.
 *
 * After sending TRANSPORT_ACCEPT the acceptor waited, unbounded, for a
 * PATH_CHALLENGE. If its acceptance was lost -- to a collision, on the medium
 * this profile is written for -- the offerer heard nothing, retried, ran out
 * of retries, moved to its next bearer with a fresh transaction, and finally
 * reported NO_COMMON_BEARER. The acceptor meanwhile stayed bound to the first
 * transaction for ever, deaf to the new offers because it was no longer in a
 * state that accepts them.
 *
 * Three machines in one room deadlocked exactly like that: two EXHAUSTED, one
 * still waiting on a transaction the other end had abandoned minutes earlier.
 *
 * So the wait is bounded by the same derived timeout everything else uses.
 * On expiry the contact's migration is abandoned -- telling the layer that
 * owns it, rather than just forgetting locally -- and the node goes back to
 * announcing after a fresh backoff.
 */
/*
 * Drop everything this epoch bound and go back to contending for the medium.
 *
 * ALL of it, including peer_ref and peer_seen. A partial reset is how a node
 * carried a stale peer binding into the next round and then judged a perfectly
 * good offer against a machine that had stopped talking to it minutes earlier.
 *
 * `announcements` is deliberately NOT reset. It bounds the total effort this
 * node spends looking for a stranger before it reports back to the
 * application, and a bound that starts over on every abandoned epoch is not a
 * bound. IDLE is a reportable outcome; an application that wants to keep
 * looking calls mcl_rdv_start() again, which is where that decision belongs.
 */
static void abandon_epoch(mcl_rdv_t *rdv, uint32_t now)
{
    (void)mcl_contact_abandon_migration(mcl_node_get_contact(rdv->node));
    rdv->migration_ref = 0u;
    rdv->offered_transport = 0u;
    rdv->offered_profile = 0u;
    rdv->session_ref = 0u;
    rdv->peer_endpoint_token = 0u;
    rdv->local_endpoint_token = 0u;
    rdv->has_pending_accept = 0u;
    rdv->has_pending_challenge = 0u;
    rdv->policy_raised = 0u;
    rdv->admitted = 0u;
    rdv->is_controller = 0u;
    rdv->peer_seen = 0u;
    rdv->peer_ref = 0u;
    rdv->retries = 0u;
    rdv->bearer_index = 0u;
    rdv->offer_retries = 0u;
    rdv->state = MCL_RDV_STATE_ANNOUNCING;
    rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
}

static void stage_awaiting_challenge(mcl_rdv_t *rdv, uint32_t now)
{
    if (rdv->is_controller) {
        return;         /* the controller drives; it does not wait here */
    }
    if (!elapsed(now, rdv->deadline_ms)) {
        return;
    }
    abandon_epoch(rdv, now);
}

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
    case MCL_RDV_STATE_SOLICITING: stage_soliciting(rdv, now); break;
    case MCL_RDV_STATE_HEARD:      stage_heard(rdv, now);      break;
    case MCL_RDV_STATE_OFFERING:   stage_offering(rdv, now);   break;
    case MCL_RDV_STATE_ACCEPTING:  stage_accepting(rdv, now);  break;
    case MCL_RDV_STATE_AGREED:
    case MCL_RDV_STATE_CANDIDATE_PENDING:
        stage_awaiting_challenge(rdv, now);
        break;
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
        /*
         * ONLY A MACHINE THAT HAS NOT YET ANNOUNCED BECOMES A RESPONDER.
         *
         * ANNOUNCING is precisely "this epoch's PRESENCE is not out yet", so
         * hearing one here means somebody else got there first: cancel the
         * pending announcement and answer them.
         *
         * SOLICITING is deliberately absent, and it is the whole correction.
         * A machine whose own PRESENCE is on the air owns the round; taking
         * the responder role as well is how every node ended up an acceptor
         * and none ever became a controller. HEARD and OFFERING are absent
         * too: they are already responding to somebody, and abandoning that to
         * chase a newer solicitation would make the two machines that DID pair
         * up wait out a timeout for nothing.
         */
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
         * AN OFFER IS A RESPONSE, SO ONLY THE SOLICITOR CONSUMES IT.
         *
         * This used to be handled in ANNOUNCING, HEARD and OFFERING as well,
         * which is what let one machine be offerer and acceptor at the same
         * instant. With two machines the roles happen to be complementary and
         * nothing shows. With three it produced a STABLE cycle -- A holding B
         * as its peer, B holding C, C holding A -- in which every node was an
         * acceptor and none ever became a controller, so no handoff was ever
         * driven. Six hundred simulated seconds of it: thirty-six bearer
         * agreements, zero path validations, zero migrations, and it never
         * broke on its own.
         *
         * HEARD and OFFERING consume nothing now. They are already answering
         * somebody else's solicitation, and a responder that accepted a fellow
         * responder's offer would be forming a contact inside a round that
         * neither of them owns.
         *
         * The offer-collision tiebreaker that used to run in OFFERING is gone
         * with them, and not because glare stopped mattering: it cannot occur.
         * Only a responder emits an offer and only the solicitor consumes one,
         * so there is no state in which two machines have offers outstanding
         * to each other. mcl_contact_resolve_offer_collision still defines the
         * rule for a caller driving mcl_contact_t directly; this layer no
         * longer reaches a position where it needs it.
         */

        /*
         * A REPEATED OFFER OF A TRANSACTION WE ALREADY ACCEPTED IS ANSWERED
         * AGAIN.
         *
         * The acceptor answered once and then left the accepting states, so if
         * that TRANSPORT_ACCEPT was lost -- to a collision, on the medium this
         * profile is defined for -- the offerer retransmitted its offer into
         * silence, exhausted its bearers and gave up, while the acceptor sat
         * waiting for a PATH_CHALLENGE that could never come. Both machines
         * behaved correctly and the rendezvous still failed.
         *
         * The handoff controls already had this property: a duplicate is
         * answered idempotently. TRANSPORT_ACCEPT is the same kind of reply
         * and needs the same rule. The stored acceptance is re-armed, with a
         * fresh backoff so two acceptors do not answer a retransmission
         * together, and nothing else about the transaction changes.
         */
        if (rdv->peer_seen &&
            rdv->migration_ref != 0u &&
            object.body.transport_offer.migration_ref == rdv->migration_ref &&
            (rdv->state == MCL_RDV_STATE_ACCEPTING ||
             rdv->state == MCL_RDV_STATE_AGREED ||
             rdv->state == MCL_RDV_STATE_CANDIDATE_PENDING)) {
            /*
             * TWO RESPONDERS CARRYING ONE REFERENCE: ABORT THE ROUND.
             *
             * migration_ref is what selects a responder out of several, so an
             * offer bearing the reference this node has already selected but a
             * DIFFERENT source is a transaction it cannot name unambiguously.
             * Sending the acceptance anyway tells both machines they were
             * chosen, and the disagreement surfaces later, on the candidate
             * bearer, as two peers driving one contact.
             *
             * This is reachable rather than theoretical. Two builders may
             * legally hold the same source_ref (wire.h assigns it no
             * uniqueness property), so a migration_ref derived from source_ref
             * plus a local counter collides exactly as easily -- which is why
             * fresh_migration_ref() draws from platform.random on a shared
             * medium. Randomness makes it rare; it does not make it
             * impossible, and rare is the condition that must still be safe.
             *
             * Abandoning costs one round. Silently selecting an ambiguous
             * transaction costs a contact that its two ends disagree about.
             */
            if (object.source_ref != rdv->peer_ref) {
                abandon_epoch(rdv, now);
                break;
            }
            if (rdv->has_pending_accept == 0u &&
                object.body.transport_offer.transport_id ==
                    rdv->offered_transport &&
                object.body.transport_offer.profile_id ==
                    rdv->offered_profile) {
                rdv->has_pending_accept = 1u;
                rdv->state = MCL_RDV_STATE_ACCEPTING;
                rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
            }
            break;
        }

        /*
         * migration_ref ZERO NAMES NO TRANSACTION, SO IT CANNOT BE SELECTED.
         *
         * Zero is the reserved "no transaction" value. Adopting it would make
         * this node send an acceptance that selects nobody -- the responder's
         * own match requires a non-zero outstanding reference, so it would not
         * recognise the answer to its own offer -- and the round would be spent
         * on a transaction that cannot complete. A bootstrap bearer is public
         * and anything at all arrives on it, so this is refused rather than
         * assumed away.
         */
        if (object.body.transport_offer.migration_ref == 0u) {
            break;
        }
        if (rdv->state == MCL_RDV_STATE_SOLICITING) {
            uint8_t i;

            /*
             * Whether to accept is not decided here beyond one question the
             * SDK can answer: is the offered bearer one this deployment
             * mandates? A bearer outside the deployment's set is refused by
             * silence, because accepting it would put the contact somewhere
             * the deployment profile does not describe.
             *
             * The FIRST valid offer wins and the selection is then frozen for
             * the epoch: later contenders fall through to nothing, because
             * this node is no longer SOLICITING by the time they arrive.
             */
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

                    /* The responder is bound HERE, from the offer. A solicitor
                       has heard no PRESENCE and holds no peer until one of the
                       contenders answers it. */
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
                    /*
                     * ZERO MEANS THE INTEGRATOR REFUSED TO ALLOCATE.
                     *
                     * Sending it would offer a session reference that
                     * mcl_contact_agree rejects, and this path used to
                     * discard that rejection and carry on -- so the two ends
                     * disagreed about whether a contact existed. Nothing is
                     * sent instead: silence is what an unanswered
                     * solicitation already means, and the offering peer
                     * retries and eventually reports NO_COMMON_BEARER, which
                     * is the truth from its side.
                     */
                    if (reply.body.transport_accept.session_ref == 0u) {
                        rdv->migration_ref = 0u;
                        rdv->peer_seen = 0u;
                        rdv->peer_ref = 0u;
                        break;
                    }
                    /*
                     * THE ACCEPTANCE CONTENDS FOR THE MEDIUM LIKE EVERYTHING
                     * ELSE.
                     *
                     * It was emitted right here, inside receive handling, the
                     * instant the offer decoded -- which is precisely the
                     * behaviour the contention rule forbids, and precisely the
                     * behaviour that guarantees a collision when two machines
                     * answer the same offer. AP-BOOTSTRAP-1 requires a machine
                     * intending to transmit to pick a random slot and sense
                     * first, and "intending to transmit" does not have an
                     * exception for replies.
                     *
                     * So it is prepared here and SENT by stage_accepting(),
                     * after a backoff and only into an idle medium. The local
                     * bookkeeping below stays inline: it is this node's own
                     * state, and delaying it would leave a window in which a
                     * second contender's offer looked like a fresh round.
                     */
                    rdv->pending_accept = reply;
                    rdv->has_pending_accept = 1u;

                    rdv->session_ref = reply.body.transport_accept.session_ref;
                    rdv->peer_endpoint_token =
                        object.body.transport_offer.endpoint_token;
                    rdv->offered_transport =
                        object.body.transport_offer.transport_id;
                    rdv->offered_profile =
                        object.body.transport_offer.profile_id;
                    /* We accepted somebody else's offer, so they control the
                       migration. The solicitor selects the responder; the
                       responder drives the handoff. */
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
                    rdv->state = MCL_RDV_STATE_ACCEPTING;
                    rdv->deadline_ms = now + mcl_rdv_reply_delay_ms(rdv);
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
        /*
         * THE OFFER WAS BROADCAST, SO THE RESPONDER IS WHOEVER ANSWERS.
         *
         * This also required object.source_ref == rdv->peer_ref, which quietly
         * assumed the offer had been addressed to somebody. It had not:
         * TRANSPORT_OFFER goes out on a shared medium with no destination
         * field, and any machine that hears it and supports the bearer may
         * accept. peer_ref meanwhile tracks the last PRESENCE heard, so with
         * three machines in a room the responder was usually NOT the node
         * peer_ref happened to name, and its acceptance was discarded.
         *
         * Measured over ten simulated minutes with three machines: thirty-six
         * bearer agreements, ZERO path validations, zero migrations. Every
         * agreement was on an acceptor and no offerer ever recognised the
         * answer to its own offer, so no node ever became a controller and
         * nothing could drive a handoff. The cycle is stable and never breaks
         * on its own.
         *
         * migration_ref is the transaction selector -- that is what it is for,
         * and it is now drawn fresh per transaction (randomly on a shared
         * medium) rather than being the constant it once was. This is also why
         * it must be random there: a selector derived from source_ref, which
         * carries no uniqueness property, can name two responders at once.
         * The first valid acceptance of THIS transaction
         * wins, and the responder is BOUND here: peer_ref becomes whoever
         * answered. A second acceptance of the same transaction then fails the
         * state test, because this node is no longer OFFERING, and the loser
         * times out exactly as it would if its acceptance had been lost.
         *
         * That is a responder election: one opportunity, one winner, the
         * initiator selects it, and no new wire field was needed to do it.
         */
        if (rdv->state == MCL_RDV_STATE_OFFERING &&
            rdv->migration_ref != 0u &&
            object.body.transport_accept.migration_ref == rdv->migration_ref &&
            object.body.transport_accept.transport_id ==
                rdv->offered_transport &&
            object.body.transport_accept.profile_id == rdv->offered_profile) {
            /* Bind the responder. Everything after this point is scoped to it,
               including the handoff, which is why it is set before any of it. */
            rdv->peer_ref = object.source_ref;
            rdv->peer_seen = 1u;
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
        } else if (rdv->state == MCL_RDV_STATE_OFFERING) {
            /*
             * SOMEBODY ELSE WAS SELECTED. STAND DOWN AND CONTEND AGAIN.
             *
             * An acceptance is audible to every contender, and one that does
             * not name our transaction names another responder's. Waiting out
             * the response timeout anyway is not wrong -- the loser reaches
             * the same place -- but it is three offers of 787 ms each, spread
             * over eighteen seconds, spent on a round this node has already
             * lost, and every one of them is medium time taken from the pair
             * that did agree.
             *
             * This is a SUPPRESSION, not a correctness dependency: a
             * hidden-terminal contender that never hears the acceptance still
             * converges, more slowly, through its offer retries.
             *
             * bearer_index is reset with the rest of the epoch by
             * abandon_epoch(). That matters: losing the election says nothing
             * whatever about whether this bearer is supported, and advancing
             * past it would report NO_COMMON_BEARER about bearers that were
             * never refused.
             */
            abandon_epoch(rdv, now);
        }
        break;

    default:
        /* A kind this stage has no use for. Refused by doing nothing. */
        break;
    }
    return MCL_RDV_OK;
}

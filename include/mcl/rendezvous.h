/*
 * MCL rendezvous coordinator -- the bootstrap lifecycle, driven.
 *
 * WHAT THIS IS FOR, AND WHY mcl_node_t WAS NOT ENOUGH
 *
 * mcl_node_t gives a builder the pieces: encode a Tier-0 object, send a frame,
 * receive one, drive a handoff. mcl_conformance_evaluate() then tells a builder
 * whether the configuration they assembled satisfies a conformance layer.
 * Neither one MEETS A STRANGER. Between the two sits the sequence
 * V1_SCOPE.md section 5.10 actually requires:
 *
 *     mutual detection -> bootstrap acquisition -> PRESENCE exchanged
 *     -> a common richer bearer identified -> OFFER / ACCEPT
 *     -> candidate path validated -> contact migrated
 *
 * Every builder implementing that from the primitives would write the same
 * loop, and -- this is the part that matters -- would write it DIFFERENTLY.
 * Two such loops do not interoperate merely because both are conformant, which
 * is the finding that reopened v1 scope in research/TWO_BUILDER_AUDIT.md. The
 * sequence is protocol, so it belongs in the release rather than in each
 * builder's integration notes.
 *
 * THE THING THIS LAYER HAD TO CONFRONT
 *
 * "A common richer bearer identified" cannot be done by inspection at major 1.
 *
 * PRESENCE carries no capability set. machine_class was removed from major-1
 * PRESENCE on the audit evidence in V1_SCOPE.md section 4.8, and
 * capability_tag is normatively a SENDER-CONTROLLED OPAQUE REVISION TOKEN that
 * a receiver is FORBIDDEN to compare across peers (wire.h). So hearing a
 * stranger tells you that it is there and nothing whatever about what it
 * speaks.
 *
 * A common bearer is therefore found by ORDERED TRIAL, not by intersection:
 * offer the bearers this deployment mandates, in the order the deployment
 * fixes, and let TRANSPORT_ACCEPT or a timeout answer. The order comes from
 * the deployment profile, which is exactly why the deployment profile exists --
 * two independent builders converge because they were handed the same ordered
 * list, not because they exchanged one.
 *
 * This is not a workaround for a missing field. Adding a capability bitmap to
 * PRESENCE would put a bearer list in the one object that has to survive the
 * worst bearer in the system, and it would still be a claim rather than a
 * demonstration: a peer that advertises BLE and cannot be reached over BLE has
 * told you nothing useful. A trial answers the question that was actually
 * being asked, which is not "what do you support" but "can we talk there".
 *
 * HOW IT RUNS: A PUMP, NOT A THREAD
 *
 * The coordinator never blocks, never allocates, never calls back into the
 * caller from inside itself, and owns no timer. The caller drives it:
 *
 *     mcl_rdv_init(&r, &cfg, &platform, &node);
 *     for (;;) {
 *         mcl_rdv_deliver(&r, transport_id, bytes, n);   // when bytes arrive
 *         mcl_rdv_poll(&r, &event);                      // on a timer tick
 *     }
 *
 * That shape runs on an ESP32-S3 with no RTOS, inside somebody else's event
 * loop, or under a test harness with a simulated clock -- the last of which is
 * how the contention behaviour is tested without a room full of machines.
 *
 * WHAT IT DOES NOT DO
 *
 * It performs no cryptography and establishes no security property. MCL has no
 * Stable SECURITY class and no assigned feature bits at v1
 * (mcl-link/research/security-carrier-design.md), so there is nowhere legal for
 * handshake bytes to travel. MCL_RDV_EVENT_SECURITY_ESTABLISHED is declared so
 * that the event vocabulary does not change when MCL-S1 lands, and NO BUILD OF
 * THIS RELEASE EMITS IT. test_rendezvous.c asserts that.
 */

#ifndef MCL_RENDEZVOUS_H
#define MCL_RENDEZVOUS_H

#include "mcl/sdk.h"
#include "mcl/wire.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport ids are 8-bit, but a deployment's mandatory set is small and
   ordered. Sixteen is more bearers than any deployment profile the schema
   permits, and it keeps the config a fixed-size caller-owned struct. */
#define MCL_RDV_MAX_BEARERS 16u

typedef enum {
    MCL_RDV_OK = 0,
    MCL_RDV_ERR_NULL = 1,
    MCL_RDV_ERR_CONFIG = 2,
    MCL_RDV_ERR_STATE = 3,
    MCL_RDV_ERR_TRANSPORT = 4,
    MCL_RDV_ERR_DECODE = 5
} mcl_rdv_status_t;

typedef enum {
    /* Nothing has happened since the last poll. Not an error, and the common
       case: a coordinator that had something to say on every tick would be
       reporting the clock rather than the protocol. */
    MCL_RDV_EVENT_NONE = 0,
    MCL_RDV_EVENT_PEER_DISCOVERED = 1,
    MCL_RDV_EVENT_BEARER_AGREED = 2,
    MCL_RDV_EVENT_CANDIDATE_VALIDATED = 3,
    MCL_RDV_EVENT_CONTACT_MIGRATED = 4,
    /* Every mandated bearer was offered and none was accepted. A terminal,
       REPORTABLE outcome -- distinct from "still trying" and from a failure,
       because a deployment that cannot reach a peer it can hear is a fact its
       operator needs rather than a silence. */
    MCL_RDV_EVENT_NO_COMMON_BEARER = 5,
    MCL_RDV_EVENT_CONTACT_LOST = 6,
    /* The peer is reachable and the next step is not the SDK's to take:
       whether to admit this stranger is local policy. */
    MCL_RDV_EVENT_POLICY_REQUIRED = 7,
    /* Declared, never emitted by this release. See the header comment. */
    MCL_RDV_EVENT_SECURITY_ESTABLISHED = 8
} mcl_rdv_event_kind_t;

typedef enum {
    MCL_RDV_STATE_IDLE = 0,
    MCL_RDV_STATE_ANNOUNCING = 1,   /* emitting PRESENCE on the bootstrap bearer */
    MCL_RDV_STATE_HEARD = 2,        /* a peer PRESENCE has arrived */
    MCL_RDV_STATE_OFFERING = 3,     /* a TRANSPORT_OFFER is outstanding */
    MCL_RDV_STATE_AGREED = 4,       /* accepted; the candidate is not yet proven */
    MCL_RDV_STATE_VALIDATING = 5,
    MCL_RDV_STATE_MIGRATED = 6,
    MCL_RDV_STATE_EXHAUSTED = 7,    /* every mandated bearer was tried */
    MCL_RDV_STATE_CLOSED = 8
} mcl_rdv_state_t;

typedef struct {
    mcl_rdv_event_kind_t kind;
    uint8_t transport_id;    /* the bearer the event concerns, where it has one */
    uint8_t profile_id;
    uint32_t peer_ref;       /* correlation only; never identity */
    uint32_t migration_ref;
} mcl_rdv_event_t;

/*
 * Platform services. All caller-supplied, because the SDK is freestanding and
 * has no business knowing what a socket or a speaker is on this machine.
 */
typedef struct {
    /*
     * Emit bytes on a bearer. Contract identical to mcl_sdk_tx_fn, including
     * the part that matters: return > 0 when the transport CANNOT TELL whether
     * the bytes went out. Claiming a certainty it does not have is the failure
     * that sdk.h documents at length, and it reaches here unchanged.
     */
    mcl_sdk_tx_fn tx;
    /*
     * Monotonic milliseconds. Need not be wall time, need not start at zero,
     * must not go backwards. The coordinator only ever subtracts two readings,
     * so unsigned wrap at 2^32 is handled and needs no special value.
     */
    uint32_t (*now_ms)(void *user);
    /*
     * Bounded randomness. REQUIRED for any shared-medium claim.
     *
     * It was optional, with a documented fallback deriving the backoff slot
     * from source_ref. That fallback is now refused for MCL Stranger-Contact 1:
     * source_ref is a correlation reference with NO uniqueness property, so two
     * uncoordinated builders may legally choose the same one and then collide
     * on every single contention round, forever. A deterministic function of a
     * non-unique value cannot provide a multi-builder collision guarantee, and
     * describing it as scheduling was generous.
     *
     * mcl_rdv_init() refuses a configuration that claims the shared medium
     * without it. Point-to-point use on an arranged bearer may still omit it.
     */
    int (*random)(void *user, uint8_t *out, size_t size);
    /*
     * Is the shared medium busy right now? Non-zero means an MCL transmission
     * is in progress or being acquired.
     *
     * THIS IS WHAT MAKES CONTENTION WORK, AND ITS ABSENCE IS WHY THE FIRST
     * DESIGN COULD NOT.
     *
     * A backoff that only delays cannot help when the delay window is shorter
     * than the transmission it is protecting: at 300 baud a 17-byte
     * TRANSPORT_OFFER occupies 786.7 ms of air, and PRESENCE occupies 600 ms,
     * so two responders scheduled anywhere inside a 400 ms window overlap with
     * CERTAINTY rather than with some probability. Randomising harder does not
     * fix an interval that is too short by construction.
     *
     * What fixes it is deferring on a busy medium, so a machine that draws a
     * later slot yields to one already transmitting instead of talking over it.
     * `mcl_ap_listener` already distinguishes QUIET / WAITING / HEARD /
     * CONTACT, so an MCL implementation has this without inventing an energy
     * detector.
     *
     * Optional only for a bearer with no shared medium.
     */
    int (*medium_busy)(void *user);
    /*
     * Is this machine's own emitter transmitting right now?
     *
     * SELF-ECHO IS A PHYSICAL FACT AND MUST NOT BE INFERRED FROM source_ref.
     *
     * The first version discarded any object whose source_ref equalled our own.
     * But wire.h defines source_ref as a correlation reference for semantic
     * origin, explicitly not identity, with no uniqueness property assigned. So
     * two unrelated builders may legally pick the same value, and each would
     * then silently discard everything the other said -- two machines in one
     * room, both announcing, both deaf, and no diagnostic anywhere.
     *
     * Only the platform knows when its own speaker was driven. When this is
     * NULL the coordinator falls back to the source_ref comparison, which is
     * defence in depth and NOT a discrimination mechanism; a shared-medium
     * claim requires the callback.
     */
    int (*self_transmitting)(void *user);
    void *user;
} mcl_rdv_platform_t;

typedef struct {
    uint32_t source_ref;
    /*
     * The bearer first contact happens on. A deployment names it; there is no
     * default, because the bearer that needs no prior arrangement is a
     * deployment decision and silently picking one would be the SDK deciding
     * what the deployment is.
     */
    uint8_t bootstrap_transport_id;
    /*
     * The mandated bearers, IN THE ORDER THEY ARE OFFERED. This ordering is
     * the whole mechanism by which two builders who never coordinate converge,
     * so it is taken from the deployment profile and never sorted, reordered
     * or de-duplicated here.
     */
    uint8_t bearer_count;
    uint8_t bearer_transport_id[MCL_RDV_MAX_BEARERS];
    uint8_t bearer_profile_id[MCL_RDV_MAX_BEARERS];
    /*
     * This machine's own reachability hint on each bearer, or 0 for none.
     *
     * It goes into TRANSPORT_OFFER.endpoint_token. The first version hardcoded
     * zero and left a comment saying a builder "sets it on the object before it
     * goes out" -- which the builder could not do, because the object is built
     * and emitted inside one function and never surfaces in between. The hook
     * has to be in the configuration, where the integrator actually is.
     *
     * Zero remains legal and means "reach me by the profile's own discovery".
     */
    uint32_t bearer_endpoint_token[MCL_RDV_MAX_BEARERS];
    mcl_contact_role_t role;
    /*
     * Does this deployment run on a SHARED MEDIUM?
     *
     * When set, mcl_rdv_init() requires platform.random and platform.medium_busy
     * and refuses the configuration without them, because the contention rules
     * in section 7 of AP-BOOTSTRAP-1 cannot be honoured without both. A
     * point-to-point bearer sets it to 0 and needs neither.
     */
    uint8_t shared_medium;
    /* Timing, all in milliseconds. Zero means "use the built-in default". */
    uint16_t announce_interval_ms;
    uint16_t response_timeout_ms;
    /*
     * Contention backoff: a slot count and a slot width.
     *
     * A machine waits a random whole number of slots, then transmits only if
     * the medium is idle. The width must exceed the acquisition time of a
     * transmission already under way -- the AP preamble is 200 ms -- so that a
     * machine drawing a later slot can SEE an earlier one and defer. It does
     * not have to exceed the whole frame, which is what makes 250 ms workable
     * where a 400 ms continuous window was not.
     */
    uint8_t backoff_slots;
    uint16_t backoff_slot_ms;
    uint8_t max_announcements;
    uint8_t max_offer_retries;
    /*
     * capability_tag this node advertises. Opaque and sender-scoped: the
     * coordinator copies it into PRESENCE and never compares one peer's tag
     * with another's, which wire.h forbids.
     */
    uint32_t capability_tag;
    uint8_t presence_ttl;
} mcl_rdv_config_t;

typedef struct {
    mcl_rdv_config_t config;
    mcl_rdv_platform_t platform;
    mcl_node_t *node;

    mcl_rdv_state_t state;
    uint8_t bearer_index;        /* next bearer to offer */
    uint8_t announcements;
    uint8_t offer_retries;
    uint8_t peer_seen;
    uint32_t peer_ref;
    uint32_t migration_ref;
    /*
     * Every transaction gets a FRESH migration_ref, and the previous one is
     * remembered so a regenerated value cannot repeat it.
     *
     * The first version computed `source_ref | 1`, which is constant for the
     * life of the node: abandoning a bearer reset it to zero and the next
     * transaction regenerated the identical value. wire.h says migration_ref
     * "correlates one transport-change transaction, and nothing else", and the
     * coordinator's own stale-ACCEPT check depends on that -- so it was relying
     * on a property it had just broken.
     */
    uint32_t last_migration_ref;
    uint32_t transaction_counter;
    /* What we actually offered, so an acceptance can be matched against it. */
    uint8_t offered_transport;
    uint8_t offered_profile;
    uint32_t session_ref;
    uint8_t challenge[MCL_CONTACT_CHALLENGE_SIZE];
    uint8_t is_controller;       /* we sent the offer that was accepted */
    uint32_t deadline_ms;        /* next scheduled action */
    uint32_t last_event_ms;
    mcl_rdv_event_t pending;
    uint8_t has_pending;
} mcl_rdv_t;

/*
 * Initialise. `node` is caller-owned and must already be mcl_node_init'd; the
 * coordinator borrows it and never frees or reinitialises it.
 */
mcl_rdv_status_t mcl_rdv_init(mcl_rdv_t *rdv,
                              const mcl_rdv_config_t *config,
                              const mcl_rdv_platform_t *platform,
                              mcl_node_t *node);

/* Begin announcing. Legal only from IDLE. */
mcl_rdv_status_t mcl_rdv_start(mcl_rdv_t *rdv);

/*
 * Advance the machine. Call on a timer; the interval is not critical because
 * every decision is taken against now_ms() rather than against a tick count.
 * Fills `out` with MCL_RDV_EVENT_NONE when there is nothing to report.
 */
mcl_rdv_status_t mcl_rdv_poll(mcl_rdv_t *rdv, mcl_rdv_event_t *out);

/*
 * Hand the coordinator bytes that arrived on a bearer. The bytes are a raw
 * Wire Tier-0 object -- what the acoustic bootstrap carries. Objects that do
 * not decode, or that decode to a kind this stage does not expect, are
 * REFUSED AND NOT ACTED ON, and refusing is not an error: a bootstrap bearer
 * is public, and anything at all can arrive on it.
 */
mcl_rdv_status_t mcl_rdv_deliver(mcl_rdv_t *rdv,
                                 uint8_t transport_id,
                                 const uint8_t *data,
                                 size_t size);

/* Current state, for a caller that wants to display it. */
mcl_rdv_state_t mcl_rdv_state(const mcl_rdv_t *rdv);

/*
 * The backoff this node waits before transmitting into a contended medium.
 *
 * CONTENTION IS PROTOCOL, NOT MODULATION. Ten machines that answer one
 * PRESENCE at once defeat a perfect modem: the replies overlap, every one is
 * lost, and the modem's error structure has nothing to do with it.
 *
 * THE FIRST DESIGN OF THIS FUNCTION WAS ARITHMETICALLY UNABLE TO WORK.
 *
 * It drew a delay uniformly from a 400 ms window. At 300 baud a 17-byte
 * TRANSPORT_OFFER occupies 786.7 ms of air and a 10-byte PRESENCE occupies
 * 600 ms, so two responders placed anywhere in that window overlap with
 * CERTAINTY. That is not a high collision probability to be improved by better
 * randomness; it is a window shorter than the thing it was protecting.
 *
 * What replaces it is a slotted backoff with deferral: draw a whole number of
 * slots, and at slot expiry transmit only if platform.medium_busy says the
 * medium is idle. The slot width need only exceed the acquisition time of a
 * transmission already in progress -- the AP preamble is 200 ms -- because a
 * machine that draws a later slot can then SEE an earlier one and yield. That
 * is why 250 ms works where a 400 ms continuous window could not.
 *
 * Randomness is required rather than derived. source_ref carries no uniqueness
 * property, so two builders may legally share one and collide on every round.
 *
 * Exposed because it is testable: a simulated clock, a stub random source and
 * an airtime-aware medium model measure the collision rate with no hardware.
 * Not const: drawing a slot consumes randomness, which is a state change.
 */
uint16_t mcl_rdv_reply_delay_ms(mcl_rdv_t *rdv);

#ifdef __cplusplus
}
#endif

#endif /* MCL_RENDEZVOUS_H */

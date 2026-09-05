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
     * Bounded randomness for reply scheduling. Optional: when NULL the
     * coordinator schedules replies deterministically from source_ref, which
     * still separates two peers but not three. See mcl_rdv_reply_delay_ms().
     */
    int (*random)(void *user, uint8_t *out, size_t size);
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
    mcl_contact_role_t role;
    /* Timing, all in milliseconds. Zero means "use the built-in default". */
    uint16_t announce_interval_ms;
    uint16_t response_timeout_ms;
    uint16_t reply_slot_ms;
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
 * The delay this node waits before replying to a PRESENCE it heard.
 *
 * CONTENTION IS PROTOCOL, NOT MODULATION. Ten machines that answer one
 * PRESENCE immediately defeat a perfect modem: the replies collide, every one
 * of them is lost, and the modem's error structure has nothing to do with it.
 * So a reply is placed in a slot rather than sent at once.
 *
 * The slot is drawn from platform.random when it is available. When it is not,
 * it is derived from source_ref, which separates two peers reliably and three
 * only by luck -- stated here rather than left for a builder to discover with
 * three machines in a room.
 *
 * Exposed because it is testable: a simulated clock and a stub random source
 * can measure the collision rate without any hardware.
 */
uint16_t mcl_rdv_reply_delay_ms(const mcl_rdv_t *rdv);

#ifdef __cplusplus
}
#endif

#endif /* MCL_RENDEZVOUS_H */

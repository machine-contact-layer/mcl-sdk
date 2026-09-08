#ifndef MCL_MACHINE_H
#define MCL_MACHINE_H

/*
 * THE INTEGRATION SURFACE.
 *
 * Everything below this header is MCL's problem. Everything above it is the
 * machine's. That sentence is the entire design brief.
 *
 * WHY THIS EXISTS WHEN mcl/rendezvous.h ALREADY DID THE HARD PART
 *
 * `mcl_rdv_*` is the protocol: solicitation epochs, contention, ordered bearer
 * trial, path validation, irrevocable COMMIT, idempotent retransmission. It is
 * correct and it is not what an integrator wants to hold.
 *
 * Wiring it up means initialising a node AND a coordinator, keeping their two
 * configurations consistent, filling in eight platform operations, driving a
 * poll loop, routing received bytes by transport, and -- the part every builder
 * would have written differently -- noticing `BEARER_AGREED` and opening the
 * candidate bearer before the coordinator will emit anything on it. The
 * reference node in `hardware/dfr1154-autonomous-node` did all of that, and the
 * shape of what it had to do is the argument for this file.
 *
 * So this header asks for what only the machine knows -- a clock, randomness, a
 * way to put bytes on a medium, a way to open a candidate bearer -- and answers
 * with what the machine actually wants to know:
 *
 *     a peer is here, and the contact is established
 *     the contact is gone
 *     something needs a policy decision that is not mine to take
 *
 * MCL keeps PRESENCE, contention, OFFER/ACCEPT, activation, PATH_CHALLENGE /
 * PATH_RESPONSE, retries, duplicate handling, COMMIT / CONFIRM and session
 * continuity. The integrator provides hardware operations, not protocol
 * choreography.
 *
 * WHAT IT DOES NOT DO, AND WILL NOT
 *
 *   - It does not decide whether to admit a stranger. Reception is not
 *     identity, is not authority and is not obligation; that decision is the
 *     deployment's and this header will not take it silently.
 *   - It provides no security. There is no cryptography anywhere in MCL v1.
 *     A machine that completes this sequence has established REACHABILITY and
 *     CORRELATION, and nothing else. Anything in range can complete it.
 *   - It hides no refusal. Where the protocol refuses, this reports the
 *     refusal; a facade that turned a refusal into a retry would be lying on
 *     behalf of the layer it wraps.
 *
 * THE NUMBER OF OPERATIONS IS THE POINT
 *
 * Eight for the reference deployment, and two of those are optional. If
 * porting MCL to a new machine ever
 * needs fifty, this facade is not finished. See `mcl_platform_t`.
 */

#include <stddef.h>
#include <stdint.h>

#include "mcl/rendezvous.h"
#include "mcl/sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ status */

typedef enum {
    MCL_MACHINE_OK = 0,
    MCL_MACHINE_ERR_NULL = 1,
    MCL_MACHINE_ERR_CONFIG = 2,
    MCL_MACHINE_ERR_STATE = 3
} mcl_machine_status_t;

/* ------------------------------------------------------------------ events */

typedef enum {
    /* Nothing happened. The common case; a poll loop is not a fault log. */
    MCL_MACHINE_EVENT_NONE = 0,
    /*
     * Something is out there. Correlation only -- `peer_ref` is a source_ref,
     * which wire.h defines as having no uniqueness property and no identity
     * meaning whatsoever.
     */
    MCL_MACHINE_EVENT_PEER_DETECTED = 1,
    /*
     * Raised ONLY when `mcl_platform_t::policy_admit` is NULL. The peer is
     * reachable and whether to admit it is the deployment's decision; answer
     * with mcl_machine_admit() or mcl_machine_refuse().
     *
     * This sits before the first irrevocable act on each side, because a
     * refusal after commitment is not a refusal, it is a broken contact.
     */
    MCL_MACHINE_EVENT_POLICY_REQUIRED = 2,
    /*
     * A contact exists on the continuation bearer, validated and migrated.
     * This is the event an application waits for.
     */
    MCL_MACHINE_EVENT_CONTACT_ESTABLISHED = 3,
    MCL_MACHINE_EVENT_CONTACT_LOST = 4,
    /*
     * Every bearer this deployment mandates was offered and none was accepted.
     * A terminal, REPORTABLE outcome and not a failure: a machine that can hear
     * a peer it cannot reach is a fact its operator needs, not a silence.
     */
    MCL_MACHINE_EVENT_NO_COMMON_BEARER = 5,
    /*
     * The machine could not proceed for a reason that is not the protocol's:
     * a candidate bearer that would not open, a platform operation that
     * refused. `status` carries what happened.
     */
    MCL_MACHINE_EVENT_ERROR = 6
} mcl_machine_event_kind_t;

typedef struct {
    mcl_machine_event_kind_t kind;
    uint8_t  transport_id;   /* the bearer this event concerns, where it has one */
    uint8_t  profile_id;
    /* Correlation reference for the peer. NEVER an identity. */
    uint32_t peer_ref;
    /* Contact-lifetime session, non-zero once a contact exists. */
    uint32_t session_ref;
    /* Set on MCL_MACHINE_EVENT_ERROR. */
    mcl_machine_status_t status;
} mcl_machine_event_t;

/* ----------------------------------------------------------- the platform */

/*
 * What `candidate_open` reports.
 *
 * Opening a bearer is not instantaneous anywhere real: a BLE central has to
 * scan, find a beacon and connect; a socket has to be created. The middle
 * answer exists so a machine does not have to lie about which of the other two
 * it is.
 */
typedef enum {
    MCL_MACHINE_CANDIDATE_READY = 0,    /* usable now */
    MCL_MACHINE_CANDIDATE_PENDING = 1,  /* opening; call mcl_machine_candidate_ready() */
    MCL_MACHINE_CANDIDATE_REFUSED = 2   /* cannot be opened at all */
} mcl_machine_candidate_t;

/*
 * THE PORTING INTERFACE. Implement these and MCL runs on your machine.
 *
 * Six required operations and two optional ones for the reference deployment.
 * Nothing here knows what a
 * socket, a speaker or a radio is, and nothing here is protocol: no operation
 * in this struct builds, parses, sequences or retransmits anything.
 */
typedef struct {
    /*
     * REQUIRED. Monotonic milliseconds. Need not be wall time, need not start
     * at zero, must not go backwards. Only ever used as a difference, so
     * unsigned wrap at 2^32 is handled and needs no special value.
     */
    uint32_t (*clock_ms)(void *user);

    /*
     * REQUIRED on a shared bootstrap medium. Bounded randomness; return 0 on
     * success.
     *
     * Not optional and not derivable. Contention backoff derived from
     * `source_ref` is what two builders who legally chose the same one collide
     * on forever, and `source_ref` is defined to carry no uniqueness property.
     */
    int (*random_bytes)(void *user, uint8_t *out, size_t size);

    /*
     * REQUIRED. Put bytes on a medium.
     *
     * Return 0 when transmitted, NEGATIVE when definitely not transmitted, and
     * POSITIVE when the transport cannot tell. The three-way answer is load
     * bearing: it is what lets MCL decide whether a COMMIT it sent was
     * irrevocable. Claiming a certainty the hardware does not have is the
     * failure this contract exists to prevent.
     */
    int32_t (*transport_send)(void *user, uint8_t transport_id,
                              const uint8_t *data, size_t size);

    /*
     * REQUIRED on a shared bootstrap medium. Is the medium busy right now?
     *
     * A backoff that only delays cannot separate transmissions longer than the
     * delay window -- at 300 baud a 17-byte object occupies 786 ms of air, so
     * two machines scheduled anywhere inside a 400 ms window overlap with
     * certainty. What works is DEFERRING on a busy medium, and only the
     * platform can see whether the medium is busy.
     */
    int (*medium_busy)(void *user);

    /*
     * REQUIRED on a shared bootstrap medium. Is this machine's own emitter
     * transmitting right now?
     *
     * Self-echo is a physical fact and must not be inferred from `source_ref`:
     * two unrelated builders may legally choose the same one, and a machine
     * that discarded everything carrying "its own" reference would be deaf to
     * exactly one peer, silently.
     */
    int (*self_transmitting)(void *user);

    /*
     * REQUIRED to continue past first contact. Make the agreed bearer usable.
     *
     * `peer_endpoint_token` is where to reach the peer, as it arrived on the
     * air. IT IS ZERO ON THE OFFERING SIDE and that is not a bug:
     * `TRANSPORT_OFFER` carries a token and `TRANSPORT_ACCEPT` does not, so the
     * acceptor learns where to reach the offerer and the offerer learns
     * nothing. That asymmetry IS the role assignment under `BLE-ACTIVATE-1`:
     * the peer that can be found advertises, the peer that knows the token
     * scans. Nothing chooses.
     *
     * `local_endpoint_token` is this machine's own token for the transaction,
     * or zero if none was minted -- what a BLE offerer must advertise.
     *
     * Return PENDING and call mcl_machine_candidate_ready() when the bearer is
     * up; nothing is emitted on it before that call.
     */
    mcl_machine_candidate_t (*candidate_open)(void *user,
                                              uint8_t transport_id,
                                              uint8_t profile_id,
                                              uint32_t peer_endpoint_token,
                                              uint32_t local_endpoint_token);

    /*
     * OPTIONAL. Release a bearer opened by `candidate_open`. Called when a
     * contact ends or a candidate is abandoned.
     */
    void (*candidate_close)(void *user, uint8_t transport_id);

    /*
     * OPTIONAL. Local admission policy: return non-zero to admit.
     *
     * When NULL, MCL_MACHINE_EVENT_POLICY_REQUIRED is raised instead and the
     * caller answers with mcl_machine_admit() or mcl_machine_refuse(). There is
     * deliberately no third option in which MCL decides: a layer that admitted
     * strangers on the integrator's behalf would be asserting that reception
     * implies authority, which is the one thing this project says it does not.
     */
    int (*policy_admit)(void *user, uint32_t peer_ref, uint8_t transport_id);

    void *user;
} mcl_platform_v1_t;

/* Source-compatible name retained for the v1 line. New ports should spell the
   version explicitly so a future platform contract cannot change silently. */
typedef mcl_platform_v1_t mcl_platform_t;

/* ----------------------------------------------------------- the machine */

/*
 * A named deployment, so a builder does not choose among equivalent
 * combinations before they have seen MCL work once.
 */
typedef enum {
    /*
     * MCL-REFERENCE-DEPLOYMENT-1, as published in
     * mcl-core/deployments/MCL-REFERENCE-DEPLOYMENT-1.json:
     *
     *   Wire 1 + Link 1 + MCL Stranger-Contact 1
     *   bootstrap  AP-BOOTSTRAP-1 on transport 1 (acoustic)
     *   mandatory  BLE-GATT profile 1 on transport 3
     *   optional   IP-DATAGRAM profile 1 on transport 2
     *   security   none, and v1 has none to offer
     *
     * `tools/check-reference-deployment.sh` fails the build if this constant
     * and that file ever disagree.
     */
    MCL_DEPLOYMENT_REFERENCE_1 = 1
} mcl_deployment_t;

typedef struct {
    /*
     * This machine's correlation reference. NOT an identity, no uniqueness
     * property, and a peer must never treat it as proof of who sent anything.
     * Randomise it rather than burning a constant into a product line.
     */
    uint32_t source_ref;
    mcl_contact_role_t role;     /* ordering only; confers no authority */
    /*
     * An opaque, sender-scoped revision token for what this machine
     * advertises. It must change when the advertised capabilities change and
     * must never be compared across peers.
     */
    uint32_t capability_tag;
    uint8_t presence_ttl;

    /* Filled by mcl_machine_config_deployment(). Set by hand only for a
       deployment that is not one of the named ones. */
    uint8_t bootstrap_transport_id;
    uint8_t bearer_count;
    uint8_t bearer_transport_id[MCL_RDV_MAX_BEARERS];
    uint8_t bearer_profile_id[MCL_RDV_MAX_BEARERS];
    uint8_t shared_medium;
    /* For logs and conformance declarations. Not interpreted. */
    const char *deployment_profile_id;
} mcl_machine_config_t;

typedef struct {
    mcl_node_t node;
    mcl_rdv_t rdv;
    mcl_platform_t platform;
    mcl_machine_config_t config;
    uint32_t local_endpoint_token;   /* minted for the current transaction */
    uint8_t started;
    uint8_t candidate_open_transport;
    uint8_t awaiting_candidate;
} mcl_machine_t;

/*
 * Fill a configuration from a named deployment profile. `source_ref` and
 * `role` are yours; everything else comes from the profile.
 */
mcl_machine_status_t mcl_machine_config_deployment(mcl_machine_config_t *config,
                                                   mcl_deployment_t deployment,
                                                   uint32_t source_ref,
                                                   mcl_contact_role_t role);

mcl_machine_status_t mcl_machine_init(mcl_machine_t *machine,
                                      const mcl_machine_config_t *config,
                                      const mcl_platform_t *platform);

/* Begin looking for peers. */
mcl_machine_status_t mcl_machine_start(mcl_machine_t *machine);

/*
 * Advance the machine and report at most one event. Call it from a regular
 * timer and again promptly after receive/candidate/policy input. A late poll
 * directly delays scheduled transmission and timeout handling; the reference
 * host and embedded integrations use a 10 ms service cadence. This is not a
 * blocking call and it does not sleep.
 */
mcl_machine_status_t mcl_machine_poll(mcl_machine_t *machine,
                                      mcl_machine_event_t *out);

/*
 * Hand MCL bytes that arrived on a transport. Undecodable bytes are not an
 * error: a public medium carries everything, and a hard failure for noise
 * would make a caller treat a passing car as a fault.
 */
mcl_machine_status_t mcl_machine_receive(mcl_machine_t *machine,
                                         uint8_t transport_id,
                                         const uint8_t *data,
                                         size_t size);

/* The bearer `candidate_open` reported PENDING for is now usable. */
mcl_machine_status_t mcl_machine_candidate_ready(mcl_machine_t *machine);

/* Answers to MCL_MACHINE_EVENT_POLICY_REQUIRED. */
mcl_machine_status_t mcl_machine_admit(mcl_machine_t *machine);
mcl_machine_status_t mcl_machine_refuse(mcl_machine_t *machine);

/* For a display or a log. Never a control input. */
const char *mcl_machine_state_name(const mcl_machine_t *machine);
const char *mcl_machine_event_name(mcl_machine_event_kind_t kind);

#ifdef __cplusplus
}
#endif

#endif /* MCL_MACHINE_H */

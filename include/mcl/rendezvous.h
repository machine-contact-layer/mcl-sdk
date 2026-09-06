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

/* Retransmissions of one handoff control before the contact is given up.
   The controls are idempotent, so a retry is safe; an unbounded retry is not
   a protocol, it is a stuck node that never reports. */
#define MCL_RDV_MAX_HANDOFF_RETRIES 4u

/*
 * AP-BOOTSTRAP-1 RENDEZVOUS PARAMETERS. THESE ARE PROTOCOL, NOT PREFERENCE.
 *
 * These values were builder configuration, with defaults, and arbitrary
 * non-zero values were accepted. That is the two-builder failure this whole
 * layer exists to remove, in a new place: Builder A at 8 slots of 200 ms and
 * Builder B at 32 slots of 500 ms both compile, both pass their own tests, and
 * implement different medium-access behaviour. Nothing on the wire tells
 * either of them.
 *
 * So for a Stranger-Contact claim they come from the PROFILE. A configuration
 * may leave them zero, or state them and match; anything else is refused by
 * mcl_rdv_init(). Research that needs other values says so explicitly, with
 * mcl_rdv_config_t::parameters, and gives up the claim.
 */
#define MCL_RDV_AP1_BACKOFF_SLOTS         16u
#define MCL_RDV_AP1_BACKOFF_SLOT_MS      250u
#define MCL_RDV_AP1_ANNOUNCE_INTERVAL_MS 1500u
#define MCL_RDV_AP1_MAX_ANNOUNCEMENTS     10u
#define MCL_RDV_AP1_MAX_OFFER_RETRIES      2u

/*
 * THE RESPONSE TIMEOUT IS DERIVED, NOT CHOSEN.
 *
 * It was 3000 ms, and once every transmission obeys the contention rule that
 * number is not merely tight, it is IMPOSSIBLE: the legal maximum backoff
 * alone is 15 x 250 = 3750 ms, so a peer behaving perfectly could be declared
 * silent before it was permitted to answer.
 *
 * Replacing one guessed constant with another would leave the same defect. It
 * comes from the profile's own numbers:
 *
 *   maximum contention delay   (16 - 1) x 250 ms        = 3750 ms
 *   one deferral               a maximum frame's air    =  787 ms
 *   the response itself        a maximum frame's air    =  787 ms
 *   scheduling allowance                                =  500 ms
 *                                                        --------
 *                                                         5824 ms
 *
 * 787 ms is the airtime of a 17-byte TRANSPORT_OFFER at 300 baud, 160 samples
 * per symbol, 48 kHz, with a 0.2 s preamble -- the largest Tier-0 object at
 * major 1, so it bounds every response. The deferral term is there because
 * sensing is mandatory: a node whose chosen slot is busy waits for the medium,
 * and the longest thing it can be waiting for is one maximum frame.
 *
 * Frozen at 6000 ms, the next round number above the sum. If the waveform
 * changes, this is recomputed rather than adjusted.
 */
#define MCL_RDV_AP1_RESPONSE_TIMEOUT_MS 6000u

/*
 * How long a SOLICITOR waits for the first response to its PRESENCE.
 *
 * Numerically identical to the response timeout today, and DELIBERATELY A
 * SEPARATE NAME. They answer different questions -- "how long until a
 * responder can have answered my solicitation" and "how long until my offer
 * can have been accepted" -- and they are equal only because at AP-BOOTSTRAP-1
 * the bound on both is the same worst-case backoff plus one maximum frame.
 *
 * Sharing one constant would mean a future waveform change to one silently
 * moved the other, which is exactly the class of coupling that put a 3000 ms
 * response timeout against a 3750 ms legal backoff in the first place.
 */
#define MCL_RDV_AP1_SOLICIT_TIMEOUT_MS 6000u

typedef enum {
    /* The parameters above. The default, and what a zeroed config gets. */
    MCL_RDV_PARAMETERS_PROFILE = 0,
    /*
     * Arbitrary values, for research. NOT INTEROPERABLE and not a
     * Stranger-Contact claim: a peer built to the profile will use the
     * profile's numbers and cannot discover yours.
     */
    MCL_RDV_PARAMETERS_EXPERIMENTAL = 1
} mcl_rdv_parameters_t;

typedef enum {
    MCL_RDV_OK = 0,
    MCL_RDV_ERR_NULL = 1,
    MCL_RDV_ERR_CONFIG = 2,
    MCL_RDV_ERR_STATE = 3,
    /* The transport DEFINITELY did not send. State must not advance. */
    MCL_RDV_ERR_TRANSPORT = 4,
    MCL_RDV_ERR_DECODE = 5,
    /*
     * THE TRANSPORT CANNOT TELL WHETHER THE BYTES WENT OUT.
     *
     * mcl_sdk_tx_fn defines this third outcome deliberately -- returning > 0
     * rather than claiming a certainty the hardware does not have -- and this
     * layer collapsed it into MCL_RDV_OK, so "definitely not sent" and
     * "possibly sent" became the same thing one level above the place that
     * took care to separate them.
     *
     * They must not be the same thing here either. A definite refusal means
     * the peer has nothing, so retrying is free. An uncertain send means the
     * peer may already have acted on it, so the only safe move is to advance
     * and let the idempotent retransmission settle it -- never to start a
     * fresh transaction over the top of one that may be live.
     */
    MCL_RDV_TX_UNCERTAIN = 6
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

/*
 * THE SOLICITATION EPOCH, AND THE ROLE CONFUSION IT REPLACED.
 *
 * There used to be no notion of WHOSE round this is. A node stayed in
 * ANNOUNCING after transmitting PRESENCE, and TRANSPORT_OFFER was consumed in
 * ANNOUNCING, HEARD and OFFERING alike -- so every machine could be announcer,
 * responder, offerer and acceptor at the same instant. With two machines that
 * is harmless, because whichever roles they land in are complementary. With
 * three it does not converge:
 *
 *   measured, 600 s simulated, three machines, one medium:
 *     36 BEARER_AGREED, 0 CANDIDATE_VALIDATED, 0 CONTACT_MIGRATED
 *
 * Every agreement landed on an acceptor and no node ever became a controller,
 * so nothing drove a handoff. It is a stable cycle, not a race: it never
 * breaks on its own, and no amount of ACCEPT-matching repairs it, because
 * there was no distinguished party to select among the broadcast responses.
 *
 * So one machine owns each round:
 *
 *     PRESENCE  = solicitation      (only a SOLICITING node has sent one)
 *     OFFER     = contender response (only a responder sends one)
 *     ACCEPT    = responder selection, keyed on migration_ref
 *
 * A machine that has successfully emitted this epoch's PRESENCE is the
 * solicitor and does not become a responder to anyone else's PRESENCE during
 * it. A machine that hears a PRESENCE BEFORE transmitting its own cancels its
 * pending announcement and is a responder for that epoch. Two PRESENCE frames
 * in one slot collide, nobody hears either, both time out and re-announce from
 * a fresh random slot -- which is why randomising the FIRST announcement is
 * load-bearing rather than tidy.
 *
 * No wire field was added. TRANSPORT_ACCEPT already echoes migration_ref, and
 * that echo is the selection: the responder whose reference comes back is
 * chosen, the others hear a reference that is not theirs.
 */
typedef enum {
    MCL_RDV_STATE_IDLE = 0,
    /* This machine has NOT yet emitted this epoch's PRESENCE. Still eligible
       to become a responder to somebody else's. */
    MCL_RDV_STATE_ANNOUNCING = 1,
    /* Responder: a peer PRESENCE arrived before we announced. */
    MCL_RDV_STATE_HEARD = 2,
    MCL_RDV_STATE_OFFERING = 3,     /* a TRANSPORT_OFFER is outstanding */
    MCL_RDV_STATE_AGREED = 4,       /* accepted; the candidate is not yet proven */
    MCL_RDV_STATE_VALIDATING = 5,
    MCL_RDV_STATE_MIGRATED = 6,
    MCL_RDV_STATE_EXHAUSTED = 7,    /* every mandated bearer was tried */
    MCL_RDV_STATE_CLOSED = 8,
    /*
     * The bearer is agreed and the CALLER now has to make the candidate
     * usable: resolve the peer's endpoint token, open the socket or the GATT
     * connection, apply local admission. The coordinator cannot do any of that
     * -- it has no idea what a socket is on this machine -- so it stops here
     * and waits for mcl_rdv_candidate_ready().
     *
     * Without this state the coordinator sent PATH_CHALLENGE on a bearer the
     * integrator had not been given a chance to open. The simulator did not
     * notice because its candidate bearer was routable before anybody asked.
     */
    MCL_RDV_STATE_CANDIDATE_PENDING = 9,
    /*
     * Reachability is proven and the next step is not the SDK's to take.
     * MCL_RDV_EVENT_POLICY_REQUIRED has been raised and the coordinator is
     * waiting for mcl_rdv_admit() or mcl_rdv_refuse().
     *
     * This sits before the first IRREVOCABLE act on each side -- COMMIT for
     * the controller, PATH_RESPONSE for the acceptor -- because a policy
     * refusal after commitment is not a refusal, it is a broken contact.
     */
    MCL_RDV_STATE_ADMITTING = 10,
    /* COMMIT has been sent and CONFIRM is outstanding. COMMIT is irrevocable:
       this state can only go forward or be retried, never back. */
    MCL_RDV_STATE_COMMITTING = 11,
    /*
     * An acceptance is prepared and waiting for its contention slot.
     *
     * TRANSPORT_ACCEPT used to go out inline, the instant the offer decoded.
     * That is the one behaviour guaranteed to collide when two machines answer
     * the same offer, and it contradicted the profile's own rule, which has no
     * exception for replies.
     */
    MCL_RDV_STATE_ACCEPTING = 12,
    /*
     * SOLICITING: this machine's PRESENCE is on the air and it owns the round.
     *
     * It is the ONLY state that consumes a TRANSPORT_OFFER. It takes the first
     * valid offer of a bearer this deployment mandates, binds that responder,
     * and ignores the rest of the epoch's contenders. HEARD and OFFERING are
     * responder states and consume no offers at all, which is what makes the
     * three-machine cycle -- A believing B is its peer while B believes C is
     * and C believes A is -- structurally unreachable rather than merely
     * unlikely.
     */
    MCL_RDV_STATE_SOLICITING = 13
} mcl_rdv_state_t;

typedef struct {
    mcl_rdv_event_kind_t kind;
    uint8_t transport_id;    /* the bearer the event concerns, where it has one */
    uint8_t profile_id;
    uint32_t peer_ref;       /* correlation only; never identity */
    uint32_t migration_ref;
    /*
     * The peer's endpoint token for this bearer, as it arrived in
     * TRANSPORT_OFFER or TRANSPORT_ACCEPT.
     *
     * This is the field the integrator actually needs on BEARER_AGREED, and
     * the event did not carry it: transport, profile and refs said WHICH
     * bearer was agreed but nothing about where to reach the peer on it. It is
     * opaque here -- the coordinator does not interpret it, and its meaning is
     * defined per transport by that transport's endpoint registry.
     *
     * IT IS ZERO ON THE OFFERING SIDE, AND THAT IS NOT A BUG.
     *
     * TRANSPORT_OFFER carries an endpoint_token; TRANSPORT_ACCEPT does not.
     * So the accepting peer learns where to reach the offerer, and the offerer
     * learns nothing about where to reach the acceptor. The field cannot be
     * added at major 1 -- TRANSPORT_ACCEPT is 16 bytes and that size is fixed
     * for the major -- so the asymmetry is reported rather than papered over.
     *
     * An offering integrator must resolve the peer's candidate address by
     * other means, normally the source address its own transport reports for
     * the acceptance. mcl_rdv_candidate_ready() is where it asserts that it
     * has, and nothing is emitted on the candidate before that call.
     */
    uint32_t peer_endpoint_token;
    /* Contact-lifetime session, non-zero once agreed. Never a source_ref. */
    uint32_t session_ref;
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
    /*
     * ALLOCATE A SESSION REFERENCE FOR A NEW CONTACT.
     *
     * Optional. When NULL the coordinator generates one, which is correct for
     * a node holding ONE contact and wrong for a node holding several.
     * session_ref must be distinct across everything the integrator is
     * running, and the coordinator can only see itself: it has no view of the
     * contact pool, of references handed out by another coordinator in the
     * same process, or of references persisted across a restart.
     *
     * That boundary was invisible because the generator looked adequate.
     * `source_ref ^ k + counter * k'` is fine for one machine's own
     * transactions and says nothing about anybody else's, and a builder
     * running a pool would have discovered the collision as two contacts
     * quietly sharing a session.
     *
     * MUST write a NON-ZERO value and return 0. Zero is the reserved "no
     * session" value; returning it, or returning non-zero, is treated as a
     * refusal to allocate and the acceptance is not sent -- rather than
     * manufacturing a reference mcl_contact_agree will reject.
     */
    int (*allocate_session)(void *user, uint32_t *out);
    /*
     * THIS MACHINE'S REACHABILITY HINT ON A BEARER, FOR THIS TRANSACTION.
     *
     * Optional. When NULL the static mcl_rdv_config_t::bearer_endpoint_token
     * value is used, which is what a node with one contact and a fixed
     * address wants.
     *
     * It is not sufficient where the token has to select ONE transaction.
     * BLE-ACTIVATE-1 makes the token the match key of the advertisement the
     * peer scans for, so a machine running two activations on one bearer with
     * one static token advertises identically for both and a scanner cannot
     * tell them apart. A per-transaction token is the integrator's to mint,
     * because only the integrator knows what its transport can be reached on.
     *
     * Called ONCE per transaction, not once per emission: a retransmitted
     * offer carries the same token, for the same reason it carries the same
     * migration_ref. Zero is legal and means "reach me by the profile's own
     * discovery"; a non-zero return value from the callback itself is a
     * refusal, and the offer is deferred rather than sent with a token the
     * integrator did not sanction.
     */
    int (*allocate_endpoint_token)(void *user, uint8_t transport_id,
                                   uint8_t profile_id, uint32_t *out);
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
    /*
     * Where the timing parameters above come from.
     *
     * MCL_RDV_PARAMETERS_PROFILE (zero, and so the default) means the
     * AP-BOOTSTRAP-1 values. Leave the timing fields zero, or set them to the
     * profile's values; mcl_rdv_init() REFUSES a shared-medium configuration
     * that deviates, because a deviation is invisible on the wire and turns
     * into an interoperability failure at the second builder rather than a
     * compile error at the first.
     *
     * MCL_RDV_PARAMETERS_EXPERIMENTAL accepts any values and gives up the
     * Stranger-Contact claim. It exists so research does not have to lie
     * about which it is doing.
     */
    mcl_rdv_parameters_t parameters;
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
     *
     * ON A SHARED MEDIUM IT MUST ALSO BE RANDOM, AND A COUNTER IS NOT ENOUGH.
     *
     * A counter is the right shape for a value that only has to be distinct
     * within one node. migration_ref stopped being that the moment it became
     * the RESPONDER SELECTOR for an unaddressed broadcast: an ACCEPT names a
     * transaction, and every responder holding that reference believes it was
     * chosen. `source_ref ^ (counter * k)` is deterministic in source_ref, and
     * wire.h assigns source_ref no uniqueness property -- so two builders who
     * legally chose the same one, on their first transaction, produce the same
     * migration_ref and are selected by the same ACCEPT.
     *
     * So for a shared-medium deployment the value is drawn from
     * platform.random, which mcl_rdv_init() already requires there. A retry
     * REUSES it; a genuinely new transaction draws again. Point-to-point
     * deployments keep the counter, where its absolute non-repetition is worth
     * more than a probabilistic guarantee against a peer that cannot exist.
     */
    uint32_t last_migration_ref;
    uint32_t transaction_counter;
    /* What we actually offered, so an acceptance can be matched against it. */
    uint8_t offered_transport;
    uint8_t offered_profile;
    uint32_t session_ref;
    /*
     * OUR OWN token for the bearer currently being offered, resolved once per
     * transaction. Held rather than recomputed so a retransmitted offer is
     * byte-identical to the first: an allocator called again would mint a
     * second token, and the peer would then be scanning for one of two
     * advertisements with no way to know which.
     */
    uint32_t local_endpoint_token;
    uint32_t peer_endpoint_token; /* where to reach the peer on the candidate */
    uint8_t challenge[MCL_CONTACT_CHALLENGE_SIZE];
    /* The acceptor holds the challenge it has been asked to answer while
       local policy decides, because answering is its point of no return. */
    uint8_t pending_challenge[MCL_CONTACT_CHALLENGE_SIZE];
    uint8_t has_pending_challenge;
    /* Retransmission of the SAME control, never a fresh transaction. The
       handoff protocol is idempotent by construction and this is what
       exercises it; a new challenge or a new migration_ref on a retry would
       be a second transaction racing the first. */
    uint8_t retries;
    /* Local policy has admitted this peer. Latched for the contact: the
       admission question is asked once, not at every retransmission. */
    uint8_t admitted;
    /* POLICY_REQUIRED has been raised for this contact. Asked once, not once
       per retransmission: a peer that resent a challenge is not a new one. */
    uint8_t policy_raised;
    /* An acceptance prepared but not yet transmitted; see
       MCL_RDV_STATE_ACCEPTING. Held whole rather than rebuilt, so what is
       sent after the backoff is exactly what was decided on. */
    mcl_wire_tier0_t pending_accept;
    uint8_t has_pending_accept;
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
/*
 * The caller has made the agreed candidate bearer usable.
 *
 * Call this after MCL_RDV_EVENT_BEARER_AGREED, once the peer's
 * endpoint_token has been resolved and the endpoint is open and bound. Path
 * validation does not start until it is called, because a PATH_CHALLENGE on a
 * bearer nobody has opened proves nothing and merely fails slowly.
 *
 * MCL_RDV_ERR_STATE if there is no candidate waiting.
 */
mcl_rdv_status_t mcl_rdv_candidate_ready(mcl_rdv_t *rdv);

/*
 * Local policy has admitted this peer. Proceed.
 *
 * Call this after MCL_RDV_EVENT_POLICY_REQUIRED. The coordinator raises that
 * event once reachability is proven and before the first irrevocable act, and
 * takes no further step until one of these two is called.
 */
mcl_rdv_status_t mcl_rdv_admit(mcl_rdv_t *rdv);

/*
 * Local policy has refused this peer.
 *
 * The contact does not migrate and the coordinator closes. REFUSAL IS
 * CONFORMANT: reception is not identity, is not authority, and is not
 * obligation. A node that hears a stranger and declines to admit it has
 * behaved correctly, and nothing in MCL says otherwise.
 */
mcl_rdv_status_t mcl_rdv_refuse(mcl_rdv_t *rdv);

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
uint32_t mcl_rdv_reply_delay_ms(mcl_rdv_t *rdv);

#ifdef __cplusplus
}
#endif

#endif /* MCL_RENDEZVOUS_H */

#ifndef MCL_SDK_H
#define MCL_SDK_H

#include <stddef.h>
#include <stdint.h>

#include "mcl/wire.h"
#include "mcl/extension.h"
#include "mcl/link.h"
#include "mcl/contact.h"
#include "mcl/handoff.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t mcl_sdk_status_t;
enum {
    MCL_SDK_OK = 0,
    MCL_SDK_ERR_INVALID_ARGUMENT = 1,
    MCL_SDK_ERR_BUFFER_TOO_SMALL = 2,
    MCL_SDK_ERR_WIRE_FAILURE = 3,
    MCL_SDK_ERR_LINK_FAILURE = 4,
    MCL_SDK_ERR_TX_UNAVAILABLE = 5,
    MCL_SDK_ERR_TX_FAILURE = 6,
    MCL_SDK_ERR_FRAME_FAILURE = 7,
    MCL_SDK_ERR_INVALID_STATE = 8,
    /*
     * The transport stated definitely that nothing was transmitted. Distinct
     * from MCL_SDK_ERR_TX_UNCERTAIN because the two demand opposite handling
     * of an irrevocable step; see the tx callback contract below.
     */
    MCL_SDK_ERR_TX_NOT_SENT = 9,
    /*
     * The transport could not say whether the bytes left. Any state change the
     * transmission implies HAS been made, because the peer may have received
     * it. The caller should retransmit -- every control in this SDK is
     * idempotent for exactly this case.
     */
    MCL_SDK_ERR_TX_UNCERTAIN = 10,
    /* Bytes arrived on a transport where this frame is not admissible. */
    MCL_SDK_ERR_WRONG_TRANSPORT = 11,
    /*
     * Ordinary traffic is suspended for this contact. Not an error in the
     * contact: a transport cutover is in progress and neither medium can be
     * used for data until it completes. See mcl_contact_data_transport.
     */
    MCL_SDK_ERR_QUIESCED = 12,
    /*
     * The frame decoded correctly and is addressed to another node. Not an
     * error: on a shared bearer, hearing frames for other machines is normal.
     */
    MCL_SDK_NOT_ADDRESSED = 13
};

/*
 * Low-level transport transmission callback.
 *
 * Contract:
 * - user:         caller-provided opaque context passed at node initialization.
 * - transport_id: which bearer these bytes MUST leave on, from the
 *                 transport-id registry in mcl/contact.h.
 * - data:         the exact bytes to transmit.
 * - data_size:    their length.
 *
 * WHY THE TRANSPORT IS A PARAMETER
 *
 * An earlier revision had none, and a node therefore had exactly one way out.
 * That cannot express a migration, which is the one thing this layer exists to
 * do. During a migration a contact spans two media at once:
 *
 *     TRANSPORT_OFFER / ACCEPT        old transport
 *     PATH_CHALLENGE / PATH_RESPONSE  candidate transport
 *     COMMIT / CONFIRM                candidate transport
 *     ordinary traffic                depends on the cutover state
 *
 * With one callback and no transport argument, an integrator had to infer which
 * socket, characteristic or speaker each call meant from the order of calls --
 * which is to say, the SDK documented the requirement and then made it the
 * caller's problem to guess. Now the SDK derives it from the contact state and
 * says so on every call.
 *
 * WHAT THE RETURN VALUE MEANS
 *
 *      0   accepted for transmission.
 *    < 0   DEFINITELY not transmitted. Nothing left this machine.
 *    > 0   outcome UNKNOWN. It may or may not have left.
 *
 * The three-way return exists because of COMMIT. Committing a migration is
 * irrevocable once the bytes are transmitted, so the SDK must not enter that
 * state for a frame the transport is certain it never sent -- and must enter it
 * for one the transport cannot vouch for, because the peer may have it.
 * Collapsing "not sent" and "unknown" into one failure forces a choice between
 * a contact stuck irrevocably on nothing, and a rollback after a commit the
 * peer may have acted on. Real bearers distinguish these: a full transmit queue
 * is a definite refusal, a BLE notification with no completion event is not.
 *
 * A transport that genuinely cannot tell the difference MUST return > 0.
 * Claiming certainty it does not have is the failure this parameter prevents.
 */
typedef int32_t (*mcl_sdk_tx_fn)(
    void *user,
    uint8_t transport_id,
    const uint8_t *data,
    size_t data_size);

typedef struct {
    uint16_t supported_wire_majors_mask;
    mcl_sdk_tx_fn tx_fn;   /* Optional: NULL for receive-only nodes */
    void *user_ctx;        /* Passed to tx_fn */
    /*
     * Contact reference this node puts in the source_ref of frames it sends.
     * It correlates frames within a contact and is not an identity: a peer
     * must never treat it as proof of who sent something.
     */
    uint32_t source_ref;
    /*
     * Transport this node's contact starts on, from the transport-id registry.
     * Required: a contact always exists on some medium, and leaving it
     * unspecified would mean the node could not describe its own migrations.
     */
    uint8_t transport_id;
    /* Ordering role for migration negotiation. Confers no authority. */
    mcl_contact_role_t role;
} mcl_node_config_t;

/* ============================================================
 * WHO OWNS WHAT: LINK LIFECYCLE VERSUS CONTACT CONTINUITY
 *
 * A node holds two state machines, and they answer different questions:
 *
 *   mcl_link_t     the protocol lifecycle. Have we discovered a peer,
 *                  exchanged capabilities, negotiated, established?
 *   mcl_contact_t  transport continuity. Which medium carries this contact,
 *                  and is a change of medium under way?
 *
 * NEITHER IMPLIES THE OTHER, and neither is derived from the other. A machine
 * can be settled on a transport having negotiated nothing; it can be
 * mid-negotiation with no migration in sight. An earlier revision provided
 * mcl_contact_link_state(), which claimed a mapping between them; it has been
 * removed, because a function reporting what one machine "ought" to look like,
 * beside a second machine that is independently mutable, is a third source of
 * truth that drifts from both.
 *
 * They cross in exactly ONE place, and the SDK owns it:
 *
 *   A migration may only be DRIVEN while the link lifecycle is ESTABLISHED or
 *   HANDOFF, and the link may not leave those states while a migration is in
 *   progress.
 *
 * The SDK enforces both halves: mcl_node_send_handoff and
 * mcl_node_apply_handoff refuse to act outside those states, and
 * mcl_node_link_transition refuses to leave them with a transaction
 * outstanding. Sending and receiving ordinary frames is NOT gated on the
 * lifecycle -- first contact necessarily happens before establishment, and a
 * layer whose first frame required an established session could never send one.
 *
 * Recorded in mcl-link/spec/link-contact-ownership-v0.1.md.
 * ============================================================ */

/*
 * One node currently tracks one contact, mirroring the single mcl_link_t it
 * already holds. A machine that must hold several concurrent contacts
 * instantiates several nodes.
 *
 * That is a real limitation, not an oversight: distinguishing several
 * simultaneous contacts, each with its own candidate endpoint, is the
 * multi-peer cross-binding problem recorded in
 * mcl-link/research/secure-contact-threat-model.md, and it is not solved by
 * giving one node an array.
 */
typedef struct {
    mcl_link_t link;
    mcl_contact_t contact;
    mcl_sdk_tx_fn tx_fn;
    void *user_ctx;
    uint16_t supported_wire_majors_mask;
    uint32_t source_ref;
    uint16_t tx_sequence;
} mcl_node_t;

/* Node lifecycle */
mcl_sdk_status_t mcl_node_init(
    mcl_node_t *node,
    const mcl_node_config_t *config);

mcl_sdk_status_t mcl_node_reset(mcl_node_t *node);

/* Tier-0 transmission and reception */
mcl_sdk_status_t mcl_node_send_tier0(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent);

mcl_sdk_status_t mcl_node_receive_tier0(
    mcl_node_t *node,
    const uint8_t *data,
    size_t data_size,
    mcl_wire_tier0_t *object,
    size_t *consumed);

/* ---------- Framed contact path ----------
 *
 * The functions above move raw canonical Wire bytes, which is what a bearer
 * like MCL-AP carries directly. These move MCL Link frames instead, which is
 * what the IP, BLE and UWB bindings carry, and what a real contact session
 * needs: a frame class, a session reference, a sequence, and optional
 * integrity.
 *
 * Both paths are supported deliberately. A minimal broadcast beacon has no use
 * for a session reference, and forcing one on it would waste bytes on exactly
 * the transport where bytes are scarcest.
 */

/*
 * Send a Tier-0 object inside a Link frame.
 *
 * `scratch` holds the Wire encoding and then the finished frame, so it must be
 * large enough for both. `flags` selects the optional Link frame fields; the
 * sequence is maintained by the node when MCL_LINK_FLAG_SEQUENCE is set.
 *
 * MCL_LINK_FLAG_DESTINATION addresses the frame to THIS CONTACT'S PEER, whose
 * reference was learned during first contact. There is no destination
 * parameter: a node holds one contact, so the only machine it can address is
 * that one, and the value is already known. Setting the flag before a peer
 * reference has been learned returns MCL_SDK_ERR_INVALID_STATE rather than
 * emitting a frame addressed to reference zero. The same applies to
 * mcl_node_send_framed_tier0_ext and mcl_node_send_handoff.
 *
 * The frame goes out on mcl_contact_data_transport(). While that reports the
 * contact QUIESCED -- between the transmission of COMMIT and the arrival of
 * CONFIRM -- this returns MCL_SDK_ERR_QUIESCED and sends nothing. In that
 * window the peer may already have left the transport this node still considers
 * active, and this node cannot find out until the CONFIRM it is waiting for.
 * Transmitting anyway would put ordinary traffic onto a medium believed,
 * without evidence, still to be carrying the contact. The window is bounded by
 * the CONFIRM exchange; handoff controls are unaffected, since completing that
 * exchange is what ends it.
 */
mcl_sdk_status_t mcl_node_send_framed_tier0(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent);

/*
 * Send a Tier-0 object in a Link frame at explicitly chosen majors.
 *
 * Wire and Link version independently, so both are named. `MCL Base 1` (see
 * mcl-core/spec/conformance-profiles-v1.md section 4) requires Wire major 1
 * carried inside Link major 1; mcl_node_send_framed_tier0 emits the
 * experimental pair and cannot express that combination.
 *
 * Refuses, rather than substituting a workable major:
 *
 *   - an unassigned Wire or Link major;
 *   - a Candidate object at the Stable Wire major -- HAZARD, REQUEST,
 *     AUTHORITY_CLAIM and DEGRADED_STATE are not carried at major 1, and the
 *     refusal is MCL_SDK_ERR_WIRE_FAILURE.
 *
 * PRESENCE is not the same bytes at both majors: major 1 drops machine_class,
 * so the same object encodes to 11 bytes at major 0 and 10 at major 1. A
 * caller moving to major 1 should stop populating that field rather than
 * expect it to travel.
 */
mcl_sdk_status_t mcl_node_send_framed_tier0_at_major(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    uint8_t wire_major,
    uint8_t link_major,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent);

/*
 * Decode a received Link frame and, when it carries a Tier-0 object, decode
 * that too.
 *
 * The frame is decoded first and rejected on its own terms before any payload
 * is looked at, so a malformed frame never reaches the semantic decoder. The
 * decoded object is written only when the frame class is one that carries
 * semantics; `has_object` reports whether it was.
 *
 * The frame's payload_len is an exact boundary: a semantic object that ends
 * before it means the payload carries undeclared trailing bytes, and the frame
 * is rejected rather than partially accepted.
 *
 * This has no side effects on link state. A received frame is information for
 * local policy, never an instruction: nothing here transitions the state
 * machine, and an AUTHORITY_CLAIM arriving in a frame does not become
 * authority by being received.
 *
 * `arrival_transport` is where the bytes came in. A frame arriving on a
 * transport that is neither this contact's active one nor its candidate is
 * refused with MCL_SDK_ERR_WRONG_TRANSPORT: the contact is not there, and
 * accepting it would let any bearer the process happens to have open inject
 * frames into a contact established somewhere else.
 *
 * A frame carrying MCL_LINK_FLAG_DESTINATION whose destination_ref is not this
 * node's returns MCL_SDK_NOT_ADDRESSED, with the frame still decoded. That is
 * not an error -- on a shared bearer, hearing traffic for other machines is the
 * normal case -- but the SDK must not hand the caller a semantic object from a
 * frame explicitly addressed elsewhere and leave the filtering to be remembered.
 *
 * All pointer arguments except `object` are required.
 */
mcl_sdk_status_t mcl_node_receive_framed(
    mcl_node_t *node,
    uint8_t arrival_transport,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_wire_tier0_t *object,
    uint8_t *has_object,
    size_t *consumed);

/* ---------- Framed path with Wire extensions ----------
 *
 * mcl_node_send_framed_tier0 and mcl_node_receive_framed carry objects with no
 * extensions, and the receive side REFUSES an object whose header sets
 * extension_present rather than decoding the body and discarding the rest.
 * That is the safe default -- an extension may be critical -- but it also means
 * those two functions cannot talk to a peer that uses extensions at all.
 *
 * These are the extension-aware counterparts.
 *
 * Both take caller-owned buffers for BOTH stages, rather than putting a
 * worst-case Wire buffer on the stack as the non-extension path does. A Tier-0
 * object with a full extension block is an order of magnitude larger than one
 * without, and a hidden 275-byte stack frame is not something a Cortex-M0
 * integrator should discover from a stack overflow. Charter: caller-owned state
 * and buffers.
 */

/*
 * Send a Tier-0 object with extensions inside a Link frame.
 *
 * `wire_scratch` holds the encoded object and must be at least
 * MCL_WIRE_TIER0_EXT_MAX_SIZE for the largest block; `frame_scratch` holds the
 * finished frame. Passing extension_count 0 produces bytes identical to
 * mcl_node_send_framed_tier0.
 */
mcl_sdk_status_t mcl_node_send_framed_tier0_ext(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    const mcl_wire_extension_t *extensions,
    size_t extension_count,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *wire_scratch,
    size_t wire_scratch_capacity,
    uint8_t *frame_scratch,
    size_t frame_scratch_capacity,
    size_t *bytes_sent);

/*
 * Decode a received Link frame and, when it carries a Tier-0 object, decode
 * that object and its extension block.
 *
 * `accept` decides which critical extensions this caller implements AND
 * accepts, seeing each one's value; NULL means none, and every critical
 * extension then makes the object undecodable. `reader` is positioned at the
 * extension block on success and borrows from `data`, so it stays valid only as
 * long as `data` does.
 *
 * As with mcl_node_receive_framed, this changes no link state. A received frame
 * is information for local policy, never an instruction.
 */
mcl_sdk_status_t mcl_node_receive_framed_ext(
    mcl_node_t *node,
    uint8_t arrival_transport,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_wire_tier0_t *object,
    mcl_wire_extension_reader_t *reader,
    mcl_wire_extension_accept_fn accept,
    void *accept_user,
    uint8_t *has_object,
    size_t *consumed);

/* ---------- Handoff control path ----------
 *
 * The migration controls defined in mcl/handoff.h, carried as the payload of a
 * Link frame of class HANDOFF. This is what makes migration an on-wire protocol
 * rather than a sequence of local function calls: a hardware run driven by
 * direct calls to mcl_contact_* proves the radios work, not that two
 * independent implementations could migrate.
 *
 * Sending, receiving and APPLYING are three separate steps on purpose.
 * Receiving decodes and changes nothing; applying is an explicit call the
 * caller makes after its own policy has decided to. Charter 2.10.1 puts the
 * interaction sequence in the deployment's hands, and charter 2.3 forbids
 * reception from becoming authority.
 */

/*
 * What the caller should send in response to a control it has applied.
 *
 * The library decides WHAT to send and on WHICH transport
 * (mcl_contact_control_transport); the caller performs the transmission,
 * because applying a control and answering it are separate decisions and
 * charter 2.10.1 puts the interaction sequence in the deployment's hands.
 */
typedef uint8_t mcl_handoff_action_t;
enum {
    MCL_HANDOFF_ACTION_NONE               = 0u,
    MCL_HANDOFF_ACTION_SEND_PATH_RESPONSE = 1u,
    MCL_HANDOFF_ACTION_SEND_CONFIRM       = 2u
};

/*
 * Send a handoff control inside a HANDOFF Link frame.
 *
 * `flags` selects the optional Link frame fields as for
 * mcl_node_send_framed_tier0. MCL_LINK_FLAG_SESSION is REQUIRED -- see below --
 * and its session_ref must equal the control's; this function enforces both
 * rather than letting a frame contradict, or fail to name, its own payload.
 *
 * `scratch` must hold the encoded frame.
 *
 * THE SESSION FLAG IS MANDATORY
 *
 * All four handoff controls are post-acceptance: they exist only after an
 * acceptance bound a session reference. They arrive on the CANDIDATE transport,
 * which the contact has not been using, so a machine holding several contacts
 * must decide which contact a freshly arrived frame belongs to BEFORE parsing a
 * class-specific payload. The Link header already carries that routing field.
 * Requiring it costs four bytes on frames of 10 or 18 payload bytes, on a
 * transport just chosen for being better than the one where bytes were scarce.
 *
 * An earlier revision made it a recommendation and checked it only when
 * present, which meant the routing field a multi-contact receiver depends on
 * could simply be absent.
 *
 * THE TRANSPORT IS CHOSEN, NOT PASSED
 *
 * The frame goes out on mcl_contact_control_transport() -- the candidate while
 * a migration is in progress, the active transport otherwise, which is where a
 * COMMIT retransmitted after completion belongs. The caller cannot override it:
 * a handoff control sent on the wrong medium proves nothing about the medium it
 * claims to be establishing.
 *
 * COMMIT IS SPECIAL, AND THIS FUNCTION OWNS THE TRANSITION
 *
 * Sending COMMIT is irrevocable, so the state change must be tied to the
 * transmission rather than made before it by the caller. This function performs
 * both: it transmits, and then enters COMMITTING unless the transport reported
 * that nothing was sent. Doing it the other way round -- caller calls
 * mcl_contact_commit_begin(), then asks the SDK to transmit -- can strand a
 * contact irrevocably in COMMITTING over a frame that was never sent.
 */
mcl_sdk_status_t mcl_node_send_handoff(
    mcl_node_t *node,
    const mcl_handoff_control_t *control,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent);

/*
 * Decode a received HANDOFF frame and its control payload.
 *
 * The frame is decoded and rejected on its own terms first, so a malformed
 * frame never reaches the control decoder. A frame whose class is not HANDOFF
 * is refused here: the class is what tells a receiver which registry the
 * payload's first bytes belong to, and a payload interpreted under two classes
 * is a payload with two meanings.
 *
 * The frame's payload_len is an exact boundary. A control that does not fill it
 * is rejected rather than partially accepted.
 *
 * `arrival_transport` is where the bytes actually came in, and it is CHECKED:
 * a control is refused unless it arrived on mcl_contact_control_transport().
 * Without this argument the SDK could not perform the one check path validation
 * depends on. A PATH_RESPONSE fed in from the old path would otherwise validate
 * a candidate that had never carried a single byte -- which is precisely, and
 * only, what PATH_CHALLENGE/PATH_RESPONSE exist to establish.
 *
 * MCL_LINK_FLAG_SESSION is required and its session_ref must equal the
 * control's. A frame lacking it is refused, not merely unchecked.
 *
 * This changes no state whatever -- not the contact, not the link, not the
 * sequence. Nothing about receiving these bytes commits this machine to
 * anything; see mcl_node_apply_handoff.
 */
mcl_sdk_status_t mcl_node_receive_handoff(
    mcl_node_t *node,
    uint8_t arrival_transport,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_handoff_control_t *control,
    size_t *consumed);

/*
 * Apply a decoded control to this node's contact, and report what to send back.
 *
 * Every state change goes through the existing mcl_contact_* API. There is
 * deliberately no second state machine here: a duplicate would drift from the
 * first, and the two would disagree exactly when a migration was already going
 * wrong.
 *
 *   PATH_CHALLENGE in AGREED       -> validated locally; send PATH_RESPONSE
 *   PATH_CHALLENGE in VALIDATED    -> duplicate; send PATH_RESPONSE, no change
 *   PATH_RESPONSE  in VALIDATING   -> VALIDATED
 *   PATH_RESPONSE  in VALIDATED    -> duplicate; no change
 *   COMMIT         in VALIDATED    -> ACTIVE on the candidate; send CONFIRM
 *   COMMIT         in ACTIVE       -> duplicate; send CONFIRM, no change
 *   CONFIRM        in COMMITTING   -> ACTIVE on the candidate
 *   CONFIRM        in ACTIVE       -> duplicate; no change
 *
 * EVERY ROW HAS A DUPLICATE ROW, AND THAT IS THE POINT.
 *
 * On a lossy medium the only repair is retransmission, so each control must be
 * answerable a second time with the same outcome. The duplicate PATH_CHALLENGE
 * row is the one that was missing and that a radio would have found: B echoes a
 * challenge, its response is lost, A correctly retransmits -- and B, no longer
 * in AGREED, refused it. One dropped frame killed a migration with both peers
 * behaving correctly.
 *
 * Applying a COMMIT goes straight from VALIDATED to ACTIVE and never enters
 * COMMITTING. That state is reserved for the peer that SENT a commit and does
 * not know whether it arrived -- the peer that is forbidden to roll back. A
 * receiver has sent no CONFIRM and is under no such constraint, so keeping the
 * two out of one state is what makes the rollback rule enforceable.
 *
 * For the peer that ECHOES a challenge, reaching VALIDATED means it has
 * performed its half: it received a frame on the candidate and is about to send
 * one. It does not yet know its own reply arrived. It learns that only when
 * COMMIT follows, which the controlling peer sends only after the response
 * reached it.
 *
 * A control that does not match the contact's state or its transaction
 * references is refused WITHOUT changing anything, and the old working
 * transport is left intact. A failed migration must never destroy the contact.
 *
 * Applying a control establishes no identity, authenticity, authority or trust.
 * Every reference in it crossed an observable medium in the clear, so anyone in
 * range can quote it back. See mcl-link/spec/link-handoff-control-v0.1.md §4.2.
 */
mcl_sdk_status_t mcl_node_apply_handoff(
    mcl_node_t *node,
    const mcl_handoff_control_t *control,
    mcl_handoff_action_t *action);

/* Narrow Link state operations */
/*
 * Contact and migration state.
 *
 * Separate from the Link accessors on purpose. A Wire context_id and a contact
 * session_ref are different things with different lifetimes, and an earlier
 * revision of this SDK copied the former into the latter when emitting a frame.
 * See the note on MCL_LINK_FLAG_SESSION in mcl_node_send_framed_tier0.
 */
mcl_contact_t *mcl_node_get_contact(mcl_node_t *node);
const mcl_contact_t *mcl_node_get_contact_const(const mcl_node_t *node);

mcl_link_t *mcl_node_get_link(mcl_node_t *node);
const mcl_link_t *mcl_node_get_link_const(const mcl_node_t *node);

mcl_sdk_status_t mcl_node_link_transition(
    mcl_node_t *node,
    mcl_link_state_t new_state);

mcl_sdk_status_t mcl_node_link_install_context(
    mcl_node_t *node,
    const mcl_link_context_key_t *key);

mcl_sdk_status_t mcl_node_link_authorize_context(
    const mcl_node_t *node,
    const mcl_link_context_key_t *key);

#ifdef __cplusplus
}
#endif

#endif /* MCL_SDK_H */

#ifndef MCL_SDK_H
#define MCL_SDK_H

#include <stddef.h>
#include <stdint.h>

#include "mcl/wire.h"
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
    MCL_SDK_ERR_INVALID_STATE = 8
};

/*
 * Low-level transport transmission callback.
 *
 * Contract:
 * - user: Caller-provided opaque context passed at node initialization.
 * - data: Pointer to canonical Wire-encoded bytes.
 *         NOTE: This is NOT a normative MCL Link binary frame; it is raw canonical
 *         Wire payload bytes delivered directly to the physical/bearer transport layer.
 * - data_size: Exact length of the canonical Wire encoding.
 *
 * Returns:
 * 0 on success, or non-zero transport-specific error code (mapped to MCL_SDK_ERR_TX_FAILURE).
 */
typedef int32_t (*mcl_sdk_tx_fn)(
    void *user,
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
 * All pointer arguments except `object` are required.
 */
mcl_sdk_status_t mcl_node_receive_framed(
    mcl_node_t *node,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_wire_tier0_t *object,
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
 * The library does not send it: transmitting requires knowing which transport
 * to use, and during a migration the candidate and the current transport are
 * different. Only the caller knows which socket, characteristic or speaker the
 * reply belongs on.
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
 * mcl_node_send_framed_tier0. MCL_LINK_FLAG_SESSION is recommended and, when
 * set, must carry the same session_ref the control does; this function enforces
 * that rather than letting a frame contradict its own payload.
 *
 * `scratch` must hold the encoded frame.
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
 * This changes no state whatever -- not the contact, not the link, not the
 * sequence. Nothing about receiving these bytes commits this machine to
 * anything; see mcl_node_apply_handoff.
 */
mcl_sdk_status_t mcl_node_receive_handoff(
    mcl_node_t *node,
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
 *   PATH_RESPONSE  in VALIDATING   -> VALIDATED
 *   COMMIT         in VALIDATED    -> ACTIVE on the candidate; send CONFIRM
 *   COMMIT         in ACTIVE       -> retransmission; send CONFIRM, no change
 *   CONFIRM        in COMMITTING   -> ACTIVE on the candidate
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

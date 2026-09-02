#ifndef MCL_SDK_H
#define MCL_SDK_H

#include <stddef.h>
#include <stdint.h>

#include "mcl/wire.h"
#include "mcl/link.h"

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
} mcl_node_config_t;

typedef struct {
    mcl_link_t link;
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

/* Narrow Link state operations */
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

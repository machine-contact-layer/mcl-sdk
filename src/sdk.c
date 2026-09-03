#include "mcl/sdk.h"

mcl_sdk_status_t mcl_node_init(
    mcl_node_t *node,
    const mcl_node_config_t *config)
{
    mcl_link_status_t lst;

    if (node == NULL || config == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_init(&node->link, config->supported_wire_majors_mask);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    node->tx_fn = config->tx_fn;
    node->user_ctx = config->user_ctx;
    node->supported_wire_majors_mask = config->supported_wire_majors_mask;
    node->source_ref = config->source_ref;
    node->tx_sequence = 0u;

    /*
     * The contact begins on the configured transport. Its source_ref doubles as
     * the contact's local reference so the two cannot disagree about who this
     * node is within the contact.
     */
    lst = mcl_contact_begin(&node->contact, config->role,
                            config->source_ref, config->transport_id);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_reset(mcl_node_t *node)
{
    mcl_link_status_t lst;

    if (node == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_reset(&node->link);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }
    node->tx_sequence = 0u;

    /*
     * Reset the contact onto the transport it is currently using rather than
     * the one it started on. A reset clears session and migration state; it is
     * not a claim that the machine teleported back to the first medium.
     */
    lst = mcl_contact_begin(&node->contact, node->contact.role,
                            node->source_ref, node->contact.active_transport);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_send_tier0(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    mcl_wire_status_t wst;
    size_t written = 0u;
    int32_t tx_res;

    if (node == NULL || object == NULL || scratch == NULL || scratch_capacity == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    if (node->tx_fn == NULL) {
        return MCL_SDK_ERR_TX_UNAVAILABLE;
    }

    wst = mcl_wire_tier0_encode(object, scratch, scratch_capacity, &written);
    if (wst == MCL_WIRE_ERR_BUFFER_TOO_SMALL) {
        return MCL_SDK_ERR_BUFFER_TOO_SMALL;
    }
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    tx_res = node->tx_fn(node->user_ctx, scratch, written);
    if (tx_res != 0) {
        return MCL_SDK_ERR_TX_FAILURE;
    }

    if (bytes_sent != NULL) {
        *bytes_sent = written;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_receive_tier0(
    mcl_node_t *node,
    const uint8_t *data,
    size_t data_size,
    mcl_wire_tier0_t *object,
    size_t *consumed)
{
    mcl_wire_status_t wst;
    size_t bytes_consumed = 0u;

    if (node == NULL || data == NULL || object == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    /*
     * Parse Tier-0 object using Wire codec.
     * Note: Receiving an AUTHORITY_CLAIM, REQUEST, or other semantic does NOT
     * perform local policy execution, grant local privileges, or mutate Link context.
     * Product policy remains strictly outside the Machine Contact Layer.
     */
    wst = mcl_wire_tier0_decode(data, data_size, object, &bytes_consumed);
    if (wst == MCL_WIRE_ERR_BUFFER_TOO_SMALL || wst == MCL_WIRE_ERR_TRUNCATED) {
        return MCL_SDK_ERR_BUFFER_TOO_SMALL;
    }
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    if (consumed != NULL) {
        *consumed = bytes_consumed;
    }

    return MCL_SDK_OK;
}

mcl_contact_t *mcl_node_get_contact(mcl_node_t *node)
{
    return (node != NULL) ? &node->contact : NULL;
}

const mcl_contact_t *mcl_node_get_contact_const(const mcl_node_t *node)
{
    return (node != NULL) ? &node->contact : NULL;
}

mcl_link_t *mcl_node_get_link(mcl_node_t *node)
{
    return (node != NULL) ? &node->link : NULL;
}

const mcl_link_t *mcl_node_get_link_const(const mcl_node_t *node)
{
    return (node != NULL) ? &node->link : NULL;
}

mcl_sdk_status_t mcl_node_link_transition(
    mcl_node_t *node,
    mcl_link_state_t new_state)
{
    mcl_link_status_t lst;

    if (node == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_transition(&node->link, new_state);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_link_install_context(
    mcl_node_t *node,
    const mcl_link_context_key_t *key)
{
    mcl_link_status_t lst;

    if (node == NULL || key == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_install_context(&node->link, key);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_link_authorize_context(
    const mcl_node_t *node,
    const mcl_link_context_key_t *key)
{
    mcl_link_status_t lst;

    if (node == NULL || key == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_authorize_context(&node->link, key);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

/* ---------- Framed contact path ---------- */

mcl_sdk_status_t mcl_node_send_framed_tier0(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    mcl_link_frame_t frame;
    mcl_wire_status_t wst;
    mcl_link_status_t lst;
    size_t wire_written = 0u;
    size_t frame_written = 0u;
    int32_t tx_res;

    if (node == NULL || object == NULL || scratch == NULL || scratch_capacity == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
    if (node->tx_fn == NULL) {
        return MCL_SDK_ERR_TX_UNAVAILABLE;
    }

    wst = mcl_wire_tier0_encode(object, wire_buf, sizeof(wire_buf), &wire_written);
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    frame.frame_class = frame_class;
    frame.flags = flags;
    frame.source_ref = node->source_ref;
    frame.destination_ref = 0u;
    frame.session_ref = 0u;
    frame.sequence = 0u;
    frame.freshness_ms = 0u;
    frame.payload = wire_buf;
    frame.payload_len = (uint16_t)wire_written;

    if ((flags & MCL_LINK_FLAG_SESSION) != 0u) {
        /*
         * The session reference comes from the CONTACT, never from the Wire
         * context.
         *
         * An earlier revision emitted node->link.active_context.context_id
         * here, which conflated two unrelated things:
         *
         *   session_ref   correlates a continuing contact across a transport
         *                 change; it is chosen in TRANSPORT_ACCEPT and lives
         *                 for as long as the contact does
         *
         *   context_id    names an installed semantic compression ruleset; it
         *                 has its own generation and can change, or never
         *                 exist, while the contact is perfectly healthy
         *
         * A machine can have a session with no context, the same session across
         * several context generations, or the same contact with an entirely new
         * context. Deriving one from the other made all three unrepresentable
         * and forced a session reference to depend on a compression decision.
         *
         * Emitting a session reference before a migration has been agreed would
         * invite a peer to correlate against a session that does not exist, so
         * that is refused rather than filled with a placeholder.
         */
        if (node->contact.session_valid == 0u) {
            return MCL_SDK_ERR_INVALID_STATE;
        }
        frame.session_ref = node->contact.session_ref;
    }
    if ((flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        frame.sequence = node->tx_sequence;
    }

    lst = mcl_link_frame_encode(&frame, scratch, scratch_capacity, &frame_written);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    tx_res = node->tx_fn(node->user_ctx, scratch, frame_written);
    if (tx_res != 0) {
        return MCL_SDK_ERR_TX_FAILURE;
    }

    /* Advance only after the transport accepted the frame. */
    if ((flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        node->tx_sequence = (uint16_t)(node->tx_sequence + 1u);
    }
    if (bytes_sent != NULL) {
        *bytes_sent = frame_written;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_receive_framed(
    mcl_node_t *node,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_wire_tier0_t *object,
    uint8_t *has_object,
    size_t *consumed)
{
    mcl_link_status_t lst;
    mcl_wire_status_t wst;
    size_t wire_consumed = 0u;

    if (node == NULL || data == NULL || frame == NULL ||
        has_object == NULL || consumed == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    *has_object = 0u;

    /*
     * Decode the frame on its own terms first. A malformed frame must never
     * reach the semantic decoder, and an unknown frame class is rejected
     * rather than guessed at.
     */
    lst = mcl_link_frame_decode(data, data_size, frame, consumed);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    /*
     * Only classes that carry semantics are handed to Wire. A KEEPALIVE or an
     * ACK has no payload to interpret, and attempting to decode one as a
     * semantic object would manufacture meaning that was never sent.
     */
    if (frame->frame_class != MCL_LINK_CLASS_CONTACT &&
        frame->frame_class != MCL_LINK_CLASS_DATA &&
        frame->frame_class != MCL_LINK_CLASS_CAPABILITY &&
        frame->frame_class != MCL_LINK_CLASS_NEGOTIATION) {
        return MCL_SDK_OK;
    }
    if (object == NULL || frame->payload == NULL || frame->payload_len == 0u) {
        return MCL_SDK_OK;
    }

    wst = mcl_wire_tier0_decode(frame->payload, (size_t)frame->payload_len,
                                object, &wire_consumed);
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }
    if (wire_consumed != (size_t)frame->payload_len) {
        /*
         * payload_len is an exact declared boundary, so a semantic object that
         * ends before it means the payload carries bytes nobody declared.
         * Accepting the object and ignoring the remainder is how a framing
         * discrepancy becomes a semantic one: two implementations would
         * disagree about what was sent while both believed they had decoded it
         * successfully.
         */
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    *has_object = 1u;
    return MCL_SDK_OK;
}

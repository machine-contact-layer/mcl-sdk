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

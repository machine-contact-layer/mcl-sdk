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
    MCL_SDK_ERR_TX_FAILURE = 6
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
} mcl_node_config_t;

typedef struct {
    mcl_link_t link;
    mcl_sdk_tx_fn tx_fn;
    void *user_ctx;
    uint16_t supported_wire_majors_mask;
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

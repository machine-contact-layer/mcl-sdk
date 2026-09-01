/*
 * MCL SDK Minimal Example: Two-Node In-Memory Exchange
 *
 * Demonstrates:
 * Node A (Presence semantic) -> SDK -> Wire bytes -> transport callback -> Node B -> decoded semantic object.
 */

#include "mcl/sdk.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t buffer[64];
    size_t size;
} pipe_transport_t;

static int32_t pipe_tx(void *user, const uint8_t *data, size_t data_size)
{
    pipe_transport_t *p = (pipe_transport_t *)user;
    if (data_size > sizeof(p->buffer)) {
        return -1;
    }
    memcpy(p->buffer, data, data_size);
    p->size = data_size;
    return 0;
}

int main(void)
{
    mcl_node_t node_a;
    mcl_node_t node_b;
    mcl_node_config_t cfg_a;
    mcl_node_config_t cfg_b;
    pipe_transport_t pipe;
    uint8_t scratch[64];
    size_t sent = 0u;
    size_t consumed = 0u;
    mcl_wire_tier0_t send_obj;
    mcl_wire_tier0_t recv_obj;

    memset(&pipe, 0, sizeof(pipe));

    /* Configure Node A with transmitter callback */
    cfg_a.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg_a.tx_fn = pipe_tx;
    cfg_a.user_ctx = &pipe;
    mcl_node_init(&node_a, &cfg_a);

    /* Configure Node B as a receiver node */
    cfg_b.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg_b.tx_fn = NULL;
    cfg_b.user_ctx = NULL;
    mcl_node_init(&node_b, &cfg_b);

    /* Create a Tier-0 PRESENCE announcement on Node A */
    memset(&send_obj, 0, sizeof(send_obj));
    send_obj.kind = MCL_WIRE_KIND_PRESENCE;
    send_obj.priority = 1u;
    send_obj.source_ref = 0xCAFEBABEu;
    send_obj.body.presence.machine_class = 2u;
    send_obj.body.presence.capability_digest = 0x112233u; /* 24-bit field */
    send_obj.body.presence.ttl = 10u;

    printf("Node A: encoding and sending Tier-0 PRESENCE...\n");
    mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &sent);
    printf("        Wire bytes sent: %zu bytes\n", sent);

    /* Node B receives the canonical Wire bytes from the transport pipe */
    printf("Node B: receiving %zu Wire bytes from transport...\n", pipe.size);
    mcl_node_receive_tier0(&node_b, pipe.buffer, pipe.size, &recv_obj, &consumed);

    if (recv_obj.kind == MCL_WIRE_KIND_PRESENCE &&
        recv_obj.body.presence.capability_digest == 0x112233u) {
        printf("Node B: successfully decoded PRESENCE! capability_digest = 0x%06X\n",
               recv_obj.body.presence.capability_digest);
        puts("Hello World: OK");
        return 0;
    }

    fprintf(stderr, "Error: received object mismatch\n");
    return 1;
}

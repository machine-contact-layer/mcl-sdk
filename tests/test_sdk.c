#include "mcl/sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_SDK_STATUS(call, expected) do { \
    const mcl_sdk_status_t st__ = (call); \
    if (st__ != (expected)) { \
        fprintf(stderr, "FAIL at %s:%d: %s returned %d, expected %d\n", \
                __FILE__, __LINE__, #call, (int)st__, (int)(expected)); \
        exit(1); \
    } \
} while (0)

#define CHECK_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL at %s:%d: (%s) is false\n", \
                __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

typedef struct {
    uint8_t buffer[256];
    size_t size;
    int fail_next;
    size_t call_count;
} memory_transport_t;

static int32_t memory_tx_callback(void *user, uint8_t transport_id,
                          const uint8_t *data, size_t data_size)
{
    memory_transport_t *tx = (memory_transport_t *)user;

    (void)transport_id;
    if (tx == NULL) {
        return -1;
    }
    tx->call_count++;
    if (tx->fail_next) {
        tx->fail_next = 0;
        return -2;
    }
    if (data_size > sizeof(tx->buffer)) {
        return -3;
    }
    memcpy(tx->buffer, data, data_size);
    tx->size = data_size;
    return 0;
}

static void test_hello_world_presence(void)
{
    mcl_node_t node_a;
    mcl_node_t node_b;
    mcl_node_config_t config_a;
    mcl_node_config_t config_b;
    memory_transport_t transport;
    uint8_t scratch[64];
    size_t bytes_sent = 0u;
    size_t bytes_consumed = 0u;
    mcl_wire_tier0_t send_obj;
    mcl_wire_tier0_t recv_obj;

    memset(&transport, 0, sizeof(transport));
    memset(&send_obj, 0, sizeof(send_obj));
    memset(&recv_obj, 0, sizeof(recv_obj));

    config_a.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config_a.tx_fn = memory_tx_callback;
    config_a.user_ctx = &transport;
    config_a.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config_a.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK_SDK_STATUS(mcl_node_init(&node_a, &config_a), MCL_SDK_OK);

    config_b.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config_b.tx_fn = NULL; /* receive-only node */
    config_b.user_ctx = NULL;
    config_b.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config_b.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK_SDK_STATUS(mcl_node_init(&node_b, &config_b), MCL_SDK_OK);

    send_obj.kind = MCL_WIRE_KIND_PRESENCE;
    send_obj.priority = 1u;
    send_obj.source_ref = 0x11223344u;
    send_obj.body.presence.machine_class = 2u;
    send_obj.body.presence.capability_tag = 0x887766u; /* 24-bit field */
    send_obj.body.presence.ttl = 15u;

    CHECK_SDK_STATUS(
        mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent),
        MCL_SDK_OK);

    CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_PRESENCE));
    CHECK_TRUE(transport.size == bytes_sent);
    CHECK_TRUE(transport.call_count == 1u);

    CHECK_SDK_STATUS(
        mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed),
        MCL_SDK_OK);

    CHECK_TRUE(bytes_consumed == bytes_sent);
    CHECK_TRUE(recv_obj.kind == send_obj.kind);
    CHECK_TRUE(recv_obj.priority == send_obj.priority);
    CHECK_TRUE(recv_obj.source_ref == send_obj.source_ref);
    CHECK_TRUE(recv_obj.body.presence.machine_class == send_obj.body.presence.machine_class);
    CHECK_TRUE(recv_obj.body.presence.capability_tag == send_obj.body.presence.capability_tag);
    CHECK_TRUE(recv_obj.body.presence.ttl == send_obj.body.presence.ttl);
}

static void test_all_six_tier0_semantics(void)
{
    mcl_node_t node_a;
    mcl_node_t node_b;
    mcl_node_config_t config_a;
    mcl_node_config_t config_b;
    memory_transport_t transport;
    uint8_t scratch[128];
    size_t bytes_sent = 0u;
    size_t bytes_consumed = 0u;

    memset(&transport, 0, sizeof(transport));
    config_a.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config_a.tx_fn = memory_tx_callback;
    config_a.user_ctx = &transport;
    config_a.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config_a.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK_SDK_STATUS(mcl_node_init(&node_a, &config_a), MCL_SDK_OK);

    config_b.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config_b.tx_fn = NULL;
    config_b.user_ctx = NULL;
    config_b.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config_b.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK_SDK_STATUS(mcl_node_init(&node_b, &config_b), MCL_SDK_OK);

    /* 1. PRESENCE */
    {
        mcl_wire_tier0_t send_obj, recv_obj;
        memset(&send_obj, 0, sizeof(send_obj));
        memset(&recv_obj, 0, sizeof(recv_obj));
        send_obj.kind = MCL_WIRE_KIND_PRESENCE;
        send_obj.priority = 0u;
        send_obj.source_ref = 0x1000u;
        send_obj.body.presence.machine_class = 5u;
        send_obj.body.presence.capability_tag = 0xabcdefu; /* 24-bit */
        send_obj.body.presence.ttl = 30u;

        CHECK_SDK_STATUS(mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent), MCL_SDK_OK);
        CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_PRESENCE));
        CHECK_SDK_STATUS(mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed), MCL_SDK_OK);
        CHECK_TRUE(bytes_consumed == bytes_sent);
        CHECK_TRUE(memcmp(&send_obj, &recv_obj, sizeof(send_obj)) == 0);
    }

    /* 2. HAZARD */
    {
        mcl_wire_tier0_t send_obj, recv_obj;
        memset(&send_obj, 0, sizeof(send_obj));
        memset(&recv_obj, 0, sizeof(recv_obj));
        send_obj.kind = MCL_WIRE_KIND_HAZARD;
        send_obj.priority = 3u;
        send_obj.source_ref = 0x2000u;
        send_obj.body.hazard.hazard_class = 1u;
        send_obj.body.hazard.severity = 4u;
        send_obj.body.hazard.confidence = 95u;
        send_obj.body.hazard.x = -150;
        send_obj.body.hazard.y = 320;
        send_obj.body.hazard.z = 10;
        send_obj.body.hazard.radius = 120u;
        send_obj.body.hazard.ttl = 3u;

        CHECK_SDK_STATUS(mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent), MCL_SDK_OK);
        CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_HAZARD));
        CHECK_SDK_STATUS(mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed), MCL_SDK_OK);
        CHECK_TRUE(bytes_consumed == bytes_sent);
        CHECK_TRUE(memcmp(&send_obj, &recv_obj, sizeof(send_obj)) == 0);
    }

    /* 3. REQUEST */
    {
        mcl_wire_tier0_t send_obj, recv_obj;
        memset(&send_obj, 0, sizeof(send_obj));
        memset(&recv_obj, 0, sizeof(recv_obj));
        send_obj.kind = MCL_WIRE_KIND_REQUEST;
        send_obj.priority = 2u;
        send_obj.source_ref = 0x3000u;
        send_obj.body.request.request_class = 7u;
        send_obj.body.request.target_ref = 0x4000u;
        send_obj.body.request.x = 25;
        send_obj.body.request.y = -50;
        send_obj.body.request.radius = 80u;
        send_obj.body.request.ttl = 12u;

        CHECK_SDK_STATUS(mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent), MCL_SDK_OK);
        CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_REQUEST));
        CHECK_SDK_STATUS(mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed), MCL_SDK_OK);
        CHECK_TRUE(bytes_consumed == bytes_sent);
        CHECK_TRUE(memcmp(&send_obj, &recv_obj, sizeof(send_obj)) == 0);
    }

    /* 4. AUTHORITY_CLAIM */
    {
        mcl_wire_tier0_t send_obj, recv_obj;
        memset(&send_obj, 0, sizeof(send_obj));
        memset(&recv_obj, 0, sizeof(recv_obj));
        send_obj.kind = MCL_WIRE_KIND_AUTHORITY_CLAIM;
        send_obj.priority = 1u;
        send_obj.source_ref = 0x4000u;
        send_obj.body.authority_claim.authority_class = 2u;
        send_obj.body.authority_claim.jurisdiction = 0x0102u;
        send_obj.body.authority_claim.credential_ref = 0x12345678u;
        send_obj.body.authority_claim.validity = 60u;

        CHECK_SDK_STATUS(mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent), MCL_SDK_OK);
        CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_AUTHORITY_CLAIM));
        CHECK_SDK_STATUS(mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed), MCL_SDK_OK);
        CHECK_TRUE(bytes_consumed == bytes_sent);
        CHECK_TRUE(memcmp(&send_obj, &recv_obj, sizeof(send_obj)) == 0);
    }

    /* 5. DEGRADED_STATE */
    {
        mcl_wire_tier0_t send_obj, recv_obj;
        memset(&send_obj, 0, sizeof(send_obj));
        memset(&recv_obj, 0, sizeof(recv_obj));
        send_obj.kind = MCL_WIRE_KIND_DEGRADED_STATE;
        send_obj.priority = 3u;
        send_obj.source_ref = 0x5000u;
        send_obj.body.degraded_state.affected_capability = 1u;
        send_obj.body.degraded_state.health = 25u;
        send_obj.body.degraded_state.severity = 3u;
        send_obj.body.degraded_state.ttl = 45u;

        CHECK_SDK_STATUS(mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent), MCL_SDK_OK);
        CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_DEGRADED_STATE));
        CHECK_SDK_STATUS(mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed), MCL_SDK_OK);
        CHECK_TRUE(bytes_consumed == bytes_sent);
        CHECK_TRUE(memcmp(&send_obj, &recv_obj, sizeof(send_obj)) == 0);
    }

    /* 6. TRANSPORT_OFFER */
    {
        mcl_wire_tier0_t send_obj, recv_obj;
        memset(&send_obj, 0, sizeof(send_obj));
        memset(&recv_obj, 0, sizeof(recv_obj));
        send_obj.kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
        send_obj.priority = 0u;
        send_obj.source_ref = 0x6000u;
        /* migration_ref must be set explicitly: the round trip compares the
         * whole object, so an uninitialised field would make the result
         * depend on stack residue. */
        send_obj.body.transport_offer.migration_ref = UINT32_C(0x4D194201);
        send_obj.body.transport_offer.transport_id = 2u;
        send_obj.body.transport_offer.profile_id = 1u;
        send_obj.body.transport_offer.endpoint_token = 0xabcdef01u;
        send_obj.body.transport_offer.validity = 120u;

        CHECK_SDK_STATUS(mcl_node_send_tier0(&node_a, &send_obj, scratch, sizeof(scratch), &bytes_sent), MCL_SDK_OK);
        CHECK_TRUE(bytes_sent == mcl_wire_tier0_encoded_size(MCL_WIRE_KIND_TRANSPORT_OFFER));
        CHECK_SDK_STATUS(mcl_node_receive_tier0(&node_b, transport.buffer, transport.size, &recv_obj, &bytes_consumed), MCL_SDK_OK);
        CHECK_TRUE(bytes_consumed == bytes_sent);
        CHECK_TRUE(memcmp(&send_obj, &recv_obj, sizeof(send_obj)) == 0);
    }
}

static void test_negative_cases(void)
{
    mcl_node_t node;
    mcl_node_config_t config = {0};
    /* transport_id 0 is the reserved value, so it must be set explicitly. */
    memory_transport_t transport;
    uint8_t scratch[64];
    uint8_t wire_bytes[64];
    size_t written = 0u;
    size_t consumed = 0u;
    mcl_wire_tier0_t obj;

    memset(&transport, 0, sizeof(transport));
    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = 1u;
    obj.body.presence.machine_class = 1u;
    obj.body.presence.capability_tag = 0x123u;
    obj.body.presence.ttl = 10u;

    /* 1. NULL node init */
    CHECK_SDK_STATUS(mcl_node_init(NULL, &config), MCL_SDK_ERR_INVALID_ARGUMENT);
    CHECK_SDK_STATUS(mcl_node_init(&node, NULL), MCL_SDK_ERR_INVALID_ARGUMENT);

    config.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config.tx_fn = memory_tx_callback;
    config.user_ctx = &transport;
    config.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK_SDK_STATUS(mcl_node_init(&node, &config), MCL_SDK_OK);

    /* 2. NULL send arguments */
    CHECK_SDK_STATUS(mcl_node_send_tier0(NULL, &obj, scratch, sizeof(scratch), &written), MCL_SDK_ERR_INVALID_ARGUMENT);
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, NULL, scratch, sizeof(scratch), &written), MCL_SDK_ERR_INVALID_ARGUMENT);
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &obj, NULL, sizeof(scratch), &written), MCL_SDK_ERR_INVALID_ARGUMENT);
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &obj, scratch, 0u, &written), MCL_SDK_ERR_INVALID_ARGUMENT);

    /* 3. Scratch capacity too small */
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &obj, scratch, 4u, &written), MCL_SDK_ERR_BUFFER_TOO_SMALL);

    /* 4. Missing TX callback */
    {
        mcl_node_t rx_only;
        mcl_node_config_t rx_config;
        rx_config.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
        rx_config.tx_fn = NULL;
        rx_config.user_ctx = NULL;
        rx_config.transport_id = MCL_CONTACT_TRANSPORT_AP;
        rx_config.role = MCL_CONTACT_ROLE_INITIATOR;
        CHECK_SDK_STATUS(mcl_node_init(&rx_only, &rx_config), MCL_SDK_OK);
        CHECK_SDK_STATUS(mcl_node_send_tier0(&rx_only, &obj, scratch, sizeof(scratch), &written), MCL_SDK_ERR_TX_UNAVAILABLE);
    }

    /* 5. TX callback failure */
    transport.fail_next = 1;
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &obj, scratch, sizeof(scratch), &written), MCL_SDK_ERR_TX_NOT_SENT);

    /* 6. NULL receive arguments */
    CHECK_SDK_STATUS(mcl_node_receive_tier0(NULL, scratch, 10u, &obj, &consumed), MCL_SDK_ERR_INVALID_ARGUMENT);
    CHECK_SDK_STATUS(mcl_node_receive_tier0(&node, NULL, 10u, &obj, &consumed), MCL_SDK_ERR_INVALID_ARGUMENT);
    CHECK_SDK_STATUS(mcl_node_receive_tier0(&node, scratch, 10u, NULL, &consumed), MCL_SDK_ERR_INVALID_ARGUMENT);

    /* 7. Truncated wire object */
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &obj, scratch, sizeof(scratch), &written), MCL_SDK_OK);
    CHECK_TRUE(written > 4u);
    CHECK_SDK_STATUS(mcl_node_receive_tier0(&node, scratch, written - 2u, &obj, &consumed), MCL_SDK_ERR_BUFFER_TOO_SMALL);

    /* 8. Malformed wire object: corrupt opcode */
    memcpy(wire_bytes, scratch, written);
    wire_bytes[1] ^= 0xF0u; /* alter opcode to non-existent opcode */
    CHECK_SDK_STATUS(mcl_node_receive_tier0(&node, wire_bytes, written, &obj, &consumed), MCL_SDK_ERR_WIRE_FAILURE);

    /* 9. Unknown/unsupported semantic kind */
    obj.kind = 99u;
    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &obj, scratch, sizeof(scratch), &written), MCL_SDK_ERR_WIRE_FAILURE);
}

static void test_policy_sovereignty_and_trust_invariants(void)
{
    mcl_node_t node;
    mcl_node_config_t config;
    memory_transport_t transport;
    uint8_t scratch[64];
    size_t written = 0u;
    size_t consumed = 0u;
    mcl_wire_tier0_t auth_claim_obj;
    mcl_wire_tier0_t decoded_claim;
    uint8_t has_ctx = 0u;
    mcl_link_context_key_t test_key;

    memset(&transport, 0, sizeof(transport));
    config.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config.tx_fn = memory_tx_callback;
    config.user_ctx = &transport;
    config.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK_SDK_STATUS(mcl_node_init(&node, &config), MCL_SDK_OK);

    /*
     * Invariant: Receiving an AUTHORITY_CLAIM must produce a decoded object and nothing more.
     * It MUST NOT mutate Link state, install or authorize context, or grant any local privileges.
     */
    memset(&auth_claim_obj, 0, sizeof(auth_claim_obj));
    auth_claim_obj.kind = MCL_WIRE_KIND_AUTHORITY_CLAIM;
    auth_claim_obj.priority = 3u; /* highest priority */
    auth_claim_obj.source_ref = 0xFEEDFACEu;
    auth_claim_obj.body.authority_claim.authority_class = 1u;
    auth_claim_obj.body.authority_claim.jurisdiction = 0x0042u;
    auth_claim_obj.body.authority_claim.credential_ref = 0xCAFEBABEu;
    auth_claim_obj.body.authority_claim.validity = 255u;

    CHECK_SDK_STATUS(mcl_node_send_tier0(&node, &auth_claim_obj, scratch, sizeof(scratch), &written), MCL_SDK_OK);
    CHECK_SDK_STATUS(mcl_node_receive_tier0(&node, transport.buffer, transport.size, &decoded_claim, &consumed), MCL_SDK_OK);

    /* Check link context state remains strictly unauthorized and uninstalled */
    CHECK_TRUE(mcl_node_get_link(&node)->state == MCL_LINK_STATE_IDLE);
    CHECK_TRUE(mcl_link_has_active_context(&node.link, &has_ctx) == MCL_LINK_OK);
    CHECK_TRUE(has_ctx == 0u);

    memset(&test_key, 0, sizeof(test_key));
    test_key.wire_major = 0u;
    test_key.context_id = 1u;
    test_key.ruleset_digest_size = 1u;
    test_key.ruleset_digest[0] = 0xAAu;
    CHECK_TRUE(mcl_node_link_authorize_context(&node, &test_key) == MCL_SDK_ERR_LINK_FAILURE);
}

static void test_link_sdk_integration(void)
{
    mcl_node_t node;
    mcl_node_config_t config;
    mcl_link_context_key_t key;

    memset(&key, 0, sizeof(key));
    key.wire_major = 0u;
    key.context_id = 0x1234u;
    key.generation = 1u;
    key.ruleset_digest_size = 16u;
    key.ruleset_digest[0] = 0x55u;

    config.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    config.tx_fn = NULL;
    config.user_ctx = NULL;
    config.transport_id = MCL_CONTACT_TRANSPORT_AP;
    config.role = MCL_CONTACT_ROLE_INITIATOR;

    /* 1. Node init -> link in IDLE */
    CHECK_SDK_STATUS(mcl_node_init(&node, &config), MCL_SDK_OK);
    CHECK_TRUE(mcl_node_get_link(&node)->state == MCL_LINK_STATE_IDLE);

    /* 2. Lifecycle sequence: IDLE -> DISCOVERED -> CAPABILITIES -> NEGOTIATING */
    CHECK_SDK_STATUS(mcl_node_link_transition(&node, MCL_LINK_STATE_DISCOVERED), MCL_SDK_OK);
    CHECK_SDK_STATUS(mcl_node_link_transition(&node, MCL_LINK_STATE_CAPABILITIES), MCL_SDK_OK);
    CHECK_SDK_STATUS(mcl_node_link_transition(&node, MCL_LINK_STATE_NEGOTIATING), MCL_SDK_OK);

    /* 3. Install context in NEGOTIATING */
    CHECK_SDK_STATUS(mcl_node_link_install_context(&node, &key), MCL_SDK_OK);

    /* 4. NEGOTIATING -> ESTABLISHED */
    CHECK_SDK_STATUS(mcl_node_link_transition(&node, MCL_LINK_STATE_ESTABLISHED), MCL_SDK_OK);

    /* 5. Authorize exact context -> OK */
    CHECK_SDK_STATUS(mcl_node_link_authorize_context(&node, &key), MCL_SDK_OK);

    /* 6. Reset node -> link returns to IDLE, context cleared */
    CHECK_SDK_STATUS(mcl_node_reset(&node), MCL_SDK_OK);
    CHECK_TRUE(mcl_node_get_link(&node)->state == MCL_LINK_STATE_IDLE);

    /* 7. Authorize old context after reset -> failure */
    CHECK_SDK_STATUS(mcl_node_link_authorize_context(&node, &key), MCL_SDK_ERR_LINK_FAILURE);
}

int main(void)
{
    test_hello_world_presence();
    test_all_six_tier0_semantics();
    test_negative_cases();
    test_policy_sovereignty_and_trust_invariants();
    test_link_sdk_integration();

    puts("mcl_sdk test suite: ALL PASS");
    return 0;
}

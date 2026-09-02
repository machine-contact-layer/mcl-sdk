/*
 * MCL SDK framed contact path tests.
 *
 * Covers the Link-framed send and receive path, and the policy-sovereignty
 * property the charter requires: receiving a frame changes no state and grants
 * no authority.
 */

#include "mcl/sdk.h"

#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do {                                      \
    ++tests_run;                                                   \
    if (!(cond)) {                                                 \
        ++tests_failed;                                            \
        printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                              \
} while (0)

/* Capture transport: records the last frame handed to it. */
typedef struct {
    uint8_t buffer[256];
    size_t size;
    unsigned calls;
    int32_t result;
} capture_tx_t;

static int32_t capture_tx(void *user, const uint8_t *data, size_t data_size)
{
    capture_tx_t *c = (capture_tx_t *)user;
    size_t i;

    c->calls++;
    if (c->result != 0) {
        return c->result;
    }
    if (data_size > sizeof(c->buffer)) {
        return -1;
    }
    for (i = 0u; i < data_size; ++i) {
        c->buffer[i] = data[i];
    }
    c->size = data_size;
    return 0;
}

static void make_presence(mcl_wire_tier0_t *obj)
{
    memset(obj, 0, sizeof(*obj));
    obj->kind = MCL_WIRE_KIND_PRESENCE;
    obj->priority = 1u;
    obj->source_ref = 0x00000001u;
    obj->body.presence.machine_class = 1u;
    obj->body.presence.capability_digest = 0x000001u;
    obj->body.presence.ttl = 60u;
}

static void init_node(mcl_node_t *node, capture_tx_t *cap)
{
    mcl_node_config_t cfg;

    memset(cap, 0, sizeof(*cap));
    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = capture_tx;
    cfg.user_ctx = cap;
    cfg.source_ref = 0xABCD1234u;

    CHECK(mcl_node_init(node, &cfg) == MCL_SDK_OK, "node init");
}

static void test_framed_round_trip(void)
{
    mcl_node_t tx_node, rx_node;
    capture_tx_t cap, rx_cap;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u, consumed = 0u;

    printf("[TEST] framed Tier-0 round trip\n");

    init_node(&tx_node, &cap);
    init_node(&rx_node, &rx_cap);
    make_presence(&obj);

    CHECK(mcl_node_send_framed_tier0(&tx_node, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_INTEGRITY,
                                     scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
          "framed send");
    CHECK(cap.calls == 1u, "transport called once");
    CHECK(cap.size == sent, "transport received the whole frame");

    CHECK(mcl_node_receive_framed(&rx_node, cap.buffer, cap.size, &frame,
                                  &decoded, &has_object, &consumed) == MCL_SDK_OK,
          "framed receive");
    CHECK(consumed == cap.size, "consumed the whole frame");
    CHECK(has_object == 1u, "object recovered");
    CHECK(frame.frame_class == MCL_LINK_CLASS_DATA, "frame class preserved");
    CHECK(frame.source_ref == 0xABCD1234u, "source ref carried");
    CHECK(decoded.kind == MCL_WIRE_KIND_PRESENCE, "kind preserved");
    CHECK(decoded.body.presence.ttl == 60u, "ttl preserved");
    CHECK(decoded.body.presence.machine_class == 1u, "machine class preserved");
}

static void test_sequence_advances_only_on_success(void)
{
    mcl_node_t node;
    capture_tx_t cap;
    mcl_wire_tier0_t obj;
    uint8_t scratch[256];
    size_t sent = 0u;
    unsigned i;

    printf("[TEST] sequence advances only when the transport accepted\n");

    init_node(&node, &cap);
    make_presence(&obj);

    for (i = 0u; i < 3u; ++i) {
        mcl_link_frame_t frame;
        size_t consumed = 0u;
        mcl_wire_tier0_t decoded;
        uint8_t has_object = 0u;

        CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA,
                                         MCL_LINK_FLAG_SEQUENCE,
                                         scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
              "send with sequence");
        CHECK(mcl_node_receive_framed(&node, cap.buffer, cap.size, &frame,
                                      &decoded, &has_object, &consumed) == MCL_SDK_OK,
              "receive back");
        CHECK(frame.sequence == (uint16_t)i, "sequence increments per frame");
    }

    /* A transport failure must not consume a sequence number. */
    cap.result = -7;
    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_SEQUENCE,
                                     scratch, sizeof(scratch), &sent)
              == MCL_SDK_ERR_TX_FAILURE,
          "transport failure reported");
    CHECK(node.tx_sequence == 3u, "sequence not advanced on failure");

    cap.result = 0;
    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_SEQUENCE,
                                     scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
          "send succeeds after recovery");
    CHECK(node.tx_sequence == 4u, "sequence resumes without a gap");
}

static void test_session_flag_requires_context(void)
{
    mcl_node_t node;
    capture_tx_t cap;
    mcl_wire_tier0_t obj;
    mcl_link_context_key_t key;
    uint8_t scratch[256];
    size_t sent = 0u;

    printf("[TEST] a session reference requires an installed context\n");

    init_node(&node, &cap);
    make_presence(&obj);

    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_SESSION,
                                     scratch, sizeof(scratch), &sent)
              == MCL_SDK_ERR_INVALID_STATE,
          "session flag refused with no context");

    /* Install a context through the ordinary lifecycle. */
    CHECK(mcl_node_link_transition(&node, MCL_LINK_STATE_DISCOVERED) == MCL_SDK_OK,
          "to DISCOVERED");
    CHECK(mcl_node_link_transition(&node, MCL_LINK_STATE_CAPABILITIES) == MCL_SDK_OK,
          "to CAPABILITIES");
    CHECK(mcl_node_link_transition(&node, MCL_LINK_STATE_NEGOTIATING) == MCL_SDK_OK,
          "to NEGOTIATING");

    memset(&key, 0, sizeof(key));
    key.wire_major = 0u;
    key.context_id = 0x0000BEEFu;
    key.generation = 1u;
    key.ruleset_digest_size = 4u;
    key.ruleset_digest[0] = 0xDEu;
    CHECK(mcl_node_link_install_context(&node, &key) == MCL_SDK_OK, "install context");

    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_SESSION,
                                     scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
          "session flag accepted with a context");

    {
        mcl_link_frame_t frame;
        mcl_wire_tier0_t decoded;
        uint8_t has_object = 0u;
        size_t consumed = 0u;
        CHECK(mcl_node_receive_framed(&node, cap.buffer, cap.size, &frame,
                                      &decoded, &has_object, &consumed) == MCL_SDK_OK,
              "receive session frame");
        CHECK(frame.session_ref == 0x0000BEEFu,
              "session ref comes from the installed context");
    }
}

static void test_receive_has_no_side_effects(void)
{
    mcl_node_t node;
    capture_tx_t cap;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u, consumed = 0u;
    mcl_link_state_t state_before;

    printf("[TEST] receiving a frame changes no state and grants no authority\n");

    init_node(&node, &cap);

    /* An AUTHORITY_CLAIM is the case that matters most. */
    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_AUTHORITY_CLAIM;
    obj.priority = 3u;
    obj.source_ref = 0x99999999u;

    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_NEGOTIATION,
                                     0u, scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
          "send authority claim");

    state_before = mcl_node_get_link_const(&node)->state;
    CHECK(mcl_node_receive_framed(&node, cap.buffer, cap.size, &frame,
                                  &decoded, &has_object, &consumed) == MCL_SDK_OK,
          "receive authority claim");
    CHECK(has_object == 1u, "claim decoded as an object");
    CHECK(decoded.kind == MCL_WIRE_KIND_AUTHORITY_CLAIM, "claim kind preserved");
    CHECK(mcl_node_get_link_const(&node)->state == state_before,
          "link state unchanged by reception");

    {
        uint8_t has_context = 1u;
        CHECK(mcl_link_has_active_context(mcl_node_get_link_const(&node),
                                          &has_context) == MCL_LINK_OK,
              "context query");
        CHECK(has_context == 0u, "no context installed by reception");
    }
}

static void test_non_semantic_classes_are_not_decoded(void)
{
    mcl_node_t node;
    capture_tx_t cap;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 1u;
    size_t sent = 0u, consumed = 0u;

    printf("[TEST] non-semantic frame classes yield no object\n");

    init_node(&node, &cap);
    make_presence(&obj);

    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_KEEPALIVE,
                                     0u, scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
          "send keepalive");
    CHECK(mcl_node_receive_framed(&node, cap.buffer, cap.size, &frame,
                                  &decoded, &has_object, &consumed) == MCL_SDK_OK,
          "receive keepalive");
    CHECK(has_object == 0u,
          "a keepalive carries no semantics, so none are manufactured");
    CHECK(frame.frame_class == MCL_LINK_CLASS_KEEPALIVE, "class preserved");
}

static void test_malformed_frame_never_reaches_wire(void)
{
    mcl_node_t node;
    capture_tx_t cap;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t corrupt[256];
    uint8_t has_object = 1u;
    size_t sent = 0u, consumed = 0u, i;

    printf("[TEST] a malformed frame is rejected before semantic decoding\n");

    init_node(&node, &cap);
    make_presence(&obj);
    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_INTEGRITY,
                                     scratch, sizeof(scratch), &sent) == MCL_SDK_OK,
          "send for corruption test");

    for (i = 0u; i < cap.size; ++i) {
        memcpy(corrupt, cap.buffer, cap.size);
        corrupt[i] ^= 0x01u;
        has_object = 1u;
        if (mcl_node_receive_framed(&node, corrupt, cap.size, &frame,
                                    &decoded, &has_object, &consumed) != MCL_SDK_OK) {
            CHECK(has_object == 0u, "no object reported on a rejected frame");
        }
    }

    /* Truncation at every length. */
    for (i = 0u; i < cap.size; ++i) {
        CHECK(mcl_node_receive_framed(&node, cap.buffer, i, &frame,
                                      &decoded, &has_object, &consumed)
                  == MCL_SDK_ERR_FRAME_FAILURE,
              "truncated frame rejected");
    }
}

static void test_argument_validation(void)
{
    mcl_node_t node;
    capture_tx_t cap;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u, consumed = 0u;

    printf("[TEST] argument validation\n");

    init_node(&node, &cap);
    make_presence(&obj);

    CHECK(mcl_node_send_framed_tier0(NULL, &obj, MCL_LINK_CLASS_DATA, 0u,
                                     scratch, sizeof(scratch), &sent)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "null node");
    CHECK(mcl_node_send_framed_tier0(&node, NULL, MCL_LINK_CLASS_DATA, 0u,
                                     scratch, sizeof(scratch), &sent)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "null object");
    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_DATA, 0u,
                                     scratch, 4u, &sent)
              == MCL_SDK_ERR_FRAME_FAILURE, "scratch too small");
    CHECK(mcl_node_send_framed_tier0(&node, &obj, (mcl_link_frame_class_t)15u, 0u,
                                     scratch, sizeof(scratch), &sent)
              == MCL_SDK_ERR_FRAME_FAILURE, "unassigned frame class refused");
    CHECK(mcl_node_receive_framed(NULL, scratch, 8u, &frame, &decoded,
                                  &has_object, &consumed)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "null node on receive");
    CHECK(mcl_node_receive_framed(&node, NULL, 8u, &frame, &decoded,
                                  &has_object, &consumed)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "null data on receive");

    /* A node with no transport cannot send. */
    {
        mcl_node_t rx_only;
        mcl_node_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
        CHECK(mcl_node_init(&rx_only, &cfg) == MCL_SDK_OK, "receive-only node init");
        CHECK(mcl_node_send_framed_tier0(&rx_only, &obj, MCL_LINK_CLASS_DATA, 0u,
                                         scratch, sizeof(scratch), &sent)
                  == MCL_SDK_ERR_TX_UNAVAILABLE, "no transport reported");
    }
}

/*
 * A Link frame declares its payload length exactly. If the semantic object
 * inside ends before that length, the payload carries bytes nobody declared,
 * and accepting the object while ignoring them would let two implementations
 * disagree about what was sent while both reported success.
 */
static void test_payload_boundary_must_be_exact(void)
{
    mcl_node_t rx_node;
    mcl_node_config_t cfg;
    mcl_wire_tier0_t object, decoded;
    mcl_link_frame_t frame;
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    uint8_t padded[MCL_WIRE_TIER0_MAX_SIZE + 4];
    uint8_t raw[256];
    size_t wire_written = 0u, written = 0u, consumed = 0u;
    uint8_t has_object = 9u;
    size_t i;

    printf("[TEST] a payload longer than the object it carries is rejected\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.source_ref = 0x55u;
    CHECK(mcl_node_init(&rx_node, &cfg) == MCL_SDK_OK, "receiver init");

    make_presence(&object);
    CHECK(mcl_wire_tier0_encode(&object, wire_buf, sizeof(wire_buf), &wire_written)
              == MCL_WIRE_OK,
          "object encodes");

    /* Same object, but the frame declares three undeclared trailing bytes. */
    memcpy(padded, wire_buf, wire_written);
    for (i = 0u; i < 3u; ++i) {
        padded[wire_written + i] = 0x00u;
    }

    memset(&frame, 0, sizeof(frame));
    frame.frame_class = MCL_LINK_CLASS_CONTACT;
    frame.source_ref = 0x77u;
    frame.payload = padded;
    frame.payload_len = (uint16_t)(wire_written + 3u);
    CHECK(mcl_link_frame_encode(&frame, raw, sizeof(raw), &written) == MCL_LINK_OK,
          "over-long payload still forms a valid frame");

    CHECK(mcl_node_receive_framed(&rx_node, raw, written, &frame, &decoded,
                                  &has_object, &consumed) == MCL_SDK_ERR_WIRE_FAILURE,
          "trailing bytes inside the payload are rejected");
    CHECK(has_object == 0u, "no object is reported for a rejected payload");

    /* The identical frame with an exact payload length still decodes. */
    memset(&frame, 0, sizeof(frame));
    frame.frame_class = MCL_LINK_CLASS_CONTACT;
    frame.source_ref = 0x77u;
    frame.payload = wire_buf;
    frame.payload_len = (uint16_t)wire_written;
    CHECK(mcl_link_frame_encode(&frame, raw, sizeof(raw), &written) == MCL_LINK_OK,
          "exact frame encodes");
    CHECK(mcl_node_receive_framed(&rx_node, raw, written, &frame, &decoded,
                                  &has_object, &consumed) == MCL_SDK_OK,
          "the same object with an exact length decodes");
    CHECK(has_object == 1u, "object reported");
}

/*
 * A caller that omits `consumed` has made an argument error, not sent a bad
 * frame. Reporting a frame failure would send them looking at the wire.
 */
static void test_missing_consumed_is_an_argument_error(void)
{
    mcl_node_t rx_node;
    mcl_node_config_t cfg;
    mcl_wire_tier0_t decoded;
    mcl_link_frame_t frame;
    uint8_t raw[64];
    uint8_t has_object = 0u;

    printf("[TEST] a missing consumed pointer is reported as an argument error\n");

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    CHECK(mcl_node_init(&rx_node, &cfg) == MCL_SDK_OK, "init");
    memset(raw, 0, sizeof(raw));

    CHECK(mcl_node_receive_framed(&rx_node, raw, sizeof(raw), &frame, &decoded,
                                  &has_object, NULL) == MCL_SDK_ERR_INVALID_ARGUMENT,
          "null consumed is an argument error, not a frame failure");
}

int main(void)
{
    printf("MCL SDK framed contact path tests\n");
    printf("=================================\n");

    test_framed_round_trip();
    test_sequence_advances_only_on_success();
    test_session_flag_requires_context();
    test_receive_has_no_side_effects();
    test_non_semantic_classes_are_not_decoded();
    test_malformed_frame_never_reaches_wire();
    test_payload_boundary_must_be_exact();
    test_missing_consumed_is_an_argument_error();
    test_argument_validation();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return (tests_failed == 0) ? 0 : 1;
}

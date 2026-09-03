/*
 * An external MCL consumer.
 *
 * This file is written the way a real integrator writes one: it includes public
 * headers by their installed paths, calls the public API, and knows nothing
 * about how MCL is built. It does not reach into any repository, does not
 * include a private header, and does not link a source file.
 *
 * If this program compiles, links and runs against an install prefix, then the
 * package is genuinely installable. If it needs anything from the source tree,
 * the package is not.
 *
 * It exercises one path through every layer -- Wire, Link, contact and the
 * node -- because a consumer test that only called one function would prove the
 * headers exist and nothing about whether the library is usable.
 */

#include "mcl/sdk.h"
#include "mcl/negotiation.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what) do {                                  \
    ++checks;                                                   \
    if (!(cond)) {                                              \
        ++failures;                                             \
        printf("  FAIL: %s\n", (what));                         \
    }                                                           \
} while (0)

/* Captures whatever the node transmits, so the test can look at real bytes. */
typedef struct {
    uint8_t buffer[256];
    size_t size;
    uint8_t transport;
    unsigned sends;
} capture_t;

static int32_t capture_tx(void *user, uint8_t transport_id,
                          const uint8_t *data, size_t data_size)
{
    capture_t *cap = (capture_t *)user;
    size_t i;

    if (data_size > sizeof(cap->buffer)) {
        return -1;
    }
    cap->sends++;
    cap->transport = transport_id;
    for (i = 0u; i < data_size; ++i) {
        cap->buffer[i] = data[i];
    }
    cap->size = data_size;
    return 0;
}

static void test_wire_layer(void)
{
    mcl_wire_tier0_t presence;
    mcl_wire_tier0_t decoded;
    uint8_t bytes[MCL_WIRE_TIER0_MAX_SIZE];
    size_t written = 0u;
    size_t consumed = 0u;

    printf("[consumer] Wire: encode and decode a PRESENCE\n");

    memset(&presence, 0, sizeof(presence));
    presence.kind = MCL_WIRE_KIND_PRESENCE;
    presence.priority = 1u;
    presence.source_ref = 0x0BADCAFEu;
    presence.body.presence.machine_class = 1u;
    presence.body.presence.capability_tag = 0x00ABCDu;
    presence.body.presence.ttl = 60u;

    CHECK(mcl_wire_tier0_encode(&presence, bytes, sizeof(bytes), &written) ==
              MCL_WIRE_OK,
          "PRESENCE encodes");
    CHECK(written == 11u, "PRESENCE is 11 bytes");
    CHECK(mcl_wire_tier0_decode(bytes, written, &decoded, &consumed) ==
              MCL_WIRE_OK,
          "PRESENCE decodes");
    CHECK(consumed == written, "every byte consumed");
    CHECK(decoded.source_ref == presence.source_ref, "source_ref survives");
    CHECK(decoded.body.presence.capability_tag ==
              presence.body.presence.capability_tag,
          "capability_tag survives");

    /* The shared duration encoding, which a consumer needs to interpret ttl. */
    CHECK(mcl_wire_duration_seconds(0u) == 0u, "duration zero is zero");
    {
        uint8_t code = 0u;
        CHECK(mcl_wire_duration_encode(3600u, &code) == MCL_WIRE_OK,
              "an hour encodes");
        CHECK(mcl_wire_duration_seconds(code) <= 3600u,
              "and rounds down, never up");
    }

    /* The Stable-major rule is part of the public contract. */
    CHECK(mcl_wire_kind_allowed_at_major(MCL_WIRE_STABLE_MAJOR,
                                         MCL_WIRE_KIND_PRESENCE) == 1,
          "PRESENCE is Stable at the Stable major");
    CHECK(mcl_wire_kind_allowed_at_major(MCL_WIRE_STABLE_MAJOR,
                                         MCL_WIRE_KIND_HAZARD) == 0,
          "HAZARD is not");
}

static void test_link_layer(void)
{
    mcl_link_capability_t local;
    mcl_link_capability_t peer;
    mcl_link_negotiation_t selection;
    uint8_t encoded[MCL_LINK_CAPABILITY_SIZE];
    size_t written = 0u;

    printf("[consumer] Link: negotiate a version and a frame size\n");

    CHECK(mcl_link_make_capability(&local, 0x0003u, 0x0001u, 1048u, 0u) ==
              MCL_LINK_OK,
          "local capability builds");
    CHECK(mcl_link_make_capability(&peer, 0x0001u, 0x0001u, 512u, 0u) ==
              MCL_LINK_OK,
          "peer capability builds");
    CHECK(mcl_link_capability_encode(&local, encoded, sizeof(encoded),
                                     &written) == MCL_LINK_OK,
          "capability encodes");
    CHECK(written == MCL_LINK_CAPABILITY_SIZE, "to its exact size");

    CHECK(mcl_link_negotiation_select(&local, &peer, &selection) ==
              MCL_LINK_OK,
          "a selection is reached");
    CHECK(selection.wire_major == 0u, "the highest common Wire major");
    CHECK(selection.max_frame == 512u, "the smaller frame limit");
}

static void test_node_send_and_receive(void)
{
    mcl_node_t node;
    mcl_node_config_t cfg;
    capture_t cap;
    mcl_wire_tier0_t obj;
    mcl_wire_tier0_t decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u;
    size_t consumed = 0u;

    printf("[consumer] SDK: build a node, send a frame, receive it back\n");

    memset(&cap, 0, sizeof(cap));
    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = capture_tx;
    cfg.user_ctx = &cap;
    cfg.source_ref = 0x11223344u;
    cfg.transport_id = MCL_CONTACT_TRANSPORT_IP;
    cfg.role = MCL_CONTACT_ROLE_INITIATOR;

    CHECK(mcl_node_init(&node, &cfg) == MCL_SDK_OK, "node initialises");

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = cfg.source_ref;
    obj.body.presence.machine_class = 1u;
    obj.body.presence.capability_tag = 0x000001u;
    obj.body.presence.ttl = 30u;

    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_CONTACT,
                                     MCL_LINK_FLAG_FRAME_CHECK,
                                     scratch, sizeof(scratch), &sent) ==
              MCL_SDK_OK,
          "a framed PRESENCE is sent");
    CHECK(cap.sends == 1u, "the transport callback ran once");
    CHECK(cap.transport == MCL_CONTACT_TRANSPORT_IP,
          "on the transport the node was configured for");
    CHECK(cap.size > 0u, "and carried bytes");

    CHECK(mcl_node_receive_framed(&node, cap.transport, cap.buffer, cap.size,
                                  &frame, &decoded, &has_object, &consumed) ==
              MCL_SDK_OK,
          "the same bytes decode");
    CHECK(has_object == 1u, "an object came back");
    CHECK(decoded.kind == MCL_WIRE_KIND_PRESENCE, "and it is a PRESENCE");
    CHECK(consumed == cap.size, "every byte consumed");
}

static void test_refusals_reach_the_consumer(void)
{
    mcl_wire_tier0_t decoded;
    size_t consumed = 0u;
    uint8_t junk[8];
    size_t i;

    printf("[consumer] failures are reported, not swallowed\n");

    for (i = 0u; i < sizeof(junk); ++i) {
        junk[i] = 0xFFu;
    }
    CHECK(mcl_wire_tier0_decode(junk, sizeof(junk), &decoded, &consumed) !=
              MCL_WIRE_OK,
          "garbage does not decode");

    CHECK(mcl_wire_tier0_decode(NULL, 4u, &decoded, &consumed) ==
              MCL_WIRE_ERR_INVALID_ARGUMENT,
          "a null buffer is an argument error");
}

int main(void)
{
    printf("=== MCL external consumer ===\n");
    printf("Built against an install prefix with no knowledge of the\n"
           "repository layout.\n\n");

    test_wire_layer();
    test_link_layer();
    test_node_send_and_receive();
    test_refusals_reach_the_consumer();

    printf("\n%d checks, %d failed.\n", checks, failures);
    if (failures != 0) {
        printf("EXTERNAL CONSUMER FAILED\n");
        return 1;
    }
    printf("EXTERNAL CONSUMER OK\n");
    return 0;
}

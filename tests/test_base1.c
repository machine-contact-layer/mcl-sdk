/*
 * MCL Base 1 on an arranged bearer, and the compatibility guarantee that had
 * to hold while it was added.
 *
 * Two things are being protected here.
 *
 * FIRST, that Base 1 is reachable at all. Before this existed the only named
 * deployment was Stranger-Contact, mcl_machine_init() demanded a
 * candidate_open callback that an arranged-bearer machine would never call,
 * and no node-level send path could put Wire major 1 inside a Link major 1
 * frame -- which is what section 4.1 of conformance-profiles-v1.md requires.
 * An integrator reading "Base 1 is the v1.0 stable floor" could not build one.
 *
 * SECOND, that adding it changed nothing already on the wire.
 * mcl_node_send_framed_tier0() must still emit the experimental pair byte for
 * byte, because that is what every pre-major-1 caller and every retained
 * receipt contains -- including the 104-migration continuity campaign, whose
 * ordinary traffic is major-0 PRESENCE. A silent retarget would break
 * compatibility in the one way nobody can see.
 */

#include "mcl/machine.h"
#include "mcl/sdk.h"

#include <stdio.h>
#include <string.h>

#define BEARER_IP  MCL_CONTACT_TRANSPORT_IP
#define FRAME_MAX  MCL_LINK_FRAME_MAX_SIZE

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures += 1;                                                 \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------- arranged bearer */

typedef struct {
    uint8_t frame[8][FRAME_MAX];
    size_t len[8];
    unsigned count;
} inbox_t;

typedef struct {
    inbox_t *peer_inbox;
    uint32_t clock;
} bearer_t;

static uint32_t b_clock(void *user)
{
    bearer_t *b = (bearer_t *)user;
    b->clock += 1u;
    return b->clock;
}

static int32_t b_send(void *user, uint8_t transport_id,
                      const uint8_t *data, size_t size)
{
    bearer_t *b = (bearer_t *)user;
    inbox_t *in = b->peer_inbox;

    (void)transport_id;
    if (size > FRAME_MAX || in->count >= 8u) {
        return -1;
    }
    memcpy(in->frame[in->count], data, size);
    in->len[in->count] = size;
    in->count += 1u;
    return 0;
}

static void drain(mcl_machine_t *m, inbox_t *in)
{
    unsigned i;
    for (i = 0u; i < in->count; ++i) {
        (void)mcl_machine_receive(m, BEARER_IP, in->frame[i], in->len[i]);
    }
    in->count = 0u;
}

/* ------------------------------------------------------------------ tests */

/*
 * A Base 1 machine initialises with no candidate_open, no randomness and no
 * medium sensing, because it discovers nothing and opens nothing.
 */
static void test_init_without_rendezvous_machinery(void)
{
    mcl_machine_t m;
    mcl_machine_config_t cfg;
    mcl_platform_t plat;
    inbox_t sink;
    bearer_t bear;

    memset(&sink, 0, sizeof(sink));
    bear.peer_inbox = &sink;
    bear.clock = 0u;

    CHECK(mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_BASE_ARRANGED_1,
                                        0x11111111u,
                                        MCL_CONTACT_ROLE_INITIATOR)
          == MCL_MACHINE_OK);
    CHECK(cfg.arranged_bearer == 1u);
    CHECK(cfg.wire_major == MCL_WIRE_STABLE_MAJOR);
    CHECK(cfg.bearer_count == 1u);
    CHECK(cfg.shared_medium == 0u);
    CHECK(strcmp(cfg.deployment_profile_id, "MCL-BASE-DEPLOYMENT-1") == 0);

    memset(&plat, 0, sizeof(plat));
    plat.clock_ms = b_clock;
    plat.transport_send = b_send;
    plat.user = &bear;
    /* Deliberately no candidate_open. */
    CHECK(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_OK);
}

/* The reference deployment still demands candidate_open. */
static void test_reference_still_requires_candidate_open(void)
{
    mcl_machine_t m;
    mcl_machine_config_t cfg;
    mcl_platform_t plat;
    inbox_t sink;
    bearer_t bear;

    memset(&sink, 0, sizeof(sink));
    bear.peer_inbox = &sink;
    bear.clock = 0u;

    CHECK(mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x22222222u,
                                        MCL_CONTACT_ROLE_INITIATOR)
          == MCL_MACHINE_OK);
    CHECK(cfg.arranged_bearer == 0u);
    CHECK(cfg.wire_major == MCL_WIRE_EXPERIMENTAL_MAJOR);

    memset(&plat, 0, sizeof(plat));
    plat.clock_ms = b_clock;
    plat.transport_send = b_send;
    plat.user = &bear;
    CHECK(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG);
}

/* Two machines, arranged bearer, no rendezvous, contact on both sides. */
static void test_two_machines_reach_contact(void)
{
    mcl_machine_t a, b;
    mcl_machine_config_t cfg_a, cfg_b;
    mcl_platform_t plat_a, plat_b;
    inbox_t in_a, in_b;
    bearer_t bear_a, bear_b;
    unsigned round;
    int a_up = 0, b_up = 0;
    uint32_t a_peer = 0u, b_peer = 0u;

    memset(&in_a, 0, sizeof(in_a));
    memset(&in_b, 0, sizeof(in_b));
    bear_a.peer_inbox = &in_b; bear_a.clock = 0u;
    bear_b.peer_inbox = &in_a; bear_b.clock = 0u;

    (void)mcl_machine_config_deployment(&cfg_a, MCL_DEPLOYMENT_BASE_ARRANGED_1,
                                        0xA1A1A1A1u,
                                        MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_machine_config_deployment(&cfg_b, MCL_DEPLOYMENT_BASE_ARRANGED_1,
                                        0xB2B2B2B2u,
                                        MCL_CONTACT_ROLE_RESPONDER);

    memset(&plat_a, 0, sizeof(plat_a));
    plat_a.clock_ms = b_clock;
    plat_a.transport_send = b_send;
    plat_a.user = &bear_a;

    memset(&plat_b, 0, sizeof(plat_b));
    plat_b.clock_ms = b_clock;
    plat_b.transport_send = b_send;
    plat_b.user = &bear_b;

    CHECK(mcl_machine_init(&a, &cfg_a, &plat_a) == MCL_MACHINE_OK);
    CHECK(mcl_machine_init(&b, &cfg_b, &plat_b) == MCL_MACHINE_OK);
    CHECK(mcl_machine_start(&a) == MCL_MACHINE_OK);
    CHECK(mcl_machine_start(&b) == MCL_MACHINE_OK);

    for (round = 0u; round < 10u && !(a_up && b_up); ++round) {
        mcl_machine_event_t ev;

        drain(&a, &in_a);
        drain(&b, &in_b);

        if (mcl_machine_poll(&a, &ev) == MCL_MACHINE_OK) {
            if (ev.kind == MCL_MACHINE_EVENT_PEER_DETECTED) { a_peer = ev.peer_ref; }
            if (ev.kind == MCL_MACHINE_EVENT_POLICY_REQUIRED) {
                CHECK(mcl_machine_admit(&a) == MCL_MACHINE_OK);
            }
            if (ev.kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) { a_up = 1; }
        }
        if (mcl_machine_poll(&b, &ev) == MCL_MACHINE_OK) {
            if (ev.kind == MCL_MACHINE_EVENT_PEER_DETECTED) { b_peer = ev.peer_ref; }
            if (ev.kind == MCL_MACHINE_EVENT_POLICY_REQUIRED) {
                CHECK(mcl_machine_admit(&b) == MCL_MACHINE_OK);
            }
            if (ev.kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) { b_up = 1; }
        }
    }

    CHECK(a_up == 1);
    CHECK(b_up == 1);
    /* Each side correlated the OTHER machine, not itself. */
    CHECK(a_peer == 0xB2B2B2B2u);
    CHECK(b_peer == 0xA1A1A1A1u);
}

/*
 * The bytes a Base 1 machine puts on the bearer really are the Stable pair.
 * Read straight out of the frame header rather than inferred from the API.
 */
static void test_announcement_is_stable_pair(void)
{
    mcl_machine_t a;
    mcl_machine_config_t cfg;
    mcl_platform_t plat;
    inbox_t peer;
    bearer_t bear;
    uint8_t link_major, wire_major;

    memset(&peer, 0, sizeof(peer));
    bear.peer_inbox = &peer;
    bear.clock = 0u;

    (void)mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_BASE_ARRANGED_1,
                                        0xC3C3C3C3u,
                                        MCL_CONTACT_ROLE_INITIATOR);
    memset(&plat, 0, sizeof(plat));
    plat.clock_ms = b_clock;
    plat.transport_send = b_send;
    plat.user = &bear;

    CHECK(mcl_machine_init(&a, &cfg, &plat) == MCL_MACHINE_OK);
    CHECK(mcl_machine_start(&a) == MCL_MACHINE_OK);
    CHECK(peer.count == 1u);

    if (peer.count == 1u) {
        /* Link major is the high nibble of the first frame byte. */
        link_major = (uint8_t)((peer.frame[0][0] >> 4u) & 0x0Fu);
        CHECK(link_major == MCL_LINK_STABLE_MAJOR);

        /*
         * The Wire object begins after the Link header. Rather than reproduce
         * the frame layout here, decode the frame and read the payload's own
         * common header, whose high nibble is the Wire major.
         */
        {
            mcl_link_frame_t frame;
            size_t consumed = 0u;
            CHECK(mcl_link_frame_decode(peer.frame[0], peer.len[0], &frame,
                                        &consumed) == MCL_LINK_OK);
            CHECK(frame.payload_len > 0u);
            if (frame.payload_len > 0u) {
                wire_major = (uint8_t)((frame.payload[0] >> 4u) & 0x0Fu);
                CHECK(wire_major == MCL_WIRE_STABLE_MAJOR);
            }
        }
    }
}

/*
 * COMPATIBILITY PIN. The historical framed-send entry point must still emit
 * the experimental pair. If this test ever has to be updated, something has
 * silently changed what existing integrations put on the wire.
 */
static void test_legacy_framed_send_is_still_major_zero(void)
{
    mcl_node_t node;
    mcl_node_config_t cfg;
    inbox_t out;
    bearer_t bear;
    mcl_wire_tier0_t obj;
    uint8_t scratch[FRAME_MAX];
    size_t sent = 0u;

    memset(&out, 0, sizeof(out));
    bear.peer_inbox = &out;
    bear.clock = 0u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = b_send;
    cfg.user_ctx = &bear;
    cfg.source_ref = 0xD4D4D4D4u;
    cfg.transport_id = BEARER_IP;
    cfg.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK(mcl_node_init(&node, &cfg) == MCL_SDK_OK);

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = cfg.source_ref;
    obj.body.presence.machine_class = 7u;   /* exists only at major 0 */
    obj.body.presence.capability_tag = 0x112233u;
    obj.body.presence.ttl = 60u;

    CHECK(mcl_node_send_framed_tier0(&node, &obj, MCL_LINK_CLASS_CONTACT, 0u,
                                     scratch, sizeof(scratch), &sent)
          == MCL_SDK_OK);
    CHECK(out.count == 1u);
    if (out.count == 1u) {
        mcl_link_frame_t frame;
        size_t consumed = 0u;
        CHECK(((out.frame[0][0] >> 4u) & 0x0Fu) == MCL_LINK_FRAME_MAJOR);
        CHECK(mcl_link_frame_decode(out.frame[0], out.len[0], &frame,
                                    &consumed) == MCL_LINK_OK);
        if (frame.payload_len > 0u) {
            CHECK(((frame.payload[0] >> 4u) & 0x0Fu)
                  == MCL_WIRE_EXPERIMENTAL_MAJOR);
            /* Major 0 PRESENCE is 11 bytes; major 1 drops machine_class. */
            CHECK(frame.payload_len
                  == mcl_wire_tier0_encoded_size_at_major(
                         MCL_WIRE_EXPERIMENTAL_MAJOR, MCL_WIRE_KIND_PRESENCE));
        }
    }
}

/* A Candidate object is refused at the Stable major, not demoted to one. */
static void test_candidate_object_refused_at_stable_major(void)
{
    mcl_node_t node;
    mcl_node_config_t cfg;
    inbox_t out;
    bearer_t bear;
    mcl_wire_tier0_t obj;
    uint8_t scratch[FRAME_MAX];
    size_t sent = 0u;

    memset(&out, 0, sizeof(out));
    bear.peer_inbox = &out;
    bear.clock = 0u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask =
        mcl_link_wire_major_mask(MCL_WIRE_STABLE_MAJOR);
    cfg.tx_fn = b_send;
    cfg.user_ctx = &bear;
    cfg.source_ref = 0xE5E5E5E5u;
    cfg.transport_id = BEARER_IP;
    cfg.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK(mcl_node_init(&node, &cfg) == MCL_SDK_OK);

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_HAZARD;      /* Candidate, not carried at major 1 */
    obj.priority = 3u;
    obj.source_ref = cfg.source_ref;

    CHECK(mcl_node_send_framed_tier0_at_major(
              &node, &obj, MCL_WIRE_STABLE_MAJOR, MCL_LINK_STABLE_MAJOR,
              MCL_LINK_CLASS_DATA, 0u, scratch, sizeof(scratch), &sent)
          == MCL_SDK_ERR_WIRE_FAILURE);
    CHECK(out.count == 0u);   /* nothing left the machine */

    /* The same object at the experimental major is carried. */
    CHECK(mcl_node_send_framed_tier0_at_major(
              &node, &obj, MCL_WIRE_EXPERIMENTAL_MAJOR, MCL_LINK_FRAME_MAJOR,
              MCL_LINK_CLASS_DATA, 0u, scratch, sizeof(scratch), &sent)
          == MCL_SDK_OK);
}

/* An unassigned major is refused rather than guessed. */
static void test_unassigned_major_refused(void)
{
    mcl_node_t node;
    mcl_node_config_t cfg;
    inbox_t out;
    bearer_t bear;
    mcl_wire_tier0_t obj;
    uint8_t scratch[FRAME_MAX];
    size_t sent = 0u;

    memset(&out, 0, sizeof(out));
    bear.peer_inbox = &out;
    bear.clock = 0u;

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask =
        mcl_link_wire_major_mask(MCL_WIRE_STABLE_MAJOR);
    cfg.tx_fn = b_send;
    cfg.user_ctx = &bear;
    cfg.source_ref = 0xF6F6F6F6u;
    cfg.transport_id = BEARER_IP;
    cfg.role = MCL_CONTACT_ROLE_INITIATOR;
    CHECK(mcl_node_init(&node, &cfg) == MCL_SDK_OK);

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = cfg.source_ref;

    CHECK(mcl_node_send_framed_tier0_at_major(
              &node, &obj, 9u, MCL_LINK_STABLE_MAJOR,
              MCL_LINK_CLASS_DATA, 0u, scratch, sizeof(scratch), &sent)
          != MCL_SDK_OK);
    CHECK(mcl_node_send_framed_tier0_at_major(
              &node, &obj, MCL_WIRE_STABLE_MAJOR, 9u,
              MCL_LINK_CLASS_DATA, 0u, scratch, sizeof(scratch), &sent)
          != MCL_SDK_OK);
    CHECK(out.count == 0u);
}

int main(void)
{
    test_init_without_rendezvous_machinery();
    test_reference_still_requires_candidate_open();
    test_two_machines_reach_contact();
    test_announcement_is_stable_pair();
    test_legacy_framed_send_is_still_major_zero();
    test_candidate_object_refused_at_stable_major();
    test_unassigned_major_refused();

    if (failures != 0) {
        printf("test_base1: %d failure(s)\n", failures);
        return 1;
    }
    puts("test_base1: all checks passed");
    return 0;
}

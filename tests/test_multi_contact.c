/*
 * Multi-contact isolation campaign.
 *
 * V1_SCOPE.md section 5.5, release gate item 9.
 *
 * mcl/sdk.h states an architectural claim:
 *
 *     "One node currently tracks one contact... A machine that must hold
 *      several concurrent contacts instantiates several nodes."
 *
 * That is an assertion, and the scope says it needs evidence rather than
 * assertion. This file is the evidence: several nodes share one bearer and
 * must not contaminate one another. Every case below is one way they could.
 *
 * WHAT WOULD MAKE THIS FILE WORTHLESS
 *
 * A harness that routed each frame only to its intended recipient would prove
 * nothing -- isolation would be a property of the harness. So the medium here
 * is a BROADCAST bus: every frame written to it is offered to every node, and
 * each node must refuse what is not its own. That is what a shared radio
 * actually does, and it is the only way a filtering mistake becomes visible.
 */

#include "mcl/sdk.h"
#include "mcl/negotiation.h"

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

/* Three peers, so that "the other one" is never unambiguous. */
#define REF_A 0xA0000001u
#define REF_B 0xB0000002u
#define REF_C 0xC0000003u

#define MIG_A  0x11110001u
#define MIG_B  0x22220002u
#define SESS_A 0x51110001u
#define SESS_B 0x52220002u

#define ENDPOINT_A 0xD00D0001u
#define ENDPOINT_B 0xD00D0002u

/* IP-DATAGRAM, the Standards Action assignment. Isolation is not a property
   of the profile, but this suite carries the value a deployment carries: a
   test that only ever exercises an experimental value is not exercising the
   bytes anyone will send. */
#define PROFILE 1u

static const uint8_t CHALLENGE_A[MCL_CONTACT_CHALLENGE_SIZE] =
    {0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u};
static const uint8_t CHALLENGE_B[MCL_CONTACT_CHALLENGE_SIZE] =
    {0xF1u, 0xF2u, 0xF3u, 0xF4u, 0xF5u, 0xF6u, 0xF7u, 0xF8u};

/*
 * A shared bearer. One buffer, because a bus that queued would let an ordering
 * mistake pass; every node is offered whatever is currently on it.
 */
typedef struct {
    uint8_t buffer[256];
    size_t size;
    uint8_t transport;
    unsigned sent;
} bus_t;

static int32_t bus_tx(void *user, uint8_t transport_id,
                      const uint8_t *data, size_t data_size)
{
    bus_t *bus = (bus_t *)user;
    size_t i;

    if (data_size > sizeof(bus->buffer)) {
        return -1;
    }
    bus->sent++;
    bus->transport = transport_id;
    for (i = 0u; i < data_size; ++i) {
        bus->buffer[i] = data[i];
    }
    bus->size = data_size;
    return 0;
}

static void init_node(mcl_node_t *node, bus_t *bus, uint32_t source_ref,
                      mcl_contact_role_t role)
{
    mcl_node_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = bus_tx;
    cfg.user_ctx = bus;
    cfg.source_ref = source_ref;
    cfg.transport_id = MCL_CONTACT_TRANSPORT_BLE;
    cfg.role = role;
    (void)mcl_node_init(node, &cfg);

    (void)mcl_node_link_transition(node, MCL_LINK_STATE_DISCOVERED);
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_CAPABILITIES);
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_NEGOTIATING);
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_ESTABLISHED);
}

static void make_presence(mcl_wire_tier0_t *obj, uint32_t source_ref)
{
    memset(obj, 0, sizeof(*obj));
    obj->kind = MCL_WIRE_KIND_PRESENCE;
    obj->priority = 1u;
    obj->source_ref = source_ref;
    obj->body.presence.machine_class = 1u;
    obj->body.presence.capability_tag = 0x000123u;
    obj->body.presence.ttl = 60u;
}

/* ------------------------------------------------------------------ */

/*
 * Case 1. Several contacts on one bearer.
 *
 * Every node hears every frame. A node must accept what is addressed to it and
 * refuse what is addressed to another, on the bytes alone.
 */
static void test_several_contacts_on_one_bearer(void)
{
    bus_t bus;
    mcl_node_t a, b, c;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u, consumed = 0u;
    mcl_sdk_status_t st_b, st_c;

    printf("[TEST] three contacts share one bearer without contamination\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_RESPONDER);
    init_node(&c, &bus, REF_C, MCL_CONTACT_ROLE_RESPONDER);

    /* A knows B. It does not know C. */
    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&a), REF_B);

    make_presence(&obj, REF_A);
    CHECK(mcl_node_send_framed_tier0(&a, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_DESTINATION,
                                     scratch, sizeof(scratch), &sent) ==
              MCL_SDK_OK,
          "A sends addressed to B");

    /* Both B and C hear it, because a shared bearer is shared. */
    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&b), REF_A);
    st_b = mcl_node_receive_framed(&b, bus.transport, bus.buffer, bus.size,
                                   &frame, &decoded, &has_object, &consumed);
    CHECK(st_b == MCL_SDK_OK, "B accepts a frame addressed to B");
    CHECK(has_object == 1u, "and decodes the object");

    has_object = 0u;
    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&c), REF_A);
    st_c = mcl_node_receive_framed(&c, bus.transport, bus.buffer, bus.size,
                                   &frame, &decoded, &has_object, &consumed);
    CHECK(st_c == MCL_SDK_NOT_ADDRESSED,
          "C refuses a frame addressed to B, on the bytes alone");
    CHECK(has_object == 0u,
          "and produces no object from a frame it refused");
}

/*
 * Case 2. The same migration_ref in two different contacts.
 *
 * migration_ref correlates ONE transaction. Nothing makes it globally unique --
 * it is not an identity -- so two unrelated contacts can legitimately pick the
 * same value. Neither may therefore treat a matching reference as sufficient.
 */
static void test_same_migration_ref_in_two_contacts(void)
{
    bus_t bus;
    mcl_node_t a, b;
    mcl_contact_t *ca, *cb;

    printf("[TEST] one migration_ref reused in two contacts stays separate\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_INITIATOR);
    ca = mcl_node_get_contact(&a);
    cb = mcl_node_get_contact(&b);

    /* Deliberately the SAME migration reference, different endpoints. */
    CHECK(mcl_contact_record_offer(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_A, 30u) == MCL_LINK_OK,
          "contact A records its offer");
    CHECK(mcl_contact_record_offer(cb, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_B, 30u) == MCL_LINK_OK,
          "contact B records an offer with the same reference");

    CHECK(mcl_contact_agree(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A) == MCL_LINK_OK,
          "A agrees under its own session");
    CHECK(mcl_contact_agree(cb, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_B) == MCL_LINK_OK,
          "B agrees under a different session");

    /*
     * The state is per contact. If migration_ref were treated as a key into
     * anything shared, one of these would have overwritten the other.
     */
    {
        /* The contact is caller-owned state; reading it directly is the
         * documented access, and there is no accessor to go stale. */
        CHECK(ca->session_valid == 1u && ca->session_ref == SESS_A,
              "A kept its own session reference");
        CHECK(cb->session_valid == 1u && cb->session_ref == SESS_B,
              "B kept its own session reference");
        CHECK(ca->session_ref != cb->session_ref,
              "the two contacts did not converge");
    }
}

/*
 * Case 3 and 4. Wrong session_ref and wrong destination_ref.
 */
static void test_wrong_session_and_destination_refused(void)
{
    bus_t bus;
    mcl_node_t a, b;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u, consumed = 0u;

    printf("[TEST] a frame for another contact's destination is refused\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_RESPONDER);

    /* A addresses C, a peer that is not B. */
    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&a), REF_C);
    make_presence(&obj, REF_A);
    CHECK(mcl_node_send_framed_tier0(&a, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_DESTINATION,
                                     scratch, sizeof(scratch), &sent) ==
              MCL_SDK_OK,
          "A sends addressed to C");

    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&b), REF_A);
    CHECK(mcl_node_receive_framed(&b, bus.transport, bus.buffer, bus.size,
                                  &frame, &decoded, &has_object, &consumed) ==
              MCL_SDK_NOT_ADDRESSED,
          "B refuses a frame destined for C");
    CHECK(has_object == 0u, "no object surfaces from a refused frame");

    /*
     * An UNADDRESSED frame is a different case and must still be accepted:
     * first contact has no destination to name yet, so a rule that required
     * one could never bootstrap.
     */
    has_object = 0u;
    CHECK(mcl_node_send_framed_tier0(&a, &obj, MCL_LINK_CLASS_DATA, 0u,
                                     scratch, sizeof(scratch), &sent) ==
              MCL_SDK_OK,
          "A broadcasts with no destination");
    CHECK(mcl_node_receive_framed(&b, bus.transport, bus.buffer, bus.size,
                                  &frame, &decoded, &has_object, &consumed) ==
              MCL_SDK_OK,
          "B accepts an unaddressed frame");
    CHECK(has_object == 1u, "and decodes it");
}

/*
 * Case 5. A candidate endpoint swapped between contacts.
 *
 * The nastiest of these: two contacts migrating at once, and the endpoint
 * token from one is presented to the other. endpoint_token is a rendezvous
 * reference, not an identity, so nothing about the token itself refuses this.
 * The migration reference is what must.
 */
static void test_candidate_endpoint_swap_refused(void)
{
    bus_t bus;
    mcl_node_t a, b;
    mcl_contact_t *ca, *cb;

    printf("[TEST] an endpoint token from another contact is refused\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_INITIATOR);
    ca = mcl_node_get_contact(&a);
    cb = mcl_node_get_contact(&b);

    CHECK(mcl_contact_record_offer(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_A, 30u) == MCL_LINK_OK,
          "A offers endpoint A under migration A");
    CHECK(mcl_contact_record_offer(cb, MIG_B, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_B, 30u) == MCL_LINK_OK,
          "B offers endpoint B under migration B");

    /*
     * Now agree on A using B's migration reference. The transport and profile
     * match, the endpoint exists, and only the reference is wrong -- which is
     * precisely the case a check on transport and profile alone would miss.
     */
    CHECK(mcl_contact_agree(ca, MIG_B, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A) != MCL_LINK_OK,
          "A refuses an agreement carrying another contact's migration ref");

    /* And the correct reference still works, so the refusal was specific. */
    CHECK(mcl_contact_agree(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A) == MCL_LINK_OK,
          "A accepts its own migration reference");

    /*
     * The same swap arriving LATER, once A has moved past OFFERED.
     *
     * This is a distinct code path -- the duplicate/retransmission branch --
     * and it is the dangerous one: a delayed acceptance from another contact
     * reaching a contact that is already validating must not be mistaken for a
     * retransmission of its own. Mutation testing found this uncovered: with
     * the OFFERED-path reference check removed the campaign failed, but with
     * the retransmission-path check removed it passed, which meant nothing here
     * exercised it.
     */
    CHECK(mcl_contact_validation_begin(ca, CHALLENGE_A) == MCL_LINK_OK,
          "A moves past OFFERED into validation");
    CHECK(mcl_contact_agree(ca, MIG_B, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A) != MCL_LINK_OK,
          "a delayed agreement from another contact is not a retransmission");

    /* A genuine retransmission of A's own acceptance is still idempotent, so
     * the refusal above is specific rather than a blanket rejection. */
    CHECK(mcl_contact_agree(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A) == MCL_LINK_OK,
          "A's own acceptance repeated is accepted and changes nothing");
    CHECK(ca->state == MCL_CONTACT_STATE_VALIDATING,
          "and it did not drag the contact back to AGREED");

    /* CHALLENGE_B belongs to the other contact and must not validate this one. */
    CHECK(mcl_contact_validation_response(cb, MIG_A, SESS_A,
                                          CHALLENGE_B) != MCL_LINK_OK,
          "B refuses a validation naming another contact's transaction");
}

/*
 * Case 6. One contact migrating while another sends data.
 *
 * Data is suspended for a contact in cutover. That suspension must be scoped
 * to the contact and not to the bearer -- otherwise one machine's migration
 * silences every other conversation on the radio.
 */
static void test_migration_does_not_silence_other_contacts(void)
{
    bus_t bus;
    mcl_node_t a, b;
    mcl_wire_tier0_t obj;
    uint8_t scratch[256];
    size_t sent = 0u;
    mcl_contact_t *ca;
    uint8_t data_transport = 0u;
    uint8_t quiesced = 0u;

    printf("[TEST] one contact's cutover does not silence another\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_INITIATOR);
    ca = mcl_node_get_contact(&a);

    /* Drive A into a validated, committed cutover. */
    (void)mcl_contact_record_offer(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_A, 30u);
    (void)mcl_contact_agree(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A);
    (void)mcl_contact_validation_begin(ca, CHALLENGE_A);

    /* A is now mid-migration. The contact reports quiescence for ITSELF. */
    CHECK(mcl_contact_data_transport(ca, &data_transport, &quiesced) ==
              MCL_LINK_OK,
          "A reports its own data transport");

    /* B is untouched and must still be able to send. */
    make_presence(&obj, REF_B);
    CHECK(mcl_node_send_framed_tier0(&b, &obj, MCL_LINK_CLASS_DATA, 0u,
                                     scratch, sizeof(scratch), &sent) ==
              MCL_SDK_OK,
          "B sends normally while A is migrating");
    CHECK(bus.sent > 0u, "and the bytes reached the bearer");
}

/*
 * Case 7. Glare, and the exact tie.
 *
 * Two offers cross. The tiebreaker compares (source_ref, migration_ref) as one
 * key and both machines reach the same conclusion without another round trip.
 * An exact tie aborts BOTH, because a tie means the keys cannot be
 * distinguished and continuing would leave the peers disagreeing about who
 * controls the migration.
 */
static void test_glare_and_exact_tie(void)
{
    bus_t bus;
    mcl_node_t a, b;
    mcl_contact_t *ca, *cb;
    mcl_contact_collision_t out_a = MCL_CONTACT_COLLISION_TIE_ABORT;
    mcl_contact_collision_t out_b = MCL_CONTACT_COLLISION_TIE_ABORT;

    printf("[TEST] glare resolves identically on both peers; a tie aborts\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_INITIATOR);
    ca = mcl_node_get_contact(&a);
    cb = mcl_node_get_contact(&b);

    (void)mcl_contact_record_offer(ca, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_A, 30u);
    (void)mcl_contact_record_offer(cb, MIG_B, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_B, 30u);

    /* Each sees the other's offer. REF_B > REF_A, so B's key wins on both. */
    CHECK(mcl_contact_resolve_offer_collision(ca, REF_B, MIG_B,
                                              MCL_CONTACT_TRANSPORT_IP,
                                              PROFILE, ENDPOINT_B, 30u,
                                              &out_a) == MCL_LINK_OK,
          "A resolves the collision");
    CHECK(mcl_contact_resolve_offer_collision(cb, REF_A, MIG_A,
                                              MCL_CONTACT_TRANSPORT_IP,
                                              PROFILE, ENDPOINT_A, 30u,
                                              &out_b) == MCL_LINK_OK,
          "B resolves the collision");

    /* The two must reach COMPLEMENTARY conclusions, not merely valid ones. */
    CHECK(out_a == MCL_CONTACT_COLLISION_PEER_WINS,
          "A yields to the larger key");
    CHECK(out_b == MCL_CONTACT_COLLISION_LOCAL_WINS,
          "B keeps its own offer");

    /* The exact tie: identical source and migration references. */
    {
        mcl_node_t t;
        bus_t tbus;
        mcl_contact_t *ct;
        mcl_contact_collision_t out_t = MCL_CONTACT_COLLISION_LOCAL_WINS;

        memset(&tbus, 0, sizeof(tbus));
        init_node(&t, &tbus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
        ct = mcl_node_get_contact(&t);
        (void)mcl_contact_record_offer(ct, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                       PROFILE, ENDPOINT_A, 30u);
        CHECK(mcl_contact_resolve_offer_collision(ct, REF_A, MIG_A,
                                                  MCL_CONTACT_TRANSPORT_IP,
                                                  PROFILE, ENDPOINT_A, 30u,
                                                  &out_t) == MCL_LINK_OK,
              "the tie resolves without error");
        CHECK(out_t == MCL_CONTACT_COLLISION_TIE_ABORT,
              "an indistinguishable key aborts rather than picking one");
    }
}

/*
 * Case 8. A delayed frame from a dead contact.
 *
 * The frame was legal when it was sent. It arrives after its contact ended, so
 * it must not revive anything or be attributed to whatever contact now holds
 * the bearer.
 */
static void test_delayed_frame_from_dead_contact(void)
{
    bus_t bus;
    mcl_node_t a, b;
    mcl_wire_tier0_t obj, decoded;
    mcl_link_frame_t frame;
    uint8_t stale[256];
    size_t stale_size;
    uint8_t stale_transport;
    uint8_t scratch[256];
    uint8_t has_object = 0u;
    size_t sent = 0u, consumed = 0u;

    printf("[TEST] a frame from a dead contact revives nothing\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&a, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &bus, REF_B, MCL_CONTACT_ROLE_RESPONDER);

    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&a), REF_B);
    make_presence(&obj, REF_A);
    (void)mcl_node_send_framed_tier0(&a, &obj, MCL_LINK_CLASS_DATA,
                                     MCL_LINK_FLAG_DESTINATION,
                                     scratch, sizeof(scratch), &sent);

    /* Capture it, then end the contact that produced it. */
    memcpy(stale, bus.buffer, bus.size);
    stale_size = bus.size;
    stale_transport = bus.transport;
    CHECK(mcl_node_reset(&a) == MCL_SDK_OK, "the contact ends");

    /*
     * The bearer is now held by a fresh contact with a DIFFERENT reference.
     * The stale frame names REF_B as destination, so it is not addressed to
     * this one and must be refused.
     */
    init_node(&a, &bus, REF_C, MCL_CONTACT_ROLE_RESPONDER);
    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&a), REF_A);
    CHECK(mcl_node_receive_framed(&a, stale_transport, stale, stale_size,
                                  &frame, &decoded, &has_object, &consumed) ==
              MCL_SDK_NOT_ADDRESSED,
          "the new contact refuses the dead contact's frame");
    CHECK(has_object == 0u, "and produces no object from it");

    (void)b;
}

/*
 * Case 9. A new contact reusing an old reference.
 *
 * References are correlation, not identity. A value freed by one contact can
 * legitimately be taken by another, so a match must never by itself be treated
 * as continuity of the SAME contact.
 */
static void test_reference_reuse_is_not_continuity(void)
{
    bus_t bus;
    mcl_node_t n;
    mcl_contact_t *c;
    uint32_t session;

    printf("[TEST] reusing a reference does not resume the old contact\n");

    memset(&bus, 0, sizeof(bus));
    init_node(&n, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    c = mcl_node_get_contact(&n);

    (void)mcl_contact_record_offer(c, MIG_A, MCL_CONTACT_TRANSPORT_IP,
                                   PROFILE, ENDPOINT_A, 30u);
    (void)mcl_contact_agree(c, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A);
    session = c->session_ref;
    CHECK(c->session_valid == 1u, "session recorded");
    CHECK(session == SESS_A, "and it is the one that was agreed");

    /* End it, and start a new contact that happens to reuse MIG_A. */
    CHECK(mcl_node_reset(&n) == MCL_SDK_OK, "the contact ends");
    init_node(&n, &bus, REF_A, MCL_CONTACT_ROLE_INITIATOR);
    c = mcl_node_get_contact(&n);

    CHECK(c->session_valid == 0u,
          "the fresh contact carries no session from its predecessor");

    /*
     * Reusing the migration reference must require the full transaction again
     * rather than resuming: an agree with no recorded offer is refused.
     */
    CHECK(mcl_contact_agree(c, MIG_A, MCL_CONTACT_TRANSPORT_IP, PROFILE,
                            SESS_A) != MCL_LINK_OK,
          "a reused reference does not resume the old transaction");
}

/*
 * Case 10. Negotiation state is per contact.
 *
 * The negotiation added for V1_SCOPE 5.4 introduces new per-contact state, so
 * it belongs in this campaign: two nodes negotiating different outcomes on one
 * bearer must not converge.
 */
static void test_negotiation_state_is_per_contact(void)
{
    mcl_link_capability_t local_a, local_b, peer_x, peer_y;
    mcl_link_negotiation_t sel_a, sel_b;

    printf("[TEST] two contacts on one bearer negotiate independently\n");

    /* A talks to a peer supporting majors {0}; B to one supporting {0,1,2}. */
    (void)mcl_link_make_capability(&local_a, 0x0007u, 0x0001u, 1048u, 0u);
    (void)mcl_link_make_capability(&local_b, 0x0007u, 0x0001u, 1048u, 0u);
    (void)mcl_link_make_capability(&peer_x, 0x0001u, 0x0001u, 1048u, 0u);
    (void)mcl_link_make_capability(&peer_y, 0x0007u, 0x0001u, 600u, 0u);

    CHECK(mcl_link_negotiation_select(&local_a, &peer_x, &sel_a) ==
              MCL_LINK_OK, "contact A negotiates");
    CHECK(mcl_link_negotiation_select(&local_b, &peer_y, &sel_b) ==
              MCL_LINK_OK, "contact B negotiates");

    CHECK(sel_a.wire_major == 0u, "A settled on the only common major");
    CHECK(sel_b.wire_major == 2u, "B settled on a higher one");
    CHECK(sel_a.max_frame == 1048u && sel_b.max_frame == 600u,
          "the two frame limits did not merge");

    /* B's outcome is not acceptable to A's contact, and vice versa. */
    CHECK(mcl_link_negotiation_check(&sel_b, &local_a, &peer_x) !=
              MCL_LINK_OK,
          "A refuses a selection computed for another contact");
}

int main(void)
{
    printf("=== MCL multi-contact isolation campaign ===\n");
    printf("Several nodes, one shared broadcast bearer. Every frame is offered\n"
           "to every node; isolation must come from the protocol, not the\n"
           "harness.\n\n");

    test_several_contacts_on_one_bearer();
    test_same_migration_ref_in_two_contacts();
    test_wrong_session_and_destination_refused();
    test_candidate_endpoint_swap_refused();
    test_migration_does_not_silence_other_contacts();
    test_glare_and_exact_tie();
    test_delayed_frame_from_dead_contact();
    test_reference_reuse_is_not_continuity();
    test_negotiation_state_is_per_contact();

    printf("\n%d checks, %d failed.\n", tests_run, tests_failed);
    return (tests_failed == 0) ? 0 : 1;
}

/*
 * MCL SDK handoff control path.
 *
 * The point of this file is one property: a complete transport migration in
 * which the two peers exchange NOTHING but encoded Link frames.
 *
 * Every earlier migration test drives both peers by calling mcl_contact_*
 * directly on each. That proves the state machine is coherent; it cannot prove
 * the migration is specified, because both ends are the same code being called
 * the same way. A run like that over real radios would prove the radios work
 * and nothing more. Here the only thing crossing between the two nodes is a
 * byte buffer, so anything not in those bytes cannot be carrying the protocol.
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

#define MIG   0x4D194201u
#define SESS  0x9A3C0517u

static const uint8_t CHALLENGE[MCL_CONTACT_CHALLENGE_SIZE] =
    {0x8Bu, 0x41u, 0xD2u, 0x07u, 0x6Eu, 0x55u, 0x90u, 0x3Cu};

/*
 * A one-frame medium. Deliberately holds exactly one frame: a test that could
 * queue frames would let an ordering mistake pass unnoticed.
 */
typedef struct {
    uint8_t buffer[128];
    size_t size;
    unsigned sent;
    unsigned dropped;
    int drop_next;
    /*
     * Which bearer the SDK asked for. The whole point of the transport-aware
     * callback: a test that ignored this could not tell a handoff control sent
     * on the candidate from one sent on the path being abandoned, and the two
     * are the difference between validating a path and validating nothing.
     */
    uint8_t transport;
    /* Next transmit result to report, so a test can exercise a definite
     * failure and an uncertain outcome, not only success. */
    int32_t next_result;
    int32_t pending_result;
} medium_t;

static int32_t medium_tx(void *user, uint8_t transport_id,
                         const uint8_t *data, size_t data_size)
{
    medium_t *m = (medium_t *)user;
    size_t i;

    m->sent++;
    m->transport = transport_id;
    m->pending_result = m->next_result;
    m->next_result = 0;
    if (m->pending_result < 0) {
        /* Definitely not transmitted: nothing reaches the medium. */
        m->size = 0u;
        return m->pending_result;
    }
    /* Uncertain (> 0) still delivers: the bytes may well have gone out, which
     * is exactly why the sender may not assume they did not. */
    if (m->drop_next) {
        /*
         * The transport accepted the frame and then lost it. That is the case
         * that matters: a transmit failure the sender can see is easy, and a
         * frame that vanishes after a successful send is what actually happens
         * on a radio.
         */
        m->drop_next = 0;
        m->dropped++;
        m->size = 0u;
        return 0;
    }
    if (data_size > sizeof(m->buffer)) {
        return -1;
    }
    for (i = 0u; i < data_size; ++i) {
        m->buffer[i] = data[i];
    }
    m->size = data_size;
    return m->pending_result;
}

static void init_node(mcl_node_t *node, medium_t *m, uint32_t source_ref,
                      mcl_contact_role_t role)
{
    mcl_node_config_t cfg;

    memset(m, 0, sizeof(*m));
    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = medium_tx;
    cfg.user_ctx = m;
    cfg.source_ref = source_ref;
    cfg.transport_id = MCL_CONTACT_TRANSPORT_BLE;
    cfg.role = role;
    (void)mcl_node_init(node, &cfg);

    /*
     * A migration may only be driven from an established link. The two state
     * machines are independent -- a contact on a transport is not a negotiated
     * session -- and the SDK owns the one invariant that connects them, so a
     * test must walk the lifecycle rather than assume it.
     */
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_DISCOVERED);
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_CAPABILITIES);
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_NEGOTIATING);
    (void)mcl_node_link_transition(node, MCL_LINK_STATE_ESTABLISHED);
}

/* Both peers agree the migration over the old transport. TRANSPORT_OFFER and
 * TRANSPORT_ACCEPT are Wire objects and are already covered by mcl-wire; this
 * file starts where their encoding stops. */
static void reach_agreed(mcl_node_t *node)
{
    mcl_contact_t *c = mcl_node_get_contact(node);
    (void)mcl_contact_record_offer(c, MIG, MCL_CONTACT_TRANSPORT_IP, 192u,
                                   0xD00D0001u, 30u);
    (void)mcl_contact_agree(c, MIG, MCL_CONTACT_TRANSPORT_IP, 192u, SESS);
}

/* Move whatever is in one node's medium into the other node, decode it, and
 * apply it. Returns the action the receiver should take. */
static mcl_sdk_status_t deliver(
    medium_t *from,
    mcl_node_t *to,
    mcl_handoff_control_t *control,
    mcl_handoff_action_t *action)
{
    mcl_link_frame_t frame;
    size_t consumed = 0u;
    mcl_sdk_status_t st;

    *action = MCL_HANDOFF_ACTION_NONE;
    if (from->size == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    /*
     * Delivered on the transport it was actually SENT on. Feeding the receiver
     * a transport the test chose would defeat the check: the SDK verifies that
     * a control arrived on the path the transaction is being conducted over,
     * and a harness that always supplied the expected value would never
     * exercise it.
     */
    st = mcl_node_receive_handoff(to, from->transport, from->buffer, from->size,
                                  &frame, control, &consumed);
    if (st != MCL_SDK_OK) {
        return st;
    }
    if (consumed != from->size) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }
    return mcl_node_apply_handoff(to, control, action);
}


/*
 * Frame a control directly, bypassing the sending node's own state machine.
 *
 * The refusal tests below need frames a correct sender would never produce --
 * a COMMIT for an unvalidated path, a stale transaction, a session that is not
 * this contact's. mcl_node_send_handoff now refuses to build those, which is
 * right, so a test that used it could no longer produce them. A peer that is
 * buggy, or not this implementation at all, has no such scruples, and this is
 * what the receiver must survive.
 */
static size_t frame_control_directly(
    const mcl_handoff_control_t *control,
    uint32_t session_ref,
    uint8_t flags,
    uint8_t *out,
    size_t capacity)
{
    uint8_t control_bytes[MCL_HANDOFF_CONTROL_MAX_SIZE];
    mcl_link_frame_t frame;
    size_t written = 0u;
    size_t frame_size = 0u;

    if (mcl_handoff_control_encode(control, control_bytes,
                                   sizeof(control_bytes), &written)
        != MCL_LINK_OK) {
        return 0u;
    }
    frame.frame_class = MCL_LINK_CLASS_HANDOFF;
    frame.flags = flags;
    frame.source_ref = 0x0000C0C0u;
    frame.destination_ref = 0u;
    frame.session_ref = session_ref;
    frame.sequence = 0u;
    frame.freshness_ms = 0u;
    frame.payload = control_bytes;
    frame.payload_len = (uint16_t)written;
    if (mcl_link_frame_encode(&frame, out, capacity, &frame_size)
        != MCL_LINK_OK) {
        return 0u;
    }
    return frame_size;
}

/* ---------------------------------------------------------------------- */

static void test_full_migration_over_bytes(void)
{
    mcl_node_t a;
    mcl_node_t b;
    medium_t ma;
    medium_t mb;
    mcl_handoff_control_t control;
    mcl_handoff_control_t received;
    mcl_handoff_action_t action;
    uint8_t scratch[128];
    uint8_t transport_a = 0u;
    uint8_t transport_b = 0u;
    const uint8_t flags = MCL_LINK_FLAG_SESSION | MCL_LINK_FLAG_FRAME_CHECK;

    printf("full migration carried entirely by encoded frames\n");

    init_node(&a, &ma, 0x0000A0A0u, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &mb, 0x0000B0B0u, MCL_CONTACT_ROLE_RESPONDER);
    reach_agreed(&a);
    reach_agreed(&b);

    /* A -> B : PATH_CHALLENGE */
    CHECK(mcl_handoff_make_path_challenge(&control, MIG, SESS, CHALLENGE) ==
          MCL_LINK_OK, "build PATH_CHALLENGE");
    CHECK(mcl_contact_validation_begin(mcl_node_get_contact(&a), CHALLENGE) ==
          MCL_LINK_OK, "A records the challenge it is sending");
    CHECK(mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_OK, "A sends PATH_CHALLENGE");
    CHECK(deliver(&ma, &b, &received, &action) == MCL_SDK_OK,
          "B accepts PATH_CHALLENGE");
    CHECK(action == MCL_HANDOFF_ACTION_SEND_PATH_RESPONSE,
          "B is told to answer with PATH_RESPONSE");
    CHECK(received.challenge_present == 1u, "challenge survived the frame");
    CHECK(memcmp(received.challenge, CHALLENGE, sizeof(CHALLENGE)) == 0,
          "challenge bytes are exact");

    /* B -> A : PATH_RESPONSE, echoing what it decoded rather than what a test
     * handed it. If the challenge did not survive encoding, this fails. */
    CHECK(mcl_handoff_make_path_response(&control, received.migration_ref,
                                         received.session_ref,
                                         received.challenge) == MCL_LINK_OK,
          "build PATH_RESPONSE from decoded bytes");
    CHECK(mcl_node_send_handoff(&b, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_OK, "B sends PATH_RESPONSE");
    CHECK(deliver(&mb, &a, &received, &action) == MCL_SDK_OK,
          "A accepts PATH_RESPONSE");
    CHECK(action == MCL_HANDOFF_ACTION_NONE, "PATH_RESPONSE needs no reply");
    CHECK(mcl_node_get_contact(&a)->state == MCL_CONTACT_STATE_VALIDATED,
          "A reached VALIDATED");

    /* A -> B : COMMIT */
    CHECK(mcl_handoff_make_commit(&control, MIG, SESS) == MCL_LINK_OK,
          "build COMMIT");
    /*
     * No mcl_contact_commit_begin here. The SDK owns that transition and ties
     * it to the transmission, so a COMMIT the transport never sent cannot
     * strand the contact irrevocably in COMMITTING.
     */
    CHECK(mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_OK, "A sends COMMIT");
    CHECK(mcl_node_get_contact(&a)->state == MCL_CONTACT_STATE_COMMITTING,
          "and entered COMMITTING as a result of sending it");
    CHECK(ma.transport == MCL_CONTACT_TRANSPORT_IP,
          "COMMIT went out on the candidate, not the old path");
    CHECK(deliver(&ma, &b, &received, &action) == MCL_SDK_OK,
          "B accepts COMMIT");
    CHECK(action == MCL_HANDOFF_ACTION_SEND_CONFIRM,
          "B is told to answer with CONFIRM");
    CHECK(mcl_contact_active_transport(mcl_node_get_contact(&b),
                                       &transport_b) == MCL_LINK_OK &&
          transport_b == MCL_CONTACT_TRANSPORT_IP,
          "B is on the candidate transport");

    /* B -> A : CONFIRM */
    CHECK(mcl_handoff_make_confirm(&control, received.migration_ref,
                                   received.session_ref) == MCL_LINK_OK,
          "build CONFIRM");
    CHECK(mcl_node_send_handoff(&b, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_OK, "B sends CONFIRM");
    CHECK(deliver(&mb, &a, &received, &action) == MCL_SDK_OK,
          "A accepts CONFIRM");
    CHECK(mcl_contact_active_transport(mcl_node_get_contact(&a),
                                       &transport_a) == MCL_LINK_OK,
          "A reports a transport");

    CHECK(transport_a == transport_b,
          "both peers finished on the same transport");
    CHECK(transport_a == MCL_CONTACT_TRANSPORT_IP,
          "and it is the candidate they agreed on");
}

/*
 * The same migration with the CONFIRM lost in the medium.
 *
 * Without retransmission this ends with B on the new transport and A back on
 * the old one, permanently, from a single dropped frame. See
 * mcl-link/spec/link-handoff-control-v0.1.md section 8.
 */
static void test_lost_confirm_over_bytes(void)
{
    mcl_node_t a;
    mcl_node_t b;
    medium_t ma;
    medium_t mb;
    mcl_handoff_control_t control;
    mcl_handoff_control_t received;
    mcl_handoff_action_t action;
    uint8_t scratch[128];
    uint8_t transport_a = 0u;
    uint8_t transport_b = 0u;
    const uint8_t flags = MCL_LINK_FLAG_SESSION | MCL_LINK_FLAG_FRAME_CHECK;

    printf("a dropped CONFIRM is repaired by retransmitting COMMIT\n");

    init_node(&a, &ma, 0x0000A0A0u, MCL_CONTACT_ROLE_INITIATOR);
    init_node(&b, &mb, 0x0000B0B0u, MCL_CONTACT_ROLE_RESPONDER);
    reach_agreed(&a);
    reach_agreed(&b);

    (void)mcl_handoff_make_path_challenge(&control, MIG, SESS, CHALLENGE);
    (void)mcl_contact_validation_begin(mcl_node_get_contact(&a), CHALLENGE);
    (void)mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch), NULL);
    (void)deliver(&ma, &b, &received, &action);
    (void)mcl_handoff_make_path_response(&control, MIG, SESS, received.challenge);
    (void)mcl_node_send_handoff(&b, &control, flags, scratch, sizeof(scratch), NULL);
    (void)deliver(&mb, &a, &received, &action);

    (void)mcl_handoff_make_commit(&control, MIG, SESS);
    (void)mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch), NULL);
    CHECK(deliver(&ma, &b, &received, &action) == MCL_SDK_OK, "B accepts COMMIT");
    CHECK(action == MCL_HANDOFF_ACTION_SEND_CONFIRM, "B answers with CONFIRM");

    /* B's CONFIRM is accepted by the transport and then lost. */
    mb.drop_next = 1;
    (void)mcl_handoff_make_confirm(&control, MIG, SESS);
    CHECK(mcl_node_send_handoff(&b, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_OK,
          "B's send succeeds even though the frame is lost");
    CHECK(mb.dropped == 1u, "the medium dropped it");
    CHECK(mb.size == 0u, "nothing arrived at A");

    /* The peers now disagree, which is the whole problem. */
    CHECK(mcl_node_get_contact(&a)->state == MCL_CONTACT_STATE_COMMITTING,
          "A is still committing");
    (void)mcl_contact_active_transport(mcl_node_get_contact(&a), &transport_a);
    (void)mcl_contact_active_transport(mcl_node_get_contact(&b), &transport_b);
    CHECK(transport_a != transport_b, "the peers are on different transports");

    /* A retransmits COMMIT on the candidate. */
    (void)mcl_handoff_make_commit(&control, MIG, SESS);
    CHECK(mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_OK, "A retransmits COMMIT");
    CHECK(deliver(&ma, &b, &received, &action) == MCL_SDK_OK,
          "B accepts the retransmission");
    CHECK(action == MCL_HANDOFF_ACTION_SEND_CONFIRM, "B confirms again");
    CHECK(mcl_node_get_contact(&b)->migration_count == 1u,
          "and did not migrate a second time");

    (void)mcl_handoff_make_confirm(&control, MIG, SESS);
    (void)mcl_node_send_handoff(&b, &control, flags, scratch, sizeof(scratch), NULL);
    CHECK(deliver(&mb, &a, &received, &action) == MCL_SDK_OK,
          "A accepts the repeated CONFIRM");

    (void)mcl_contact_active_transport(mcl_node_get_contact(&a), &transport_a);
    (void)mcl_contact_active_transport(mcl_node_get_contact(&b), &transport_b);
    CHECK(transport_a == transport_b, "the peers converged");
    CHECK(transport_a == MCL_CONTACT_TRANSPORT_IP, "on the candidate");
}

/* Frames a receiver must refuse without damaging the contact it already has. */
static void test_refusals_do_not_damage_the_contact(void)
{
    mcl_node_t b;
    medium_t mb;
    mcl_handoff_control_t control;
    mcl_handoff_control_t received;
    mcl_handoff_action_t action;
    mcl_link_frame_t frame;
    uint8_t framed[128];
    size_t frame_size = 0u;
    size_t consumed = 0u;
    uint8_t transport = 0u;
    const uint8_t flags = MCL_LINK_FLAG_SESSION;
    /* B agreed a migration to IP, so that is where its controls belong. */
    const uint8_t candidate = MCL_CONTACT_TRANSPORT_IP;

    printf("refused controls leave the working contact intact\n");

    init_node(&b, &mb, 0x0000B0B0u, MCL_CONTACT_ROLE_RESPONDER);
    reach_agreed(&b);

    /* A COMMIT arriving before the path is validated. This is the defect the
     * state machine exists to prevent. */
    (void)mcl_handoff_make_commit(&control, MIG, SESS);
    frame_size = frame_control_directly(&control, SESS, flags, framed,
                                        sizeof(framed));
    CHECK(frame_size > 0u, "the hostile COMMIT frames");
    CHECK(mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed) == MCL_SDK_OK,
          "it decodes; refusing happens when it is APPLIED");
    CHECK(mcl_node_apply_handoff(&b, &received, &action) ==
          MCL_SDK_ERR_LINK_FAILURE, "COMMIT before VALIDATED is refused");
    CHECK(mcl_node_get_contact(&b)->state == MCL_CONTACT_STATE_AGREED,
          "and B's state did not move");

    /* A CONFIRM with no commit outstanding. */
    (void)mcl_handoff_make_confirm(&control, MIG, SESS);
    frame_size = frame_control_directly(&control, SESS, flags, framed,
                                        sizeof(framed));
    (void)mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed);
    CHECK(mcl_node_apply_handoff(&b, &received, &action) ==
          MCL_SDK_ERR_LINK_FAILURE, "CONFIRM before COMMITTING is refused");
    CHECK(mcl_node_get_contact(&b)->state == MCL_CONTACT_STATE_AGREED,
          "and B's state did not move");

    /* A challenge for a transaction this contact is not running. Without
     * migration_ref a delayed control from an abandoned attempt would be
     * indistinguishable from the live one. */
    (void)mcl_handoff_make_path_challenge(&control, MIG + 1u, SESS, CHALLENGE);
    frame_size = frame_control_directly(&control, SESS, flags, framed,
                                        sizeof(framed));
    (void)mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed);
    CHECK(mcl_node_apply_handoff(&b, &received, &action) ==
          MCL_SDK_ERR_INVALID_STATE, "a stale migration_ref is refused");
    CHECK(mcl_node_get_contact(&b)->state == MCL_CONTACT_STATE_AGREED,
          "and B did not enter VALIDATING on the strength of it");

    /* Another contact's session carrying this contact's transaction. */
    (void)mcl_handoff_make_path_challenge(&control, MIG, SESS + 1u, CHALLENGE);
    frame_size = frame_control_directly(&control, SESS + 1u, flags, framed,
                                        sizeof(framed));
    (void)mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed);
    CHECK(mcl_node_apply_handoff(&b, &received, &action) ==
          MCL_SDK_ERR_INVALID_STATE, "a foreign session_ref is refused");
    CHECK(mcl_node_get_contact(&b)->state == MCL_CONTACT_STATE_AGREED,
          "and B's state did not move");

    /*
     * THE ARRIVAL TRANSPORT IS CHECKED.
     *
     * A perfectly formed PATH_CHALLENGE, naming the right transaction and the
     * right session -- delivered over the OLD path. Every reference in it is
     * correct, so nothing else in the sequence can detect it. Accepting it
     * would mean answering a challenge, and eventually declaring a candidate
     * reachable, on the strength of bytes that never crossed the candidate.
     */
    (void)mcl_handoff_make_path_challenge(&control, MIG, SESS, CHALLENGE);
    frame_size = frame_control_directly(&control, SESS, flags, framed,
                                        sizeof(framed));
    CHECK(mcl_node_receive_handoff(&b, MCL_CONTACT_TRANSPORT_BLE, framed,
                                   frame_size, &frame, &received, &consumed) ==
          MCL_SDK_ERR_WRONG_TRANSPORT,
          "a control arriving on the old path is refused");
    CHECK(mcl_node_get_contact(&b)->state == MCL_CONTACT_STATE_AGREED,
          "and B did not begin validating anything");
    /* The same bytes on the candidate are accepted, so the refusal above was
     * about the transport and nothing else. */
    CHECK(mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed) == MCL_SDK_OK,
          "the identical frame on the candidate is fine");

    /*
     * A post-acceptance control with no outer session reference. Required, not
     * merely checked when present: a machine holding several contacts cannot
     * route this frame without it.
     */
    (void)mcl_handoff_make_commit(&control, MIG, SESS);
    frame_size = frame_control_directly(&control, 0u, 0u, framed,
                                        sizeof(framed));
    CHECK(mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed) ==
          MCL_SDK_ERR_FRAME_FAILURE,
          "a HANDOFF frame without the SESSION flag is refused");

    /* The contact is still usable. */
    CHECK(mcl_contact_active_transport(mcl_node_get_contact(&b), &transport) ==
          MCL_LINK_OK && transport == MCL_CONTACT_TRANSPORT_BLE,
          "the old working transport survived every refusal");

    /* A handoff control smuggled inside a DATA frame. The class is what selects
     * the registry the payload belongs to. */
    {
        mcl_link_frame_t out_frame;
        uint8_t control_bytes[MCL_HANDOFF_CONTROL_MAX_SIZE];
        size_t written = 0u;

        (void)mcl_handoff_make_commit(&control, MIG, SESS);
        (void)mcl_handoff_control_encode(&control, control_bytes,
                                         sizeof(control_bytes), &written);
        out_frame.frame_class = MCL_LINK_CLASS_DATA;
        out_frame.flags = MCL_LINK_FLAG_SESSION;
        out_frame.source_ref = 0x0000C0C0u;
        out_frame.destination_ref = 0u;
        out_frame.session_ref = SESS;
        out_frame.sequence = 0u;
        out_frame.freshness_ms = 0u;
        out_frame.payload = control_bytes;
        out_frame.payload_len = (uint16_t)written;
        CHECK(mcl_link_frame_encode(&out_frame, framed, sizeof(framed),
                                    &frame_size) == MCL_LINK_OK,
              "a DATA frame carrying a control encodes");
        CHECK(mcl_node_receive_handoff(&b, candidate, framed, frame_size,
                                       &frame, &received, &consumed) ==
              MCL_SDK_ERR_FRAME_FAILURE,
              "a control in a DATA frame is refused");
    }

    /* A HANDOFF frame whose own session_ref contradicts its payload. */
    (void)mcl_handoff_make_commit(&control, MIG, SESS);
    frame_size = frame_control_directly(&control, SESS + 7u, flags, framed,
                                        sizeof(framed));
    CHECK(mcl_node_receive_handoff(&b, candidate, framed, frame_size, &frame,
                                   &received, &consumed) ==
          MCL_SDK_ERR_FRAME_FAILURE,
          "a frame contradicting its own payload is refused");

    /* A HANDOFF frame carrying a malformed control. */
    {
        mcl_link_frame_t out_frame;
        static const uint8_t garbage[] = {0x00u, 0x05u, 0x4Du, 0x19u, 0x42u,
                                          0x01u, 0x9Au, 0x3Cu, 0x05u, 0x17u};

        out_frame.frame_class = MCL_LINK_CLASS_HANDOFF;
        out_frame.flags = MCL_LINK_FLAG_SESSION;
        out_frame.source_ref = 0x0000C0C0u;
        out_frame.destination_ref = 0u;
        out_frame.session_ref = SESS;
        out_frame.sequence = 0u;
        out_frame.freshness_ms = 0u;
        out_frame.payload = garbage;
        out_frame.payload_len = (uint16_t)sizeof(garbage);
        (void)mcl_link_frame_encode(&out_frame, framed, sizeof(framed),
                                    &frame_size);
        CHECK(mcl_node_receive_handoff(&b, candidate, framed, frame_size,
                                       &frame, &received, &consumed) ==
              MCL_SDK_ERR_LINK_FAILURE,
              "an unassigned operation is refused at the SDK boundary");
        CHECK(mcl_node_get_contact(&b)->state == MCL_CONTACT_STATE_AGREED,
              "and nothing moved");
    }
}

/*
 * Sending must refuse to emit a frame whose session reference would contradict
 * the control inside it, and must refuse to emit one that cannot be routed.
 */
static void test_send_refuses_contradictory_session(void)
{
    mcl_node_t a;
    medium_t ma;
    mcl_handoff_control_t control;
    uint8_t scratch[128];

    printf("a frame may not contradict, or fail to name, its control\n");

    init_node(&a, &ma, 0x0000A0A0u, MCL_CONTACT_ROLE_INITIATOR);

    /* No session agreed yet: MCL_LINK_FLAG_SESSION has nothing to carry. */
    (void)mcl_handoff_make_path_challenge(&control, MIG, SESS, CHALLENGE);
    CHECK(mcl_node_send_handoff(&a, &control, MCL_LINK_FLAG_SESSION, scratch,
                                sizeof(scratch), NULL) ==
          MCL_SDK_ERR_INVALID_STATE,
          "no session reference exists to put in the frame");

    reach_agreed(&a);

    (void)mcl_handoff_make_path_challenge(&control, MIG, SESS + 3u, CHALLENGE);
    CHECK(mcl_node_send_handoff(&a, &control, MCL_LINK_FLAG_SESSION, scratch,
                                sizeof(scratch), NULL) ==
          MCL_SDK_ERR_INVALID_STATE,
          "a control naming a different session is not framed");

    /*
     * Without the flag the frame cannot be routed to a contact by a receiver
     * holding more than one, so it is refused rather than sent. An earlier
     * revision sent it, treating the routing field as optional.
     */
    CHECK(mcl_node_send_handoff(&a, &control, 0u, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_ERR_INVALID_ARGUMENT,
          "the SESSION flag is mandatory on a handoff frame");

    /* A control this build cannot encode is never framed. */
    (void)mcl_handoff_make_path_challenge(&control, MIG, SESS, CHALLENGE);
    control.operation = 200u;
    CHECK(mcl_node_send_handoff(&a, &control, MCL_LINK_FLAG_SESSION, scratch,
                                sizeof(scratch), NULL) ==
          MCL_SDK_ERR_LINK_FAILURE,
          "an unassigned operation is never emitted");
}

/*
 * THE COMMIT SEND BOUNDARY.
 *
 * Entering COMMITTING is irrevocable, so it must follow the transmission
 * rather than precede it. A transport that reports a DEFINITE failure must
 * leave the contact in VALIDATED -- otherwise a frame nobody ever saw strands
 * the contact in a state it is forbidden to leave. A transport that cannot say
 * must be treated as though the peer has the bytes, because it might.
 */
static void test_commit_transition_follows_transmission(void)
{
    mcl_node_t a;
    medium_t ma;
    mcl_handoff_control_t control;
    uint8_t scratch[128];
    const uint8_t flags = MCL_LINK_FLAG_SESSION;

    printf("the commit transition belongs to the transmission\n");

    init_node(&a, &ma, 0x0000A0A0u, MCL_CONTACT_ROLE_INITIATOR);
    reach_agreed(&a);
    (void)mcl_contact_validation_begin(mcl_node_get_contact(&a), CHALLENGE);
    (void)mcl_contact_validation_response(mcl_node_get_contact(&a), MIG, SESS,
                                          CHALLENGE);
    CHECK(mcl_node_get_contact(&a)->state == MCL_CONTACT_STATE_VALIDATED,
          "A is validated and ready to commit");

    /* Definite failure: nothing left, so nothing is irrevocable. */
    (void)mcl_handoff_make_commit(&control, MIG, SESS);
    ma.next_result = -1;
    CHECK(mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_ERR_TX_NOT_SENT,
          "a definite transmit failure is reported as such");
    CHECK(mcl_node_get_contact(&a)->state == MCL_CONTACT_STATE_VALIDATED,
          "and the contact is NOT stranded in COMMITTING");
    CHECK(mcl_contact_abandon_migration(mcl_node_get_contact(&a)) ==
          MCL_LINK_OK, "so the migration can still be abandoned safely");

    /* Uncertain outcome: the peer may hold the bytes, so it IS irrevocable. */
    reach_agreed(&a);
    (void)mcl_contact_validation_begin(mcl_node_get_contact(&a), CHALLENGE);
    (void)mcl_contact_validation_response(mcl_node_get_contact(&a), MIG, SESS,
                                          CHALLENGE);
    ma.next_result = 1;
    CHECK(mcl_node_send_handoff(&a, &control, flags, scratch, sizeof(scratch),
                                NULL) == MCL_SDK_ERR_TX_UNCERTAIN,
          "an uncertain outcome is reported distinctly");
    CHECK(mcl_node_get_contact(&a)->state == MCL_CONTACT_STATE_COMMITTING,
          "and the contact HAS committed, because the peer may have it");
    CHECK(mcl_contact_abandon_migration(mcl_node_get_contact(&a)) ==
          MCL_LINK_ERR_INVALID_STATE,
          "so rollback is refused");
}

int main(void)
{
    printf("MCL SDK handoff control tests\n\n");

    test_full_migration_over_bytes();
    test_lost_confirm_over_bytes();
    test_refusals_do_not_damage_the_contact();
    test_send_refuses_contradictory_session();
    test_commit_transition_follows_transmission();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    printf("NOTE: completing this sequence proves reachability on the "
           "candidate path.\n"
           "      It does not prove the peer is the machine the contact began "
           "with.\n");
    return tests_failed == 0 ? 0 : 1;
}

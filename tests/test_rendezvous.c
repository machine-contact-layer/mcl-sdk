/*
 * Rendezvous coordinator checks.
 *
 * THE ROOM MODELS AIRTIME, AND THAT IS THE POINT
 *
 * The first version of this file queued every emission and then delivered each
 * one independently to every other node. Frames emitted in the same tick did
 * not interfere. That room could not fail a contention rule no matter how wrong
 * the rule was -- and the rule WAS wrong: replies were scheduled inside a
 * 400 ms window while a 17-byte TRANSPORT_OFFER occupies 786.7 ms of air, so
 * two responders overlapped with certainty rather than with some probability.
 * A simulator that cannot represent overlap cannot notice that.
 *
 * So transmissions here occupy an interval. A transmission is delivered only if
 * nothing else was on the air at any point during it, and `medium_busy` reports
 * what a real sensor would report. Three coordinators in this room contend for
 * real.
 *
 * The clock is simulated so the backoff, the retry bounds and the 2^32 wrap can
 * be tested exactly, none of which is reliable against a real clock.
 */

#include "mcl/rendezvous.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void check(int condition, const char *what)
{
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

/* ------------------------------------------------------------ the room */

#define MAX_NODES 4u
#define MAX_TX 64u
#define MAX_FRAME_SIZE 96u

/* AP-BOOTSTRAP-1 section 3: 48 kHz, 160 samples per symbol, 0.2 s preamble,
   16 training symbols, a 3-byte PHY header. Airtime in milliseconds. */
static uint32_t airtime_ms(size_t payload_bytes)
{
    double symbols = 16.0 + (double)(3u + payload_bytes) * 8.0;
    double seconds = 0.2 + symbols * 160.0 / 48000.0;
    return (uint32_t)(seconds * 1000.0 + 0.5);
}

typedef struct {
    uint8_t transport_id;
    uint8_t bytes[MAX_FRAME_SIZE];
    size_t size;
    uint8_t from;
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t collided;
    uint8_t delivered;
} transmission_t;

/* One Tier-0 object as it went out on the shared bearer. */
typedef struct {
    uint32_t at_ms;
    uint8_t kind;
    uint8_t from;
    uint8_t transport_id;
    uint8_t profile_id;
    uint32_t source_ref;
    uint32_t migration_ref;
    uint32_t session_ref;
    uint32_t endpoint_token;
} logged_t;

#define MAX_LOG 512u

typedef struct {
    uint32_t now;
    transmission_t tx[MAX_TX];
    unsigned count;
    int tx_unknown;
    int deaf;
    /* A shared medium only exists on the bootstrap bearer here; a candidate
       bearer is modelled as point-to-point, which is what it is. */
    uint8_t shared_transport;
    unsigned collisions;
    unsigned delivered;
    /* Deterministic stub randomness, per node, so a "random" run is
       reproducible and a collision result is not luck. */
    uint32_t seed[MAX_NODES];
    int random_enabled;
    /*
     * DESTROY THE FIRST CONTROL OF ONE KIND.
     *
     * The handoff controls are idempotent by construction and the Link layer
     * proved it over a hundred and four switches. The coordinator sent each of
     * them exactly once, so it inherited none of that: a single lost
     * PATH_RESPONSE left the controller waiting for ever with the recovery
     * machinery untouched.
     *
     * `drop_op` names a handoff operation whose FIRST occurrence on the
     * candidate bearer is destroyed. Only the first: the point is that the
     * retransmission of the SAME control recovers, not that the protocol
     * survives an indefinitely broken link.
     */
    int drop_op;          /* -1 for none, else mcl_handoff_op_t */
    unsigned dropped;
    /* Force every node into the same backoff slot, so a collision is certain
       rather than probable. A slot scheme is only interesting if the case it
       is designed for can actually be produced. */
    int force_same_slot;
    /*
     * Destroy the next N TRANSPORT_ACCEPT emissions on the SHARED bearer.
     *
     * drop_op above only reaches handoff controls on the candidate bearer, and
     * the acceptance is the one frame in the whole exchange that both ends
     * depend on and neither retransmits by a timer: the responder retries its
     * OFFER, and the solicitor re-arms the SAME acceptance when it hears that
     * retry. Nothing tested that path, so nothing would have noticed if the
     * re-arm had generated a fresh session or a fresh reference instead.
     */
    int drop_accepts;
    unsigned accepts_dropped;
    /*
     * FORCE THE TRANSPORT'S ANSWER FOR THE NEXT N EMISSIONS.
     *
     * mcl_sdk_tx_fn has three outcomes and the coordinator keeps all three
     * apart -- but every case in this file let the room answer 0, so the
     * distinction was implemented and never exercised. A rule no test can
     * break is a rule nobody has checked.
     *
     *   tx_force  0   sent
     *            -1   DEFINITELY not sent: nothing reaches the air at all
     *            +1   the transport cannot tell; the bytes DO go out, which is
     *                 the case that matters, because a coordinator treating
     *                 "unknown" as "not sent" would start a second
     *                 transaction over the top of a live one
     */
    int tx_force;
    int tx_force_left;
    unsigned tx_forced;
    int tx_say_unknown;        /* set for ONE call by the block above */
    /*
     * A TRANSCRIPT OF THE SHARED MEDIUM.
     *
     * Several of the election properties are statements about what was SAID,
     * not about where the nodes ended up -- "the same offer was retransmitted,
     * not a new one", "only one acceptance named this transaction". Reading
     * them off the coordinators' final state would be inferring the wire from
     * the endpoints, which is how a re-generated reference could hide.
     */
    logged_t log[MAX_LOG];
    unsigned log_count;
    unsigned log_overflow;
} room_t;

typedef struct {
    room_t *room;
    uint8_t index;
} peer_ctx_t;

static room_t *g_room;
static mcl_rdv_t *g_nodes[MAX_NODES];
static unsigned g_node_count;

static int32_t room_tx(void *user, uint8_t transport_id,
                       const uint8_t *data, size_t size)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;
    room_t *room = ctx->room;
    transmission_t *t;
    unsigned i;

    if (room->deaf) {
        return room->tx_unknown ? 1 : 0;
    }
    if (room->count >= MAX_TX || size > MAX_FRAME_SIZE) {
        return room->tx_unknown ? 1 : 0;
    }
    room->tx_say_unknown = 0;
    if (room->tx_force_left > 0 && transport_id == room->shared_transport) {
        room->tx_force_left--;
        room->tx_forced++;
        if (room->tx_force < 0) {
            return -1;      /* refused outright: no airtime, no delivery */
        }
        if (room->tx_force > 0) {
            room->tx_say_unknown = 1;
        }
    }
    /*
     * Log and filter before anything else, so a destroyed acceptance still
     * appears in the transcript. A frame that was emitted and then lost is a
     * different fact from one that was never emitted, and a test that asserts
     * "the same acceptance was sent twice" needs both of them.
     */
    if (transport_id == room->shared_transport) {
        mcl_wire_tier0_t object;
        size_t consumed = 0u;

        if (mcl_wire_tier0_decode(data, size, &object, &consumed)
            == MCL_WIRE_OK) {
            if (room->log_count < MAX_LOG) {
                logged_t *e = &room->log[room->log_count++];
                memset(e, 0, sizeof(*e));
                e->at_ms = room->now;
                e->kind = (uint8_t)object.kind;
                e->from = ctx->index;
                e->source_ref = object.source_ref;
                if (object.kind == MCL_WIRE_KIND_TRANSPORT_OFFER) {
                    e->migration_ref = object.body.transport_offer.migration_ref;
                    e->transport_id = object.body.transport_offer.transport_id;
                    e->profile_id = object.body.transport_offer.profile_id;
                    e->endpoint_token =
                        object.body.transport_offer.endpoint_token;
                } else if (object.kind == MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
                    e->migration_ref =
                        object.body.transport_accept.migration_ref;
                    e->transport_id = object.body.transport_accept.transport_id;
                    e->profile_id = object.body.transport_accept.profile_id;
                    e->session_ref = object.body.transport_accept.session_ref;
                }
            } else {
                room->log_overflow++;
            }
            if (room->drop_accepts > 0 &&
                object.kind == MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
                room->drop_accepts--;
                room->accepts_dropped++;
                /* Destroyed at the transmitter, so it occupies no air and
                   cannot collide with anything -- the honest model of a frame
                   the far end never decoded. */
                return room->tx_unknown ? 1 : 0;
            }
        }
    }
    t = &room->tx[room->count++];
    memset(t, 0, sizeof(*t));
    t->transport_id = transport_id;
    memcpy(t->bytes, data, size);
    t->size = size;
    t->from = ctx->index;
    /*
     * The loss filter sits at the transmitter, before airtime is modelled, so
     * a destroyed control occupies no air and cannot collide with anything.
     * That is the honest model of a frame the far end never decoded.
     */
    if (room->drop_op >= 0 && transport_id != room->shared_transport &&
        size > 0u) {
        /* A handoff control's operation byte, located the same way
           mcl_handoff_decode locates it: after the Link frame header. */
        size_t k;
        for (k = 0u; k + 1u < size; ++k) {
            if (data[k] == (uint8_t)room->drop_op) {
                room->dropped++;
                room->drop_op = -1;         /* first occurrence only */
                room->count--;              /* un-record this transmission */
                return 0;
            }
        }
    }
    t->start_ms = room->now;
    /* A point-to-point candidate bearer is effectively instantaneous here; the
       shared bearer costs real airtime. */
    t->end_ms = room->now
                + ((transport_id == room->shared_transport)
                   ? airtime_ms(size) : 1u);

    /* Overlap with anything still on the same shared medium is a collision for
       BOTH transmissions. */
    if (transport_id == room->shared_transport) {
        for (i = 0u; i + 1u < room->count; ++i) {
            transmission_t *o = &room->tx[i];
            if (o->transport_id != transport_id || o->delivered) continue;
            if (o->start_ms < t->end_ms && t->start_ms < o->end_ms) {
                o->collided = 1u;
                t->collided = 1u;
            }
        }
    }
    return (room->tx_unknown || room->tx_say_unknown) ? 1 : 0;
}

static uint32_t room_now(void *user)
{
    return ((peer_ctx_t *)user)->room->now;
}

static int room_medium_busy(void *user)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;
    room_t *room = ctx->room;
    unsigned i;

    for (i = 0u; i < room->count; ++i) {
        transmission_t *t = &room->tx[i];
        if (t->delivered || t->transport_id != room->shared_transport) continue;
        if (t->from == ctx->index) continue;      /* our own emitter */
        /*
         * STRICTLY LESS THAN. A TRANSMISSION BEGINNING NOW IS NOT YET AUDIBLE.
         *
         * The nodes are polled one after another at a single simulated
         * instant. With `<=`, whoever was polled first began transmitting and
         * was instantly detectable, so everyone polled afterwards at that same
         * instant sensed a busy medium and deferred -- and two machines that
         * had chosen the SAME slot never collided.
         *
         * That is not physics. If A and B pick the same slot, both sense the
         * medium before either has put energy into it, both find it idle, and
         * both transmit. The simulator was quietly guaranteeing the outcome
         * the slot scheme is only supposed to make unlikely, and hiding the
         * residual collision probability it has to tolerate.
         *
         * room_tx() still treats same-instant starts as overlapping, so the
         * collision that follows is real. Self-sensing below keeps `<=`: a
         * node does know it is transmitting at the instant it starts.
         */
        if (t->start_ms < room->now && room->now < t->end_ms) {
            return 1;
        }
    }
    return 0;
}

static int room_self_transmitting(void *user)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;
    room_t *room = ctx->room;
    unsigned i;

    for (i = 0u; i < room->count; ++i) {
        transmission_t *t = &room->tx[i];
        if (t->delivered || t->from != ctx->index) continue;
        if (t->start_ms <= room->now && room->now < t->end_ms) {
            return 1;
        }
    }
    return 0;
}

static int room_random(void *user, uint8_t *out, size_t size)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;
    room_t *room = ctx->room;
    size_t i;

    if (!room->random_enabled) {
        return -1;
    }
    if (room->force_same_slot > 0) {
        /*
         * Every node draws the same bytes, so every node computes the same
         * backoff slot and the collision the scheme exists to make unlikely
         * becomes certain. That is the only way to check it is SURVIVED rather
         * than merely improbable.
         *
         * It is a countdown, not a mode. Forcing every draw for ever makes the
         * randomness permanently degenerate, the nodes collide on every round
         * until the run ends, and the test can only assert the collision -- not
         * the recovery, which is the more interesting half.
         */
        room->force_same_slot--;
        for (i = 0u; i < size; ++i) {
            out[i] = 0x40u;
        }
        return 0;
    }
    for (i = 0u; i < size; ++i) {
        room->seed[ctx->index] =
            room->seed[ctx->index] * 1103515245u + 12345u;
        out[i] = (uint8_t)(room->seed[ctx->index] >> 16);
    }
    return 0;
}

/* Advance the clock, delivering transmissions that have finished. */
static void room_advance(room_t *room, uint32_t step_ms)
{
    unsigned i, j;

    room->now += step_ms;
    for (i = 0u; i < room->count; ++i) {
        transmission_t *t = &room->tx[i];
        if (t->delivered || room->now < t->end_ms) continue;
        t->delivered = 1u;
        if (t->collided) {
            room->collisions++;
            continue;                     /* lost to overlap: nobody hears it */
        }
        room->delivered++;
        for (j = 0u; j < g_node_count; ++j) {
            if (t->from == (uint8_t)j) continue;
            (void)mcl_rdv_deliver(g_nodes[j], t->transport_id,
                                  t->bytes, t->size);
        }
    }

    /*
     * RECYCLE FINISHED TRANSMISSIONS.
     *
     * room->count only ever grew, and room_tx() silently drops anything past
     * MAX_TX. A long run therefore stopped carrying traffic partway through
     * and every node went quiet -- which looks exactly like a protocol that
     * cannot converge. A pair did form in the 120-second contention run and
     * then failed its handoff, because the handoff frames were past the cap
     * and never existed.
     *
     * A simulator that silently stops delivering is worse than one that is too
     * small: it produces a plausible negative result. Compacting keeps the
     * array bounded by what is actually IN FLIGHT rather than by the whole
     * history of the run.
     */
    {
        unsigned keep = 0u;
        for (i = 0u; i < room->count; ++i) {
            if (room->tx[i].delivered) continue;
            if (keep != i) room->tx[keep] = room->tx[i];
            keep++;
        }
        room->count = keep;
    }
}

/*
 * THE BUILDER'S SIDE OF THE BOUNDARY, MODELLED.
 *
 * The coordinator now stops twice and waits for the integrator: once when the
 * bearer is agreed, so the candidate can be opened, and once when reachability
 * is proven, so local policy can admit or refuse. A test harness that did not
 * answer those would stall for ever, and one that answered them inside the
 * coordinator would be testing a coordinator nobody ships.
 *
 * So the harness plays the builder: it opens the candidate and it admits. The
 * two flags let a test withhold either one, which is how the policy-refusal
 * case and the candidate-not-open case are exercised.
 */
static int g_auto_ready = 1;
static int g_auto_admit = 1;
static unsigned g_policy_asked;
/*
 * Restart a coordinator that has given up.
 *
 * The coordinator deliberately stops: IDLE when nobody answered its
 * announcements, EXHAUSTED when every mandated bearer was offered and refused.
 * Both are REPORTABLE outcomes handed to the application, which decides
 * whether to keep looking. That is a design choice, not an omission.
 *
 * A three-machine contention test therefore has to model an application that
 * does keep looking, or it measures how quickly the nodes give up rather than
 * whether they converge. Off by default: the tests that assert a node stops
 * are asserting exactly that.
 */
static int g_auto_restart;
static unsigned g_ready_failures;

/*
 * THE INTEGRATOR'S SIDE OF THE ALLOCATOR BOUNDARY.
 *
 * Both hooks are optional and every other case in this file leaves them NULL,
 * which is the point: a builder who supplies neither must get exactly the
 * behaviour that existed before they were added.
 */
static unsigned g_session_calls;
static unsigned g_token_calls;
static int g_session_refuses;          /* the allocator declines */
static int g_token_refuses;
static uint32_t g_next_session;
static uint32_t g_next_token;

static int alloc_session(void *user, uint32_t *out)
{
    (void)user;
    g_session_calls++;
    if (g_session_refuses) {
        return -1;
    }
    *out = g_next_session;
    g_next_session += 0x1000u;
    return 0;
}

static int alloc_token(void *user, uint8_t transport_id, uint8_t profile_id,
                       uint32_t *out)
{
    (void)user;
    (void)profile_id;
    g_token_calls++;
    if (g_token_refuses) {
        return -1;
    }
    /* Distinct per bearer AND per call, so a reused token is visible. */
    *out = g_next_token + (uint32_t)transport_id;
    g_next_token += 0x10000u;
    return 0;
}

static void tick(room_t *room, mcl_rdv_event_t *events, uint32_t step_ms)
{
    unsigned j;

    for (j = 0u; j < g_node_count; ++j) {
        (void)mcl_rdv_poll(g_nodes[j], &events[j]);
        switch (events[j].kind) {
        case MCL_RDV_EVENT_BEARER_AGREED:
            if (g_auto_ready) {
                /* Not discarded: a builder that ignored this would have no
                   idea its candidate never opened, which is exactly the
                   silence being hunted here. */
                if (mcl_rdv_candidate_ready(g_nodes[j]) != MCL_RDV_OK) {
                    g_ready_failures++;
                }
            }
            break;
        case MCL_RDV_EVENT_POLICY_REQUIRED:
            g_policy_asked++;
            if (g_auto_admit) {
                (void)mcl_rdv_admit(g_nodes[j]);
            } else {
                (void)mcl_rdv_refuse(g_nodes[j]);
            }
            break;
        default:
            break;
        }
        if (g_auto_restart &&
            (g_nodes[j]->state == MCL_RDV_STATE_IDLE ||
             g_nodes[j]->state == MCL_RDV_STATE_EXHAUSTED)) {
            g_nodes[j]->state = MCL_RDV_STATE_IDLE;
            (void)mcl_rdv_start(g_nodes[j]);
        }
    }
    room_advance(room, step_ms);
}

/*
 * Every test below drives the two builder boundaries through tick(), and EVERY
 * ONE OF THEM CALLS THIS FIRST.
 *
 * It was called by one test, which set g_auto_restart and never cleared it --
 * so every case that ran afterwards had its coordinators silently restarted
 * the instant they reached IDLE or EXHAUSTED. The disjoint-bearer case then
 * failed its final state check for a reason that had nothing to do with
 * bearers: the node DID reach EXHAUSTED, reported NO_COMMON_BEARER, and was
 * put back to work before the assertion could see it. That failure was read as
 * a protocol defect and a timing window was widened chasing it.
 *
 * Global harness state that one test sets and the next inherits produces
 * failures whose cause is in a different function, so the reset is
 * unconditional rather than left to the test that happens to need it.
 */
static void reset_harness(void)
{
    g_auto_ready = 1;
    g_auto_admit = 1;
    g_auto_restart = 0;
    g_policy_asked = 0u;
    g_ready_failures = 0u;
    g_session_calls = 0u;
    g_token_calls = 0u;
    g_session_refuses = 0;
    g_token_refuses = 0;
    g_next_session = 0x7E550001u;
    g_next_token = 0x00A00000u;
}

static void base_config(mcl_rdv_config_t *cfg, uint32_t source_ref,
                        mcl_contact_role_t role)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->source_ref = source_ref;
    cfg->bootstrap_transport_id = 1u;      /* MCL_AP */
    cfg->bearer_count = 2u;
    cfg->bearer_transport_id[0] = 3u;      /* MCL_BLE */
    cfg->bearer_profile_id[0] = 1u;        /* BLE-GATT = 1 */
    cfg->bearer_transport_id[1] = 2u;      /* MCL_IP */
    cfg->bearer_profile_id[1] = 1u;        /* IP-DATAGRAM = 1 */
    cfg->role = role;
    cfg->shared_medium = 1u;
    /*
     * LEFT ZERO ON PURPOSE: zero means "the profile's value".
     *
     * These were 4000 ms, 40 announcements and 1 retry -- numbers this test
     * chose for itself. A shared-medium configuration that deviates from
     * AP-BOOTSTRAP-1 is now refused by mcl_rdv_init(), and the fixture must be
     * the first thing held to that rule rather than the first thing excused
     * from it. Exercising the zero path also checks that a builder who fills
     * in nothing at all gets the interoperable behaviour.
     */
    cfg->capability_tag = 0x000004u;
    cfg->presence_ttl = 60u;
}

static void base_platform(mcl_rdv_platform_t *p, peer_ctx_t *ctx)
{
    memset(p, 0, sizeof(*p));
    p->tx = room_tx;
    p->now_ms = room_now;
    p->random = room_random;
    p->medium_busy = room_medium_busy;
    p->self_transmitting = room_self_transmitting;
    p->user = ctx;
}

/*
 * The node needs its OWN tx_fn. The coordinator emits Tier-0 objects through
 * platform.tx, but handoff controls go out through mcl_node_send_handoff, which
 * uses the node's transmit function and the transport the CONTACT chooses.
 * Leaving it NULL made every handoff control fail to send, so the migration
 * stages ran and silently accomplished nothing.
 */
static void base_node(mcl_node_t *node, uint32_t source_ref,
                      mcl_contact_role_t role, peer_ctx_t *ctx)
{
    mcl_node_config_t nc;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = source_ref;
    nc.transport_id = 1u;
    nc.role = role;
    nc.tx_fn = room_tx;
    nc.user_ctx = ctx;
    (void)mcl_node_init(node, &nc);
}

static void room_init(room_t *room)
{
    unsigned i;
    memset(room, 0, sizeof(*room));
    room->shared_transport = 1u;
    room->random_enabled = 1;
    room->drop_op = -1;
    for (i = 0u; i < MAX_NODES; ++i) {
        room->seed[i] = 0x1234567u + i * 7919u;
    }
}

/* Defined with the election cases below, and used by the contention runs
   above: the transcript invariant that names the three-machine defect. */
static unsigned self_answered_offers(const room_t *room);

/* ------------------------------------------------------------ the cases */

static void test_config_refusals(void)
{
    mcl_rdv_t r;
    mcl_rdv_config_t cfg;
    mcl_rdv_platform_t plat;
    mcl_node_t node;
    room_t room;
    peer_ctx_t ctx;

    reset_harness();
    printf("[rendezvous] configuration refusals\n");
    room_init(&room);
    ctx.room = &room; ctx.index = 0u;
    base_platform(&plat, &ctx);
    base_node(&node, 0xA1u, MCL_CONTACT_ROLE_INITIATOR, &ctx);

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    check(mcl_rdv_init(NULL, &cfg, &plat, &node) == MCL_RDV_ERR_NULL,
          "init refuses a NULL coordinator");

    /*
     * transport_id 0 is reserved so an uninitialised field never names a
     * medium. Accepting it would put a meaningless offer on the air, where the
     * peer correctly ignores it, and the deployment would look like a bearer
     * mismatch instead of a configuration error.
     */
    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    cfg.bearer_transport_id[1] = 0u;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses a bearer with transport_id 0");

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    cfg.bearer_count = 0u;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses an empty bearer list");

    /*
     * THE TWO REFUSALS THAT STOP AN UNMEETABLE CLAIM.
     *
     * A shared-medium deployment without randomness backs off by a function of
     * source_ref, which has no uniqueness property -- two builders that legally
     * chose the same value collide on every round forever. Without medium
     * sensing a backoff can only delay, and delay alone cannot separate
     * transmissions longer than the window. Both are refused at init rather
     * than degraded at runtime, where a two-node test would hide them.
     */
    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    base_platform(&plat, &ctx);
    plat.random = NULL;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "a shared-medium claim without randomness is refused");

    base_platform(&plat, &ctx);
    plat.medium_busy = NULL;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "a shared-medium claim without medium sensing is refused");

    /* The same platform is fine for a point-to-point bearer, which needs
       neither, and saying so keeps the refusal narrow. */
    base_platform(&plat, &ctx);
    plat.random = NULL;
    plat.medium_busy = NULL;
    cfg.shared_medium = 0u;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_OK,
          "and is not required of a point-to-point deployment");

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    base_platform(&plat, &ctx);
    plat.now_ms = NULL;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses a platform with no clock");
}

/*
 * Two strangers with no prearrangement reach agreement, validate the candidate
 * path and MIGRATE. The last two were declared events that no code could emit.
 */
static void test_two_strangers_to_migration(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int agreed = 0, validated = 0, migrated = 0, security = 0;

    reset_harness();
    printf("[rendezvous] two strangers, through to migration\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    base_platform(&pa, &xa);
    base_platform(&pb, &xb);
    base_node(&na, 0x11111111u, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_node(&nb, 0x22222222u, MCL_CONTACT_ROLE_RESPONDER, &xb);
    base_config(&ca, 0x11111111u, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x22222222u, MCL_CONTACT_ROLE_RESPONDER);
    ca.bearer_endpoint_token[0] = 0xAAAA0001u;
    cb.bearer_endpoint_token[0] = 0xBBBB0001u;

    check(mcl_rdv_init(&a, &ca, &pa, &na) == MCL_RDV_OK, "A initialises");
    check(mcl_rdv_init(&b, &cb, &pb, &nb) == MCL_RDV_OK, "B initialises");
    check(mcl_rdv_start(&a) == MCL_RDV_OK, "A starts");
    check(mcl_rdv_start(&b) == MCL_RDV_OK, "B starts");
    check(mcl_rdv_start(&a) == MCL_RDV_ERR_STATE,
          "start refuses a second time");

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    for (i = 0u; i < 4000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED ||
            ev[1].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed = 1;
        if (ev[0].kind == MCL_RDV_EVENT_CANDIDATE_VALIDATED ||
            ev[1].kind == MCL_RDV_EVENT_CANDIDATE_VALIDATED) validated = 1;
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
        if (ev[0].kind == MCL_RDV_EVENT_SECURITY_ESTABLISHED ||
            ev[1].kind == MCL_RDV_EVENT_SECURITY_ESTABLISHED) security = 1;
    }

    {
        unsigned k, on_candidate = 0u;
        for (k = 0u; k < room.count; ++k) {
            if (room.tx[k].transport_id != 1u) on_candidate++;
        }
        printf("       (candidate-bearer transmissions: %u)\n", on_candidate);
    }
    printf("       (A state=%d ctrl=%u mig=%08lX ses=%08lX | "
           "B state=%d ctrl=%u mig=%08lX ses=%08lX)\n",
           (int)a.state, (unsigned)a.is_controller,
           (unsigned long)a.migration_ref, (unsigned long)a.session_ref,
           (int)b.state, (unsigned)b.is_controller,
           (unsigned long)b.migration_ref, (unsigned long)b.session_ref);
    check(agreed, "two strangers reach BEARER_AGREED with no prearrangement");
    check(validated, "the candidate path is VALIDATED, not assumed");
    check(migrated, "and the contact actually MIGRATES");
    check(a.state == MCL_RDV_STATE_MIGRATED || b.state == MCL_RDV_STATE_MIGRATED,
          "at least one peer ends in MIGRATED");

    /*
     * This check can never pass in this release. MCL has no Stable SECURITY
     * class and no assigned feature bits, so there is nowhere legal for
     * handshake bytes to travel; emitting the event would claim a property no
     * code here provides. It exists so the vocabulary survives MCL-S1.
     */
    check(!security,
          "no build of this release ever claims security is established");

    /* Both peers must be talking about ONE transaction. "Both AGREED" is also
       what two unresolved collisions look like. */
    check(a.migration_ref == b.migration_ref && a.migration_ref != 0u,
          "both peers hold the SAME migration transaction");
    check(a.session_ref == b.session_ref && a.session_ref != 0u,
          "and the same session reference, bound once by the accepting peer");
}

/*
 * THE CROSS-PAIR BUG A TWO-NODE ROOM CANNOT CONTAIN.
 *
 * A announces; B and C both hear it and both set peer_ref = A. C's offer then
 * reaches B. Without peer scoping B accepted it on a bearer-list match alone,
 * so three machines could produce a B-C agreement neither was attempting --
 * worse than the collision the campaign was looking for, and easily read as one.
 */
static void test_third_party_offer_refused(void)
{
    mcl_rdv_t a, b, c;
    mcl_rdv_event_t ev[3];
    mcl_rdv_config_t ca, cb, cc;
    mcl_rdv_platform_t pa, pb, pc;
    mcl_node_t na, nb, nc;
    room_t room;
    peer_ctx_t xa, xb, xc;
    unsigned i;

    reset_harness();
    printf("[rendezvous] a third machine cannot steal the pair\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    xc.room = &room; xc.index = 2u;
    base_platform(&pa, &xa);
    base_platform(&pb, &xb);
    base_platform(&pc, &xc);
    base_node(&na, 0x0A0A0A0Au, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_node(&nb, 0x0B0B0B0Bu, MCL_CONTACT_ROLE_RESPONDER, &xb);
    base_node(&nc, 0x0C0C0C0Cu, MCL_CONTACT_ROLE_RESPONDER, &xc);
    base_config(&ca, 0x0A0A0A0Au, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x0B0B0B0Bu, MCL_CONTACT_ROLE_RESPONDER);
    base_config(&cc, 0x0C0C0C0Cu, MCL_CONTACT_ROLE_RESPONDER);

    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_init(&c, &cc, &pc, &nc);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);
    (void)mcl_rdv_start(&c);

    g_room = &room;
    g_nodes[0] = &a; g_nodes[1] = &b; g_nodes[2] = &c; g_node_count = 3u;
    for (i = 0u; i < 4000u; ++i) {
        tick(&room, ev, 10u);
    }

    /*
     * Whatever else happened, no peer may have settled on a peer it never
     * heard a PRESENCE from, and B and C -- which both only ever heard A --
     * must not have paired with each other.
     */
    check(!(b.peer_seen && b.peer_ref == cc.source_ref),
          "B never adopts C as its peer");
    check(!(c.peer_seen && c.peer_ref == cb.source_ref),
          "C never adopts B as its peer");
    printf("       (3 nodes: %u transmissions delivered, %u lost to overlap)\n",
           room.delivered, room.collisions);
}

/*
 * Contention, measured rather than asserted. Three machines started together
 * on one medium: the old rule guaranteed overlap because its window was
 * shorter than a frame. What is required now is not zero collisions -- a
 * shared medium has them -- but that the scheme RECOVERS, so useful traffic
 * still gets through.
 */
static void test_contention_recovers(void)
{
    mcl_rdv_t a, b, c;
    mcl_rdv_event_t ev[3];
    mcl_rdv_config_t cfg[3];
    mcl_rdv_platform_t plat[3];
    mcl_node_t node[3];
    room_t room;
    peer_ctx_t ctx[3];
    unsigned i, k;
    unsigned migrated_nodes = 0u, validated = 0u;
    unsigned evcount[9] = {0,0,0,0,0,0,0,0,0};
    uint32_t migrated_session = 0u;
    static const uint32_t refs[3] = { 0x51515151u, 0x52525252u, 0x53535353u };

    reset_harness();
    printf("[rendezvous] three machines contending on one medium\n");
    /* Machines that keep looking, which is what an application does
       with NO_COMMON_BEARER and with a node that has gone quiet. */
    g_auto_restart = 1;
    room_init(&room);
    for (i = 0u; i < 3u; ++i) {
        ctx[i].room = &room; ctx[i].index = (uint8_t)i;
        base_platform(&plat[i], &ctx[i]);
        base_node(&node[i], refs[i],
                  (i == 0u) ? MCL_CONTACT_ROLE_INITIATOR
                            : MCL_CONTACT_ROLE_RESPONDER, &ctx[i]);
        base_config(&cfg[i], refs[i],
                    (i == 0u) ? MCL_CONTACT_ROLE_INITIATOR
                              : MCL_CONTACT_ROLE_RESPONDER);
    }
    (void)mcl_rdv_init(&a, &cfg[0], &plat[0], &node[0]);
    (void)mcl_rdv_init(&b, &cfg[1], &plat[1], &node[1]);
    (void)mcl_rdv_init(&c, &cfg[2], &plat[2], &node[2]);
    /* All three start at the same instant, which is the phase-locking case. */
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);
    (void)mcl_rdv_start(&c);

    g_room = &room;
    g_nodes[0] = &a; g_nodes[1] = &b; g_nodes[2] = &c; g_node_count = 3u;
    /*
     * 120 s of simulated time, not 30.
     *
     * The response timeout is now DERIVED from the profile at 6000 ms rather
     * than guessed at 3000, and TRANSPORT_ACCEPT contends for the medium like
     * everything else instead of going out inline. Both are correct and both
     * make a rendezvous take longer, so a window sized against the old
     * behaviour was measuring the window rather than the protocol.
     */
    for (i = 0u; i < 60000u; ++i) {
        tick(&room, ev, 10u);
        for (k = 0u; k < 3u; ++k) {
            if (ev[k].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) {
                migrated_nodes++;
                migrated_session = ev[k].session_ref;
            }
            if (ev[k].kind == MCL_RDV_EVENT_CANDIDATE_VALIDATED) validated++;
            if (ev[k].kind != MCL_RDV_EVENT_NONE &&
                (unsigned)ev[k].kind < 9u) evcount[(unsigned)ev[k].kind]++;
        }
    }

    printf("       (%u delivered, %u lost to overlap)\n",
           room.delivered, room.collisions);
    printf("       (states %u %u %u; announcements %u %u %u)\n",
           (unsigned)a.state, (unsigned)b.state, (unsigned)c.state,
           (unsigned)a.announcements, (unsigned)b.announcements,
           (unsigned)c.announcements);
    printf("       (events disc=%u agreed=%u valid=%u migr=%u nocommon=%u "
           "lost=%u policy=%u)\n",
           evcount[1], evcount[2], evcount[3], evcount[4],
           evcount[5], evcount[6], evcount[7]);
    printf("       (candidate_ready failures: %u)\n", g_ready_failures);
    check(room.delivered > 0u,
          "traffic gets through three simultaneous starters");
    /*
     * The load-bearing check. Under the old rule every reply to a shared
     * PRESENCE overlapped by construction, so a majority-collision outcome is
     * the signature of a scheme that cannot work at all.
     */
    /*
     * "SOME TRAFFIC GOT THROUGH" IS NOT RENDEZVOUS.
     *
     * delivered > 0 and delivered > collisions can both hold while no pair
     * ever agrees on anything: they are statements about the medium, not
     * about the protocol running on it. Three machines starting together must
     * end with a coherent transaction -- a validated candidate, a migrated
     * contact, and exactly one session for it.
     */
    check(validated > 0u,
          "a candidate path is validated despite the contention");
    check(migrated_nodes >= 2u,
          "a PAIR migrates -- both ends of one contact, not one hopeful end");
    check(migrated_session != 0u,
          "the migrated contact carries a real session reference");
    check(room.delivered > room.collisions,
          "and most transmissions survive rather than most colliding");
    check(self_answered_offers(&room) == 0u,
          "and no machine ever answered its own solicitation");
    reset_harness();
}

/*
 * Two builders that legally chose the SAME source_ref must still discover each
 * other. wire.h assigns source_ref no uniqueness property, so this is a legal
 * configuration -- and the first implementation, which suppressed self-echo by
 * comparing source_ref, made both machines silently deaf to the other.
 */
static void test_colliding_source_refs(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int discovered = 0, migrated = 0;

    reset_harness();
    printf("[rendezvous] two builders that chose the same source_ref\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    base_platform(&pa, &xa);
    base_platform(&pb, &xb);
    base_node(&na, 0x5A5A5A5Au, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_node(&nb, 0x5A5A5A5Au, MCL_CONTACT_ROLE_RESPONDER, &xb);
    base_config(&ca, 0x5A5A5A5Au, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x5A5A5A5Au, MCL_CONTACT_ROLE_RESPONDER);

    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    for (i = 0u; i < 8000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_PEER_DISCOVERED ||
            ev[1].kind == MCL_RDV_EVENT_PEER_DISCOVERED) discovered = 1;
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
    }
    check(discovered,
          "self-echo suppression uses the platform, not source_ref equality");
    /*
     * DISCOVERY IS NOT ENOUGH ANY MORE, AND THE REASON IS NEW.
     *
     * migration_ref selects a responder now, so a reference derived from
     * source_ref would be identical on both of these machines' first
     * transactions -- and one acceptance would name both of them. That is
     * exactly why the shared-medium path draws it from platform.random. The
     * case has to run all the way to migration to say anything about it.
     */
    check(migrated, "two builders sharing a source_ref still migrate");
    check(a.session_ref == b.session_ref && a.session_ref != 0u,
          "on one session reference");
    check(a.migration_ref == b.migration_ref && a.migration_ref != 0u,
          "and one transaction, named unambiguously");
}

/*
 * The longest a responder can legally take to try every mandated bearer and
 * report that none was accepted -- derived from the profile, not guessed.
 *
 *   discovery   max_announcements x (solicit timeout + maximum backoff),
 *               the rounds this node may spend before it hears a
 *               solicitation at all
 *   per attempt maximum backoff + one maximum frame's deferral
 *                               + one response timeout
 *   per bearer  (max_offer_retries + 1) attempts
 *
 * At the profile's numbers, for one bearer: 97 500 + 31 611 = 129 111 ms.
 */
static uint32_t exhaust_bound_ms(uint8_t bearers)
{
    uint32_t backoff = (uint32_t)(MCL_RDV_AP1_BACKOFF_SLOTS - 1u)
                       * (uint32_t)MCL_RDV_AP1_BACKOFF_SLOT_MS;
    uint32_t attempt = backoff + airtime_ms(17u)
                       + (uint32_t)MCL_RDV_AP1_RESPONSE_TIMEOUT_MS;
    uint32_t per_bearer =
        (uint32_t)(MCL_RDV_AP1_MAX_OFFER_RETRIES + 1u) * attempt;
    uint32_t discovery = (uint32_t)MCL_RDV_AP1_MAX_ANNOUNCEMENTS
                         * ((uint32_t)MCL_RDV_AP1_SOLICIT_TIMEOUT_MS + backoff);
    return discovery + (uint32_t)bearers * per_bearer;
}

/*
 * Disjoint bearer sets: they hear each other perfectly and still cannot
 * continue, and that outcome is REPORTED rather than left as a silence.
 */
static void test_no_common_bearer(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int none = 0, agreed = 0;

    reset_harness();
    printf("[rendezvous] disjoint bearer sets\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    base_platform(&pa, &xa);
    base_platform(&pb, &xb);
    base_node(&na, 0x33333333u, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_node(&nb, 0x44444444u, MCL_CONTACT_ROLE_RESPONDER, &xb);

    base_config(&ca, 0x33333333u, MCL_CONTACT_ROLE_INITIATOR);
    ca.bearer_count = 1u;
    ca.bearer_transport_id[0] = 3u;      /* BLE only */
    base_config(&cb, 0x44444444u, MCL_CONTACT_ROLE_RESPONDER);
    cb.bearer_count = 1u;
    cb.bearer_transport_id[0] = 4u;      /* UWB only */

    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    /*
     * RUN UNTIL THE OUTCOME, OR UNTIL THE PROFILE SAYS IT CANNOT COME.
     *
     * This was `for (i = 0; i < 24000; ++i)` -- 240 s, arrived at by doubling
     * 120 s after the case went red. It stayed red, so the number was never
     * the problem: the real cause was harness state leaking in from an earlier
     * test (see reset_harness). A loop count arrived at by trial is a stopwatch
     * the case measures instead of the protocol, and the next legitimate
     * timing change quietly turns it back into one.
     *
     * The bound below is COMPUTED from the frozen AP-BOOTSTRAP-1 constants and
     * this configuration's bearer count, so a profile change moves it
     * automatically and a wrong bound is a visible arithmetic error rather
     * than an intermittent failure.
     */
    for (i = 0u; i * 10u < exhaust_bound_ms(1u) && !none; ++i) {
        tick(&room, ev, 10u);
        /*
         * EITHER peer reporting it is the result. Only one of them may ever
         * get there: the two first announcements collide, the peer that heard
         * the other leaves ANNOUNCING and stops emitting PRESENCE, and the one
         * that heard nothing keeps announcing to a room that has gone quiet.
         * That is correct behaviour -- a machine with no peer has nothing to
         * report about bearers -- and checking only node A asserted a
         * coincidence rather than a property.
         */
        if (ev[0].kind == MCL_RDV_EVENT_NO_COMMON_BEARER ||
            ev[1].kind == MCL_RDV_EVENT_NO_COMMON_BEARER) none = 1;
        if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED ||
            ev[1].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed = 1;
    }
    printf("       (reported after %u ms of a %u ms bound; "
           "A state=%d bearer_index=%u | B state=%d bearer_index=%u)\n",
           i * 10u, exhaust_bound_ms(1u),
           (int)a.state, (unsigned)a.bearer_index,
           (int)b.state, (unsigned)b.bearer_index);
    check(!agreed, "disjoint bearer sets never reach agreement");
    check(none,
          "NO_COMMON_BEARER is reported within the profile's own bound");
    check(a.state == MCL_RDV_STATE_EXHAUSTED ||
          b.state == MCL_RDV_STATE_EXHAUSTED,
          "and the peer that tried every bearer says so in its state");
}

/*
 * The same transport offered under a profile this deployment does not
 * implement must be refused. Matching the transport alone completes a
 * migration onto a bearer where nothing can then be exchanged.
 */
static void test_profile_must_match(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int agreed = 0;

    reset_harness();
    printf("[rendezvous] the profile must match, not just the transport\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    base_platform(&pa, &xa);
    base_platform(&pb, &xb);
    base_node(&na, 0x6A6A6A6Au, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_node(&nb, 0x6B6B6B6Bu, MCL_CONTACT_ROLE_RESPONDER, &xb);

    base_config(&ca, 0x6A6A6A6Au, MCL_CONTACT_ROLE_INITIATOR);
    ca.bearer_count = 1u;
    ca.bearer_transport_id[0] = 3u;
    ca.bearer_profile_id[0] = 1u;        /* BLE-GATT = 1 */
    base_config(&cb, 0x6B6B6B6Bu, MCL_CONTACT_ROLE_RESPONDER);
    cb.bearer_count = 1u;
    cb.bearer_transport_id[0] = 3u;      /* same transport ... */
    cb.bearer_profile_id[0] = 192u;      /* ... different profile */

    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    for (i = 0u; i < 4000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED ||
            ev[1].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed = 1;
    }
    check(!agreed,
          "a shared transport under a different profile is not agreement");
}

/*
 * Every transaction gets a fresh reference. The first version computed
 * `source_ref | 1`, a constant, so an abandoned transaction and its successor
 * were indistinguishable -- and the stale-acceptance guard that exists to tell
 * them apart was comparing a number with itself.
 */
static void test_fresh_transaction_refs(void)
{
    mcl_rdv_t a;
    mcl_rdv_event_t ev[1];
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    room_t room;
    peer_ctx_t xa;
    mcl_wire_tier0_t presence;
    uint8_t bytes[MCL_WIRE_TIER0_MAX_SIZE];
    size_t written = 0u;
    unsigned i;
    uint32_t first = 0u, second = 0u;

    reset_harness();
    printf("[rendezvous] a fresh reference per transaction\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 0x88888888u, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&ca, 0x88888888u, MCL_CONTACT_ROLE_INITIATOR);
    ca.max_offer_retries = 0u;      /* move to the next bearer promptly */
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    g_room = &room; g_nodes[0] = &a; g_node_count = 1u;

    memset(&presence, 0, sizeof(presence));
    presence.kind = MCL_WIRE_KIND_PRESENCE;
    presence.priority = 1u;
    presence.source_ref = 0x99999999u;
    presence.body.presence.capability_tag = 1u;
    presence.body.presence.ttl = 60u;
    (void)mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &presence,
                                         bytes, sizeof(bytes), &written);
    (void)mcl_rdv_deliver(&a, 1u, bytes, written);

    for (i = 0u; i < 3000u; ++i) {
        tick(&room, ev, 10u);
        if (first == 0u && a.state == MCL_RDV_STATE_OFFERING) {
            first = a.migration_ref;
        }
        if (first != 0u && a.migration_ref != 0u && a.migration_ref != first) {
            second = a.migration_ref;
            break;
        }
    }
    check(first != 0u, "a transaction reference is non-zero");
    check(second != 0u && second != first,
          "the next bearer gets a DIFFERENT reference, not the same one again");
}

/* An emission heard back must not be treated as a peer. */
static void test_self_echo_ignored(void)
{
    mcl_rdv_t a;
    mcl_rdv_event_t ev;
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    room_t room;
    peer_ctx_t xa;
    mcl_wire_tier0_t echo;
    uint8_t bytes[MCL_WIRE_TIER0_MAX_SIZE];
    size_t written = 0u;

    reset_harness();
    printf("[rendezvous] self-echo\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 0xABCDEF01u, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&ca, 0xABCDEF01u, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);
    g_room = &room; g_nodes[0] = &a; g_node_count = 1u;

    /*
     * WAIT UNTIL IT IS ACTUALLY TRANSMITTING.
     *
     * This polled once and assumed the emitter was live, which held only while
     * the first PRESENCE went out on the very next poll. That instant is now
     * randomised -- three machines started together must not announce
     * together -- so a single poll usually finds the node still counting down
     * its backoff, the echo arrives with nothing on the air, and the node
     * quite correctly treats it as a stranger.
     *
     * The precondition is established rather than assumed.
     */
    {
        unsigned spin;
        for (spin = 0u; spin < 2000u && room.count == 0u; ++spin) {
            (void)mcl_rdv_poll(&a, &ev);
            room_advance(&room, 10u);
        }
        check(room.count > 0u, "the node is transmitting before the echo");
    }

    memset(&echo, 0, sizeof(echo));
    echo.kind = MCL_WIRE_KIND_PRESENCE;
    echo.priority = 1u;
    echo.source_ref = 0xABCDEF01u;
    echo.body.presence.capability_tag = 4u;
    echo.body.presence.ttl = 60u;
    (void)mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &echo,
                                         bytes, sizeof(bytes), &written);
    (void)mcl_rdv_deliver(&a, 1u, bytes, written);
    (void)mcl_rdv_poll(&a, &ev);

    check(ev.kind != MCL_RDV_EVENT_PEER_DISCOVERED,
          "a node does not rendezvous with its own echo");
}

/* Noise on a public bearer is refused, and refusing is not an error. */
static void test_garbage_refused(void)
{
    mcl_rdv_t a;
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    room_t room;
    peer_ctx_t xa;
    static const uint8_t junk[] = { 0xFFu, 0x00u, 0x7Fu, 0x13u, 0x99u };

    reset_harness();
    printf("[rendezvous] noise\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 0x0F0F0F0Fu, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&ca, 0x0F0F0F0Fu, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    check(mcl_rdv_deliver(&a, 1u, junk, sizeof(junk)) == MCL_RDV_OK,
          "undecodable bytes are refused without reporting a fault");
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_ANNOUNCING,
          "and the coordinator has not moved");
}

/*
 * Deadlines are compared by subtraction so they survive the 2^32 wrap of a
 * monotonic millisecond clock. The naive `now >= deadline` form stops firing
 * entirely at about 49.7 days of uptime.
 */
static void test_clock_wrap(void)
{
    mcl_rdv_t a;
    mcl_rdv_event_t ev;
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    room_t room;
    peer_ctx_t xa;
    unsigned i;
    unsigned emissions = 0u;

    reset_harness();
    printf("[rendezvous] the millisecond clock wrap\n");
    room_init(&room);
    room.now = 0xFFFFFF00u;
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 0x5A5A5A5Bu, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&ca, 0x5A5A5A5Bu, MCL_CONTACT_ROLE_INITIATOR);
    /*
     * DECLARED EXPERIMENTAL, because it is.
     *
     * This case needs a 100 ms interval and a single slot so the deadline
     * arithmetic can be isolated from the backoff; those are not the profile's
     * values and a shared-medium configuration that deviates is refused. The
     * escape hatch exists for exactly this -- research that needs other
     * numbers says so, rather than the profile check being weakened until the
     * project's own tests fit through it.
     */
    ca.parameters = MCL_RDV_PARAMETERS_EXPERIMENTAL;
    ca.announce_interval_ms = 100u;
    /*
     * The solicitation window has to be compressed with everything else.
     *
     * A node now announces ONCE and then owns the round until its solicitation
     * times out, so at the profile's 6000 ms this twenty-second run produces
     * four emissions rather than two hundred -- and the deadline arithmetic
     * the case exists to test would have been measured about four samples.
     * Under EXPERIMENTAL parameters a stated response timeout scales the
     * solicitation window too, which is the one place that coupling is
     * intended: see solicit_timeout() in rendezvous.c.
     */
    ca.response_timeout_ms = 100u;
    ca.backoff_slots = 1u;           /* isolate the deadline from the backoff */
    check(mcl_rdv_init(&a, &ca, &pa, &na) == MCL_RDV_OK,
          "an EXPERIMENTAL configuration is accepted with its own timings");
    check(mcl_rdv_start(&a) == MCL_RDV_OK, "and starts");
    g_room = &room; g_nodes[0] = &a; g_node_count = 1u;

    for (i = 0u; i < 400u; ++i) {
        (void)mcl_rdv_poll(&a, &ev);
        emissions += room.count;
        room.count = 0u;
        room.now += 50u;             /* walks straight through the wrap */
    }
    check(emissions >= 5u,
          "deadlines still fire across the 2^32 millisecond wrap");
}

/* The backoff is slotted, bounded, and actually varies. */
static void test_backoff_slotting(void)
{
    mcl_rdv_t r;
    mcl_rdv_config_t cfg;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    room_t room;
    peer_ctx_t xa;
    unsigned i;
    unsigned distinct = 0u;
    /*
     * uint32_t, NOT uint16_t.
     *
     * mcl_rdv_reply_delay_ms() was widened because slots x width is computed
     * in 32 bits, and any configuration whose product exceeds 65535 ms used to
     * wrap silently to a short delay -- worst for exactly the large slot counts
     * someone reaches for after seeing collisions. This array was left narrow,
     * so the test that checks the backoff was itself truncating it, and would
     * have gone on passing while asserting nothing about the case the widening
     * was for. -Wconversion in the local gates found it; the ordinary build did
     * not.
     */
    uint32_t seen[16];
    int in_range = 1, aligned = 1;

    reset_harness();
    printf("[rendezvous] slotted backoff\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 1u, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&cfg, 1000u, MCL_CONTACT_ROLE_INITIATOR);
    cfg.backoff_slots = 16u;
    cfg.backoff_slot_ms = 250u;
    (void)mcl_rdv_init(&r, &cfg, &pa, &na);

    for (i = 0u; i < 16u; ++i) {
        seen[i] = mcl_rdv_reply_delay_ms(&r);
        if (seen[i] >= 16u * 250u) in_range = 0;
        if ((seen[i] % 250u) != 0u) aligned = 0;
    }
    for (i = 0u; i < 16u; ++i) {
        unsigned j;
        int dup = 0;
        for (j = 0u; j < i; ++j) if (seen[j] == seen[i]) dup = 1;
        if (!dup) distinct++;
    }
    check(in_range, "a backoff never exceeds the slot window");
    check(aligned, "and lands on a slot boundary rather than anywhere in it");
    /*
     * Slots, not a continuous delay. Deferral does the separating; the slot
     * only has to be wide enough that a machine drawing a later one can SEE an
     * earlier transmission, which for a 200 ms AP preamble it is.
     */
    check(distinct >= 4u, "successive draws vary across the slot space");

    /*
     * THE WIDENING, EXERCISED.
     *
     * Nothing reached past 65535 ms above, because the profile's own window is
     * 4000 ms -- so the 16-bit truncation that used to live here could not be
     * seen by any case that used profile values. A configuration whose slot
     * space genuinely exceeds a 16-bit millisecond count is the only thing
     * that shows it, and such a configuration is by definition not the
     * profile's: it is declared EXPERIMENTAL, which is what that mode is for.
     */
    {
        mcl_rdv_t wide;
        mcl_rdv_config_t wcfg;
        mcl_node_t wnode;
        uint32_t largest = 0u;
        unsigned k;

        base_config(&wcfg, 2000u, MCL_CONTACT_ROLE_INITIATOR);
        wcfg.parameters = MCL_RDV_PARAMETERS_EXPERIMENTAL;
        wcfg.backoff_slots = 255u;
        wcfg.backoff_slot_ms = 1000u;      /* up to 254 000 ms */
        base_node(&wnode, 2000u, MCL_CONTACT_ROLE_INITIATOR, &xa);
        check(mcl_rdv_init(&wide, &wcfg, &pa, &wnode) == MCL_RDV_OK,
              "a slot space wider than 16 bits is accepted as EXPERIMENTAL");
        for (k = 0u; k < 32u; ++k) {
            uint32_t d = mcl_rdv_reply_delay_ms(&wide);
            if (d > largest) largest = d;
            if (d > 254u * 1000u) largest = 0xFFFFFFFFu;   /* out of range */
        }
        check(largest > 65535u && largest <= 254u * 1000u,
              "a backoff beyond 65535 ms is returned whole, not truncated");
    }
}


/*
 * source_ref = 0 IS LEGAL, AND IT USED TO MANUFACTURE AN ILLEGAL SESSION.
 *
 * The acceptor copied source_ref into TRANSPORT_ACCEPT.session_ref. Zero is
 * the reserved "no session" value, so a node configured with source_ref 0 --
 * which nothing forbids -- offered a session of zero, mcl_contact_agree
 * refused it, and the coordinator discarded the refusal and carried on.
 */
static void test_zero_source_ref(void)
{
    room_t room;
    peer_ctx_t ca_ctx, cb_ctx;
    mcl_rdv_t a, b;
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    mcl_rdv_event_t ev[MAX_NODES];
    unsigned i;
    int migrated = 0;
    uint32_t seen_session = 0u;

    reset_harness();
    printf("[rendezvous] source_ref zero still yields a legal session\n");
    reset_harness();
    room_init(&room);
    ca_ctx.room = &room; ca_ctx.index = 0u;
    cb_ctx.room = &room; cb_ctx.index = 1u;
    base_platform(&pa, &ca_ctx);
    base_platform(&pb, &cb_ctx);
    base_node(&na, 0u, MCL_CONTACT_ROLE_INITIATOR, &ca_ctx);
    base_node(&nb, 0u, MCL_CONTACT_ROLE_RESPONDER, &cb_ctx);

    /* BOTH sides at zero: nothing in MCL says a source_ref must be non-zero,
       and two builders that both left it unset is the realistic case. */
    base_config(&ca, 0u, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0u, MCL_CONTACT_ROLE_RESPONDER);
    ca.bearer_endpoint_token[0] = 0xAAAA0001u;
    cb.bearer_endpoint_token[0] = 0xBBBB0001u;

    check(mcl_rdv_init(&a, &ca, &pa, &na) == MCL_RDV_OK, "A initialises at source_ref 0");
    check(mcl_rdv_init(&b, &cb, &pb, &nb) == MCL_RDV_OK, "B initialises at source_ref 0");
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    for (i = 0u; i < 4000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].session_ref != 0u) seen_session = ev[0].session_ref;
        if (ev[1].session_ref != 0u) seen_session = ev[1].session_ref;
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
    }
    check(seen_session != 0u, "the session reference is non-zero");
    check(seen_session != ca.source_ref,
          "the session reference is not a copy of source_ref");
    check(migrated, "a contact between two zero-source_ref peers still migrates");
}

/*
 * EACH HANDOFF CONTROL, DESTROYED ONCE.
 *
 * The Link layer's controls are idempotent and that was demonstrated
 * independently. The coordinator sent each exactly once and so inherited none
 * of it. Every one of these four cases hung before retransmission existed.
 */
static void run_with_drop(int op, const char *what)
{
    room_t room;
    peer_ctx_t ca_ctx, cb_ctx;
    mcl_rdv_t a, b;
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    mcl_rdv_event_t ev[MAX_NODES];
    unsigned i;
    int migrated = 0;

    reset_harness();
    room_init(&room);
    room.drop_op = op;
    ca_ctx.room = &room; ca_ctx.index = 0u;
    cb_ctx.room = &room; cb_ctx.index = 1u;
    base_platform(&pa, &ca_ctx);
    base_platform(&pb, &cb_ctx);
    base_node(&na, 0x11111111u, MCL_CONTACT_ROLE_INITIATOR, &ca_ctx);
    base_node(&nb, 0x22222222u, MCL_CONTACT_ROLE_RESPONDER, &cb_ctx);
    base_config(&ca, 0x11111111u, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x22222222u, MCL_CONTACT_ROLE_RESPONDER);
    ca.bearer_endpoint_token[0] = 0xAAAA0001u;
    cb.bearer_endpoint_token[0] = 0xBBBB0001u;
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    for (i = 0u; i < 8000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
    }
    check(room.dropped == 1u, "the control was actually destroyed");
    check(migrated, what);
}

static void test_lost_handoff_controls(void)
{
    reset_harness();
    printf("[rendezvous] a lost handoff control is retransmitted\n");
    run_with_drop(MCL_HANDOFF_OP_PATH_CHALLENGE,
                  "a lost PATH_CHALLENGE still migrates");
    run_with_drop(MCL_HANDOFF_OP_PATH_RESPONSE,
                  "a lost PATH_RESPONSE still migrates");
    run_with_drop(MCL_HANDOFF_OP_COMMIT,
                  "a lost COMMIT still migrates");
    run_with_drop(MCL_HANDOFF_OP_CONFIRM,
                  "a lost CONFIRM still migrates");
}

/*
 * POLICY REFUSAL IS CONFORMANT.
 *
 * Reception is not identity, is not authority and is not obligation. A node
 * that hears a stranger, proves it is reachable and then declines to admit it
 * has behaved correctly. The coordinator never asked: POLICY_REQUIRED was
 * declared in the public API and no build emitted it.
 */
static void test_policy_refusal(void)
{
    room_t room;
    peer_ctx_t ca_ctx, cb_ctx;
    mcl_rdv_t a, b;
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    mcl_rdv_event_t ev[MAX_NODES];
    unsigned i;
    int migrated = 0, lost = 0;

    reset_harness();
    printf("[rendezvous] local policy is asked, and a refusal is honoured\n");
    reset_harness();
    g_auto_admit = 0;                 /* refuse instead of admitting */
    room_init(&room);
    ca_ctx.room = &room; ca_ctx.index = 0u;
    cb_ctx.room = &room; cb_ctx.index = 1u;
    base_platform(&pa, &ca_ctx);
    base_platform(&pb, &cb_ctx);
    base_node(&na, 0x33333333u, MCL_CONTACT_ROLE_INITIATOR, &ca_ctx);
    base_node(&nb, 0x44444444u, MCL_CONTACT_ROLE_RESPONDER, &cb_ctx);
    base_config(&ca, 0x33333333u, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x44444444u, MCL_CONTACT_ROLE_RESPONDER);
    ca.bearer_endpoint_token[0] = 0xAAAA0001u;
    cb.bearer_endpoint_token[0] = 0xBBBB0001u;
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    g_room = &room; g_nodes[0] = &a; g_nodes[1] = &b; g_node_count = 2u;
    for (i = 0u; i < 4000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_LOST ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_LOST) lost = 1;
    }
    check(g_policy_asked > 0u, "local policy was asked before any commitment");
    check(!migrated, "a refused peer does not migrate the contact");
    check(lost, "and the refusal is reported rather than silent");
    reset_harness();
}

/*
 * THE SAME SLOT MUST COLLIDE.
 *
 * If forcing two machines into one 250 ms slot does not produce a collision,
 * the simulator is being kind and every contention result it has ever
 * produced is worth less than it looks.
 */
static void test_same_slot_collides(void)
{
    room_t room;
    peer_ctx_t ctx[3];
    mcl_rdv_t r[3];
    mcl_rdv_config_t cfg[3];
    mcl_rdv_platform_t plat[3];
    mcl_node_t node[3];
    mcl_rdv_event_t ev[MAX_NODES];
    unsigned i;

    reset_harness();
    printf("[rendezvous] a forced identical slot produces a real collision\n");
    room_init(&room);
    room.force_same_slot = 6;   /* two rounds for each of the three nodes */

    for (i = 0u; i < 3u; ++i) {
        ctx[i].room = &room;
        ctx[i].index = (uint8_t)i;
        base_platform(&plat[i], &ctx[i]);
        base_node(&node[i], 0x50000001u + i,
                  i == 0u ? MCL_CONTACT_ROLE_INITIATOR
                          : MCL_CONTACT_ROLE_RESPONDER, &ctx[i]);
        base_config(&cfg[i], 0x50000001u + i,
                    i == 0u ? MCL_CONTACT_ROLE_INITIATOR
                            : MCL_CONTACT_ROLE_RESPONDER);
        cfg[i].bearer_endpoint_token[0] = 0xC0DE0001u + i;
        (void)mcl_rdv_init(&r[i], &cfg[i], &plat[i], &node[i]);
        (void)mcl_rdv_start(&r[i]);
        g_nodes[i] = &r[i];
    }
    g_room = &room;
    g_node_count = 3u;

    for (i = 0u; i < 3000u; ++i) {
        tick(&room, ev, 10u);
    }
    check(room.collisions > 0u,
          "three machines in one slot actually collide");
    check(room.delivered > 0u,
          "and the medium recovers once the slots diverge again");
}

/* ------------------------------------------- the solicitation epoch, tested
 *
 * The three-machine failure was found by a statistical run and could only be
 * confirmed by one, which is the wrong instrument for a rule about roles: a
 * run that happens to converge says nothing about whether it had to. The cases
 * below drive the coordinator directly instead, so each election property is
 * a fact about one machine's reaction to one frame.
 */

/* Encode a Tier-0 object and hand it to one coordinator, exactly as the room
   would. Used where a test must construct traffic no correct node emits --
   two contenders carrying one migration_ref, for instance. */
static void deliver_object(mcl_rdv_t *r, uint8_t transport_id,
                           const mcl_wire_tier0_t *object)
{
    uint8_t buf[MAX_FRAME_SIZE];
    size_t written = 0u;

    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, object,
                                       buf, sizeof(buf), &written)
        != MCL_WIRE_OK) {
        check(0, "the fixture encodes its own object");
        return;
    }
    (void)mcl_rdv_deliver(r, transport_id, buf, written);
}

static void make_presence(mcl_wire_tier0_t *o, uint32_t source_ref)
{
    memset(o, 0, sizeof(*o));
    o->kind = MCL_WIRE_KIND_PRESENCE;
    o->priority = 1u;
    o->source_ref = source_ref;
    o->body.presence.capability_tag = 0x000004u;
    o->body.presence.ttl = 60u;
}

static void make_offer(mcl_wire_tier0_t *o, uint32_t source_ref,
                       uint32_t migration_ref, uint8_t transport_id,
                       uint8_t profile_id, uint32_t endpoint_token)
{
    memset(o, 0, sizeof(*o));
    o->kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
    o->priority = 1u;
    o->source_ref = source_ref;
    o->body.transport_offer.migration_ref = migration_ref;
    o->body.transport_offer.transport_id = transport_id;
    o->body.transport_offer.profile_id = profile_id;
    o->body.transport_offer.endpoint_token = endpoint_token;
    o->body.transport_offer.validity = 60u;
}

static void make_accept(mcl_wire_tier0_t *o, uint32_t source_ref,
                        uint32_t migration_ref, uint8_t transport_id,
                        uint8_t profile_id, uint32_t session_ref)
{
    memset(o, 0, sizeof(*o));
    o->kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
    o->priority = 1u;
    o->source_ref = source_ref;
    o->body.transport_accept.migration_ref = migration_ref;
    o->body.transport_accept.transport_id = transport_id;
    o->body.transport_accept.profile_id = profile_id;
    o->body.transport_accept.session_ref = session_ref;
}

/* One coordinator with everything it needs, so a case that is about the
   protocol is not three quarters declarations. */
typedef struct {
    mcl_rdv_t rdv;
    mcl_rdv_config_t cfg;
    mcl_rdv_platform_t plat;
    mcl_node_t node;
    peer_ctx_t ctx;
} unit_t;

static void unit_start(unit_t *u, room_t *room, uint8_t index, uint32_t ref)
{
    u->ctx.room = room;
    u->ctx.index = index;
    base_platform(&u->plat, &u->ctx);
    base_node(&u->node, ref, (index == 0u) ? MCL_CONTACT_ROLE_INITIATOR
                                           : MCL_CONTACT_ROLE_RESPONDER,
              &u->ctx);
    base_config(&u->cfg, ref, (index == 0u) ? MCL_CONTACT_ROLE_INITIATOR
                                            : MCL_CONTACT_ROLE_RESPONDER);
    u->cfg.bearer_endpoint_token[0] = 0xE0E00000u + index;
    check(mcl_rdv_init(&u->rdv, &u->cfg, &u->plat, &u->node) == MCL_RDV_OK,
          "fixture node initialises");
    check(mcl_rdv_start(&u->rdv) == MCL_RDV_OK, "fixture node starts");
    g_nodes[index] = &u->rdv;
}

/*
 * Let this node's own emission finish before anything is injected.
 *
 * room_self_transmitting() reports the speaker busy for the whole airtime of a
 * frame -- 600 ms for a PRESENCE, 787 ms for an OFFER -- and a coordinator
 * correctly discards everything that arrives while its own emitter is driving
 * the medium. So a fixture that hands a node a frame the instant after it
 * transmits is exercising the self-echo rule and nothing else, and every
 * assertion after it passes or fails for the wrong reason.
 */
static void settle(unit_t *u, room_t *room, uint32_t ms)
{
    mcl_rdv_event_t ev;
    uint32_t t;

    for (t = 0u; t < ms; t += 10u) {
        (void)mcl_rdv_poll(&u->rdv, &ev);
        room_advance(room, 10u);
    }
}

/* Poll one node until it owns a round, or give up. Nothing here depends on
   how many polls that takes, only that it happens. */
static int drive_to_soliciting(unit_t *u, room_t *room)
{
    mcl_rdv_event_t ev;
    unsigned i;

    for (i = 0u; i < 4000u; ++i) {
        (void)mcl_rdv_poll(&u->rdv, &ev);
        if (u->rdv.state == MCL_RDV_STATE_SOLICITING) {
            return 1;
        }
        room->now += 10u;
    }
    return 0;
}

/*
 * THE INVARIANT THAT NAMES THE DEFECT, READ OFF THE WIRE.
 *
 * A machine that has emitted this round's PRESENCE owns the round and must not
 * answer somebody else's. Stated over the transcript: an OFFER from node X
 * within X's own solicitation window of X's own PRESENCE is X holding the
 * solicitor and responder roles at the same instant -- exactly the condition
 * that had three machines cycling forever with every one of them an acceptor.
 *
 * THE WINDOW IS WHAT MAKES IT BITE, and the first version of this function did
 * not have one. It asked only whether an OFFER's sender was also the sender of
 * the most recent PRESENCE, which a mutation that let a SOLICITING node take
 * the responder role walked straight past: the mutated node became a responder
 * to the OTHER machine's PRESENCE, so the most recent announcement was never
 * its own. A check that survives the defect it was written for is not
 * evidence, and this one is now run against that mutation before it is
 * trusted.
 *
 * It cannot fire on a legitimate exchange. A node may only offer after
 * returning to ANNOUNCING, which happens when its solicitation times out -- so
 * a legitimate offer is at least one full solicitation window after that
 * node's own last announcement, and the maximum backoff (3750 ms) is shorter
 * than that window (6000 ms), so there is no overlap to argue about.
 */
static unsigned self_answered_offers(const room_t *room)
{
    unsigned i, j;
    unsigned bad = 0u;

    for (i = 0u; i < room->log_count; ++i) {
        const logged_t *offer = &room->log[i];
        if (offer->kind != (uint8_t)MCL_WIRE_KIND_TRANSPORT_OFFER) continue;
        for (j = i; j-- > 0u; ) {
            const logged_t *e = &room->log[j];
            if (e->from != offer->from) continue;
            if (e->kind != (uint8_t)MCL_WIRE_KIND_PRESENCE) continue;
            if (offer->at_ms - e->at_ms < MCL_RDV_AP1_SOLICIT_TIMEOUT_MS) {
                bad++;
            }
            break;      /* this node's most recent announcement */
        }
    }
    return bad;
}

static unsigned count_kind(const room_t *room, mcl_wire_kind_t kind)
{
    unsigned i, n = 0u;
    for (i = 0u; i < room->log_count; ++i) {
        if (room->log[i].kind == (uint8_t)kind) n++;
    }
    return n;
}

/*
 * ONE SOLICITOR, TWO CONTENDERS: THE FIRST VALID OFFER IS SELECTED AND THE
 * SELECTION IS THEN FROZEN.
 *
 * Both contenders offer a bearer this deployment mandates, so a coordinator
 * that simply matched the bearer list would take whichever arrived last -- or
 * both, which is what "36 agreements, 0 migrations" looked like from outside.
 */
static void test_first_offer_is_selected(void)
{
    room_t room;
    unit_t s;
    mcl_wire_tier0_t offer;
    mcl_rdv_event_t ev;
    unsigned i;

    reset_harness();
    printf("[rendezvous] the solicitor selects the first valid offer\n");
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&s, &room, 0u, 0x70000001u);
    check(drive_to_soliciting(&s, &room),
          "a machine that announced owns the round");
    settle(&s, &room, 1000u);

    /*
     * Zero names no transaction, and a bootstrap bearer is public: anything at
     * all arrives on it. Adopting a zero reference would have this node accept
     * a transaction that the offering peer cannot recognise as its own, since
     * its ACCEPT match requires a non-zero outstanding reference.
     */
    make_offer(&offer, 0x0D0D0D0Du, 0u, 3u, 1u, 0xD0000001u);
    deliver_object(&s.rdv, 1u, &offer);
    check(s.rdv.state == MCL_RDV_STATE_SOLICITING,
          "an offer naming transaction zero is refused");
    check(!s.rdv.peer_seen, "and binds no contender");

    make_offer(&offer, 0x0B0B0B0Bu, 0x11111111u, 3u, 1u, 0xB0000001u);
    deliver_object(&s.rdv, 1u, &offer);
    check(s.rdv.peer_ref == 0x0B0B0B0Bu, "the first contender is bound");
    check(s.rdv.migration_ref == 0x11111111u,
          "and its transaction reference is adopted");
    check(s.rdv.state == MCL_RDV_STATE_ACCEPTING,
          "the acceptance waits for a contention slot, not the wire");

    /* A second contender, a different transaction, the same mandated bearer. */
    make_offer(&offer, 0x0C0C0C0Cu, 0x22222222u, 3u, 1u, 0xC0000001u);
    deliver_object(&s.rdv, 1u, &offer);
    check(s.rdv.peer_ref == 0x0B0B0B0Bu,
          "a later contender does not displace the selection");
    check(s.rdv.migration_ref == 0x11111111u,
          "and the transaction reference is unchanged");

    for (i = 0u; i < 2000u; ++i) {
        (void)mcl_rdv_poll(&s.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_ACCEPT) >= 1u,
          "the acceptance is eventually transmitted");
    for (i = 0u; i < room.log_count; ++i) {
        if (room.log[i].kind == (uint8_t)MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
            check(room.log[i].migration_ref == 0x11111111u,
                  "every acceptance names the selected transaction");
        }
    }
}

/*
 * A RESPONDER DOES NOT ANSWER ANOTHER RESPONDER.
 *
 * B and C both hear A. Under the old rule C's offer reached B and B accepted
 * it on a bearer-list match alone, so three machines could produce a B-C
 * agreement neither was attempting. The peer-scoping patch narrowed that; the
 * solicitation epoch removes the position entirely, and this asserts the
 * position rather than the patch.
 */
static void test_responders_ignore_each_other(void)
{
    room_t room;
    unit_t b;
    mcl_wire_tier0_t object;
    mcl_rdv_event_t ev;
    unsigned i;
    unsigned accepts_before;

    reset_harness();
    printf("[rendezvous] a responder never answers a fellow responder\n");
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&b, &room, 0u, 0x0B0B0B0Bu);

    /* A announces; B has not yet announced, so B becomes A's responder. */
    make_presence(&object, 0x0A0A0A0Au);
    deliver_object(&b.rdv, 1u, &object);
    check(b.rdv.state == MCL_RDV_STATE_HEARD,
          "a machine that had not announced becomes a responder");
    check(b.rdv.peer_ref == 0x0A0A0A0Au, "bound to the solicitor");

    /* C offers, in HEARD. */
    accepts_before = count_kind(&room, MCL_WIRE_KIND_TRANSPORT_ACCEPT);
    make_offer(&object, 0x0C0C0C0Cu, 0x33333333u, 3u, 1u, 0xC0000001u);
    deliver_object(&b.rdv, 1u, &object);
    check(b.rdv.peer_ref == 0x0A0A0A0Au,
          "HEARD: a third machine's offer does not rebind the peer");
    check(b.rdv.session_ref == 0u, "and no session is manufactured for it");
    check(b.rdv.has_pending_accept == 0u, "and no acceptance is prepared");

    /* Now drive B into OFFERING and try again. */
    for (i = 0u; i < 4000u && b.rdv.state != MCL_RDV_STATE_OFFERING; ++i) {
        (void)mcl_rdv_poll(&b.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(b.rdv.state == MCL_RDV_STATE_OFFERING, "the responder offers");
    settle(&b, &room, 1000u);
    make_offer(&object, 0x0C0C0C0Cu, 0x44444444u, 2u, 1u, 0xC0000002u);
    deliver_object(&b.rdv, 1u, &object);
    check(b.rdv.state == MCL_RDV_STATE_OFFERING,
          "OFFERING: a third machine's offer changes nothing");
    check(b.rdv.peer_ref == 0x0A0A0A0Au, "the peer is still the solicitor");
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_ACCEPT) == accepts_before,
          "and a responder emits no acceptance at all");
}

/*
 * THE LOSER OF AN ELECTION DOES NOT TAKE THE WINNER'S TRANSACTION.
 *
 * An acceptance is audible to every contender, and the only thing separating
 * the chosen responder from the others is the migration_ref it echoes.
 */
static void test_unselected_responder_stands_down(void)
{
    room_t room;
    unit_t b;
    mcl_wire_tier0_t object;
    mcl_rdv_event_t ev;
    unsigned i;
    uint32_t mine;

    reset_harness();
    printf("[rendezvous] an acceptance for another transaction is not ours\n");
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&b, &room, 0u, 0x0B0B0B0Bu);

    make_presence(&object, 0x0A0A0A0Au);
    deliver_object(&b.rdv, 1u, &object);
    for (i = 0u; i < 4000u && b.rdv.state != MCL_RDV_STATE_OFFERING; ++i) {
        (void)mcl_rdv_poll(&b.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(b.rdv.state == MCL_RDV_STATE_OFFERING, "the responder offers");
    mine = b.rdv.migration_ref;
    check(mine != 0u, "and holds a transaction reference");
    settle(&b, &room, 1000u);

    /* The solicitor chose somebody else. */
    make_accept(&object, 0x0A0A0A0Au, mine ^ 0x5A5A5A5Au,
                b.rdv.offered_transport, b.rdv.offered_profile, 0x99999999u);
    deliver_object(&b.rdv, 1u, &object);

    check(!b.rdv.is_controller, "the loser does not become a controller");
    check(b.rdv.session_ref == 0u,
          "and does not adopt the winner's session reference");
    check(b.rdv.state != MCL_RDV_STATE_AGREED &&
          b.rdv.state != MCL_RDV_STATE_VALIDATING &&
          b.rdv.state != MCL_RDV_STATE_COMMITTING &&
          b.rdv.state != MCL_RDV_STATE_MIGRATED,
          "and reaches no part of a migration it was not selected for");
    /*
     * It stands down rather than waiting out its offer retries. That is a
     * suppression and not a correctness requirement -- a contender that never
     * heard the acceptance still converges through its retries -- but three
     * unnecessary offers are 787 ms of air each, taken from the pair that did
     * agree.
     */
    check(b.rdv.state == MCL_RDV_STATE_ANNOUNCING,
          "it returns to contention instead of holding the medium");
    check(b.rdv.bearer_index == 0u,
          "losing an election is not evidence about a bearer");
}

/*
 * TWO CONTENDERS CARRYING ONE REFERENCE: ABORT, DO NOT GUESS.
 *
 * migration_ref selects the responder, so if two of them present the same one
 * the solicitor cannot name the transaction it is about to accept. Choosing
 * either tells both machines they were chosen. This is reachable: two builders
 * may legally hold the same source_ref, and a reference derived from it
 * collides on the first transaction -- which is why the shared-medium path
 * draws the reference from platform.random instead.
 */
static void test_migration_ref_collision_aborts(void)
{
    room_t room;
    unit_t s;
    mcl_wire_tier0_t offer;

    reset_harness();
    printf("[rendezvous] two contenders with one migration_ref abort the round\n");
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&s, &room, 0u, 0x70000002u);
    check(drive_to_soliciting(&s, &room), "the machine owns a round");
    settle(&s, &room, 1000u);

    make_offer(&offer, 0x0B0B0B0Bu, 0x77777777u, 3u, 1u, 0xB0000001u);
    deliver_object(&s.rdv, 1u, &offer);
    check(s.rdv.state == MCL_RDV_STATE_ACCEPTING, "a contender is selected");

    /* A different machine, the same reference. */
    make_offer(&offer, 0x0C0C0C0Cu, 0x77777777u, 3u, 1u, 0xC0000001u);
    deliver_object(&s.rdv, 1u, &offer);

    check(s.rdv.state == MCL_RDV_STATE_ANNOUNCING,
          "the ambiguous round is abandoned, not resolved by guessing");
    check(s.rdv.migration_ref == 0u, "the transaction reference is dropped");
    check(s.rdv.session_ref == 0u, "no session survives an abandoned round");
    check(s.rdv.has_pending_accept == 0u, "and no acceptance is left armed");
    check(!s.rdv.peer_seen, "and no peer stays bound from it");
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_ACCEPT) == 0u,
          "nothing was ever said about the ambiguous transaction");
}

/*
 * A LOST ACCEPTANCE IS RECOVERED BY REPEATING THE SAME ONE.
 *
 * The acceptance is the only frame in the exchange that neither end
 * retransmits on a timer: the responder retries its OFFER, and the solicitor
 * re-arms the acceptance it already built when it hears that retry. So the
 * property is not merely that a second acceptance goes out, it is that the
 * second one is IDENTICAL -- a fresh session or a fresh reference would be a
 * second transaction racing the first, and both ends would be right about
 * different things.
 */
static void test_lost_acceptance_recovers(void)
{
    room_t room;
    unit_t a, b;
    mcl_rdv_event_t ev[2];
    unsigned i;
    int migrated = 0;
    unsigned accepts = 0u, offers = 0u;
    uint32_t accept_ref = 0u, accept_session = 0u, offer_ref = 0u;
    int accept_consistent = 1, offer_consistent = 1;

    reset_harness();
    printf("[rendezvous] a destroyed acceptance is repeated, not reinvented\n");
    room_init(&room);
    room.drop_accepts = 1;
    g_room = &room;
    g_node_count = 2u;
    unit_start(&a, &room, 0u, 0x81818181u);
    unit_start(&b, &room, 1u, 0x82828282u);

    for (i = 0u; i < 12000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
    }

    for (i = 0u; i < room.log_count; ++i) {
        const logged_t *e = &room.log[i];
        if (e->kind == (uint8_t)MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
            if (accepts == 0u) {
                accept_ref = e->migration_ref;
                accept_session = e->session_ref;
            } else if (e->migration_ref != accept_ref ||
                       e->session_ref != accept_session) {
                accept_consistent = 0;
            }
            accepts++;
        } else if (e->kind == (uint8_t)MCL_WIRE_KIND_TRANSPORT_OFFER) {
            if (offers == 0u) {
                offer_ref = e->migration_ref;
            } else if (e->migration_ref != offer_ref) {
                offer_consistent = 0;
            }
            offers++;
        }
    }
    printf("       (%u offers, %u acceptances, %u destroyed)\n",
           offers, accepts, room.accepts_dropped);
    check(room.accepts_dropped == 1u, "the acceptance was actually destroyed");
    check(accepts >= 2u, "the acceptance is sent again");
    check(accept_consistent,
          "and it is the SAME acceptance -- same transaction, same session");
    check(offers >= 2u, "the responder retries its offer");
    check(offer_consistent,
          "and retries the SAME offer rather than opening a new transaction");
    check(migrated, "so the contact still migrates through a lost acceptance");
    check(a.rdv.session_ref == b.rdv.session_ref && a.rdv.session_ref != 0u,
          "with one session reference, agreed once");
}

/*
 * THE CYCLE, CONSTRUCTED ON PURPOSE.
 *
 * A believes B is its peer, B believes C, C believes A. This was not a
 * hypothesis: it is what three machines settled into, and it never broke on
 * its own -- 600 simulated seconds produced 36 bearer agreements, 0
 * validations and 0 migrations, with every node an acceptor.
 *
 * Priming it by hand is the honest test. Each node is handed a PRESENCE from
 * the node ahead of it in the cycle before any of them announces, so all three
 * start as responders in three different rounds. Nothing can be consumed --
 * responders do not accept offers -- so the cycle must dissolve through the
 * ordinary bearer-exhaustion path and then re-form as a real pair.
 */
static void test_forced_cycle_dissolves(void)
{
    room_t room;
    unit_t u[3];
    mcl_rdv_event_t ev[MAX_NODES];
    mcl_wire_tier0_t object;
    unsigned i, k;
    unsigned migrated_nodes = 0u, validated = 0u;
    uint32_t session[3];
    static const uint32_t refs[3] = { 0x0A0A0A0Au, 0x0B0B0B0Bu, 0x0C0C0C0Cu };

    reset_harness();
    printf("[rendezvous] a forced A->B->C->A cycle does not persist\n");
    g_auto_restart = 1;
    room_init(&room);
    g_room = &room;
    g_node_count = 3u;
    for (i = 0u; i < 3u; ++i) {
        unit_start(&u[i], &room, (uint8_t)i, refs[i]);
    }
    /* Prime the cycle: each node hears the one ahead of it, and none of them
       has announced yet, so each takes the responder role for a different
       round. */
    for (i = 0u; i < 3u; ++i) {
        make_presence(&object, refs[(i + 1u) % 3u]);
        deliver_object(&u[i].rdv, 1u, &object);
        check(u[i].rdv.state == MCL_RDV_STATE_HEARD,
              "each node starts the run as somebody's responder");
    }
    check(u[0].rdv.peer_ref == refs[1] && u[1].rdv.peer_ref == refs[2] &&
          u[2].rdv.peer_ref == refs[0], "and the cycle is really a cycle");

    for (i = 0u; i < 30000u; ++i) {
        tick(&room, ev, 10u);
        for (k = 0u; k < 3u; ++k) {
            if (ev[k].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated_nodes++;
            if (ev[k].kind == MCL_RDV_EVENT_CANDIDATE_VALIDATED) validated++;
        }
    }
    for (i = 0u; i < 3u; ++i) session[i] = u[i].rdv.session_ref;
    printf("       (states %u %u %u; sessions %08lX %08lX %08lX)\n",
           (unsigned)u[0].rdv.state, (unsigned)u[1].rdv.state,
           (unsigned)u[2].rdv.state,
           (unsigned long)session[0], (unsigned long)session[1],
           (unsigned long)session[2]);

    check(!(u[0].rdv.peer_ref == refs[1] && u[1].rdv.peer_ref == refs[2] &&
            u[2].rdv.peer_ref == refs[0]),
          "the cycle does not survive the run");
    check(validated > 0u, "a candidate path is validated out of the cycle");
    check(migrated_nodes >= 2u, "and a PAIR migrates, not one hopeful end");
    /* Exactly one pair shares a session. Three matching, or none, would both
       mean the election did not elect. */
    {
        unsigned pairs = 0u;
        for (i = 0u; i < 3u; ++i) {
            for (k = i + 1u; k < 3u; ++k) {
                if (session[i] != 0u && session[i] == session[k]) pairs++;
            }
        }
        check(pairs == 1u, "exactly one pair holds one session reference");
    }
    check(self_answered_offers(&room) == 0u,
          "no machine ever answers its own solicitation");
    reset_harness();
}

/*
 * TWO RESPONDERS SHARING A source_ref MUST NOT SHARE A migration_ref.
 *
 * This is the whole argument for drawing the reference from platform.random on
 * a shared medium, and nothing else in the suite can see it. The two-node case
 * cannot: only the responder generates a reference, so with one responder
 * there is nothing for it to collide with, and a fully deterministic generator
 * passes that case unharmed -- it did, when this was checked by mutation.
 *
 * Here both machines are responders to the same solicitation and hold the same
 * source_ref, which wire.h permits. Under `source_ref ^ (counter * k)` they
 * compute the same reference on their first transaction, and one acceptance
 * then names both of them.
 */
static void test_responder_refs_differ(void)
{
    room_t room;
    unit_t r[2];
    mcl_wire_tier0_t object;
    mcl_rdv_event_t ev;
    unsigned i, k;

    reset_harness();
    printf("[rendezvous] two responders sharing a source_ref\n");
    room_init(&room);
    g_room = &room;
    g_node_count = 2u;
    /* The SAME source_ref on both, which is a legal configuration. */
    unit_start(&r[0], &room, 0u, 0x5A5A5A5Au);
    unit_start(&r[1], &room, 1u, 0x5A5A5A5Au);

    for (k = 0u; k < 2u; ++k) {
        make_presence(&object, 0x0A0A0A0Au);
        deliver_object(&r[k].rdv, 1u, &object);
        check(r[k].rdv.state == MCL_RDV_STATE_HEARD,
              "both machines answer the same solicitation");
        for (i = 0u; i < 4000u &&
                     r[k].rdv.state != MCL_RDV_STATE_OFFERING; ++i) {
            (void)mcl_rdv_poll(&r[k].rdv, &ev);
            room_advance(&room, 10u);
        }
        check(r[k].rdv.state == MCL_RDV_STATE_OFFERING, "and both offer");
    }
    printf("       (migration refs %08lX %08lX)\n",
           (unsigned long)r[0].rdv.migration_ref,
           (unsigned long)r[1].rdv.migration_ref);
    check(r[0].rdv.migration_ref != 0u && r[1].rdv.migration_ref != 0u,
          "each contender holds a transaction reference");
    check(r[0].rdv.migration_ref != r[1].rdv.migration_ref,
          "and the two references differ despite one shared source_ref");
}

/*
 * SAME SLOT, SAME INSTANT: THE ANNOUNCEMENTS DESTROY EACH OTHER AND THE
 * MACHINES STILL CONVERGE.
 *
 * This is the case the solicitation epoch has to survive rather than avoid.
 * Two machines whose PRESENCE frames collide both end up owning a round that
 * nobody can hear, both time out, and both re-draw -- so the recovery is a
 * property of the state machine, not of the modem.
 */
static void test_presence_collision_converges(void)
{
    room_t room;
    unit_t a, b;
    mcl_rdv_event_t ev[2];
    unsigned i;
    int migrated = 0;

    reset_harness();
    printf("[rendezvous] colliding first announcements still converge\n");
    room_init(&room);
    room.force_same_slot = 4;      /* two forced rounds for each of the two */
    g_room = &room;
    g_node_count = 2u;
    unit_start(&a, &room, 0u, 0x91919191u);
    unit_start(&b, &room, 1u, 0x92929292u);

    for (i = 0u; i < 12000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
    }
    printf("       (%u delivered, %u lost to overlap)\n",
           room.delivered, room.collisions);
    check(room.collisions > 0u, "the forced slot really did collide");
    check(migrated, "and the pair still reaches migration afterwards");
    check(a.rdv.session_ref == b.rdv.session_ref && a.rdv.session_ref != 0u,
          "on one session reference");
    check(self_answered_offers(&room) == 0u,
          "with no machine answering its own solicitation");
}


/* ------------------------------------------------- the allocator boundary
 *
 * session_ref and this machine's endpoint_token were both produced INSIDE the
 * coordinator -- one from a hash of source_ref and a counter, the other read
 * straight out of a static config array. Both are adequate for a node with one
 * contact and a fixed address, and neither is a thing this layer can actually
 * know:
 *
 *   session_ref     must be distinct across the integrator's whole contact
 *                   pool, and the coordinator can only see itself
 *   endpoint_token  must select ONE transaction wherever the peer scans for
 *                   it -- BLE-ACTIVATE-1 makes it the advertisement match key,
 *                   so one static token for two concurrent activations
 *                   advertises identically for both
 *
 * So both are hooks, both optional, and the default is exactly what was there
 * before.
 */

static void test_allocator_boundary(void)
{
    room_t room;
    unit_t a, b;
    mcl_rdv_event_t ev[2];
    unsigned i;
    int migrated = 0;
    unsigned offers = 0u, accepts = 0u;
    uint32_t offer_token = 0u, accept_session = 0u;
    int token_stable = 1;

    reset_harness();
    printf("[rendezvous] the integrator allocates the session and the token\n");
    room_init(&room);
    /* Destroy the first acceptance, so the responder retransmits its offer.
       That is what makes "called once per transaction" testable rather than
       merely stated: a per-emission allocator would mint a second token. */
    room.drop_accepts = 1;
    g_room = &room;
    g_node_count = 2u;
    unit_start(&a, &room, 0u, 0xA1A1A1A1u);
    unit_start(&b, &room, 1u, 0xB1B1B1B1u);
    a.plat.allocate_session = alloc_session;
    a.plat.allocate_endpoint_token = alloc_token;
    b.plat.allocate_session = alloc_session;
    b.plat.allocate_endpoint_token = alloc_token;
    /* The platform is copied into the coordinator by init, so it has to be
       set before init -- re-init here rather than reaching into the struct. */
    (void)mcl_rdv_init(&a.rdv, &a.cfg, &a.plat, &a.node);
    (void)mcl_rdv_init(&b.rdv, &b.cfg, &b.plat, &b.node);
    (void)mcl_rdv_start(&a.rdv);
    (void)mcl_rdv_start(&b.rdv);

    for (i = 0u; i < 12000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_CONTACT_MIGRATED ||
            ev[1].kind == MCL_RDV_EVENT_CONTACT_MIGRATED) migrated = 1;
    }

    for (i = 0u; i < room.log_count; ++i) {
        const logged_t *e = &room.log[i];
        if (e->kind == (uint8_t)MCL_WIRE_KIND_TRANSPORT_OFFER) {
            if (offers == 0u) offer_token = e->endpoint_token;
            else if (e->endpoint_token != offer_token) token_stable = 0;
            offers++;
        } else if (e->kind == (uint8_t)MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
            if (accepts == 0u) accept_session = e->session_ref;
            accepts++;
        }
    }
    printf("       (%u offers, %u acceptances; token %08lX session %08lX; "
           "%u token calls, %u session calls)\n",
           offers, accepts, (unsigned long)offer_token,
           (unsigned long)accept_session, g_token_calls, g_session_calls);

    check(migrated, "a contact still migrates with both hooks supplied");
    check(g_token_calls > 0u, "the endpoint token came from the integrator");
    check(g_session_calls > 0u, "and so did the session reference");
    check(offer_token >= 0x00A00000u,
          "the token on the wire is the allocated one, not the config value");
    check(offers >= 2u, "the offer was retransmitted");
    check(token_stable,
          "and every retransmission carried the SAME token, not a new one");
    check(accept_session == a.rdv.session_ref ||
          accept_session == b.rdv.session_ref,
          "the session on the wire is the allocated one");
    check(a.rdv.session_ref == b.rdv.session_ref && a.rdv.session_ref != 0u,
          "and both ends hold it");
    check(g_token_calls <= offers,
          "the token allocator is called per transaction, not per emission");
}

/*
 * A REFUSAL IS NOT A ZERO.
 *
 * An allocator that declines is the integrator saying it cannot take this
 * contact right now. The coordinator must not substitute something of its own:
 * a manufactured session is rejected by mcl_contact_agree, and this path used
 * to discard that rejection and carry on with the two ends disagreeing about
 * whether a contact existed. A manufactured token is worse, because it is an
 * address claim the integrator never made.
 */
static void test_allocator_refusal(void)
{
    room_t room;
    unit_t s;
    mcl_wire_tier0_t offer;
    mcl_rdv_event_t ev;
    unsigned i;

    reset_harness();
    printf("[rendezvous] an allocator that declines is honoured\n");

    /* Session: the solicitor cannot mint one, so it says nothing at all. */
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&s, &room, 0u, 0xC1C1C1C1u);
    s.plat.allocate_session = alloc_session;
    (void)mcl_rdv_init(&s.rdv, &s.cfg, &s.plat, &s.node);
    (void)mcl_rdv_start(&s.rdv);
    g_session_refuses = 1;
    check(drive_to_soliciting(&s, &room), "the machine owns a round");
    settle(&s, &room, 1000u);
    make_offer(&offer, 0x0B0B0B0Bu, 0x31313131u, 3u, 1u, 0xB0000001u);
    deliver_object(&s.rdv, 1u, &offer);
    check(g_session_calls == 1u, "the allocator was asked");
    check(s.rdv.session_ref == 0u, "no session is manufactured over a refusal");
    check(s.rdv.has_pending_accept == 0u, "and no acceptance is armed");
    check(!s.rdv.peer_seen, "and no contender is left bound");
    for (i = 0u; i < 500u; ++i) {
        (void)mcl_rdv_poll(&s.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_ACCEPT) == 0u,
          "nothing was said about a contact that was refused");

    /* Token: the responder cannot mint one, so it does not offer that bearer
       -- and it does not fall back to the configured value either. */
    reset_harness();
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&s, &room, 0u, 0xC2C2C2C2u);
    s.plat.allocate_endpoint_token = alloc_token;
    (void)mcl_rdv_init(&s.rdv, &s.cfg, &s.plat, &s.node);
    (void)mcl_rdv_start(&s.rdv);
    g_token_refuses = 1;
    {
        mcl_wire_tier0_t presence;
        make_presence(&presence, 0x0A0A0A0Au);
        deliver_object(&s.rdv, 1u, &presence);
    }
    check(s.rdv.state == MCL_RDV_STATE_HEARD, "it became a responder");
    for (i = 0u; i < 20000u; ++i) {
        (void)mcl_rdv_poll(&s.rdv, &ev);
        room_advance(&room, 10u);
    }
    printf("       (%u token calls, %u offers, state %u)\n",
           g_token_calls, count_kind(&room, MCL_WIRE_KIND_TRANSPORT_OFFER),
           (unsigned)s.rdv.state);
    check(g_token_calls > 0u, "the token allocator was asked");
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_OFFER) == 0u,
          "no offer claims an address the integrator would not mint");
    check(s.rdv.state == MCL_RDV_STATE_EXHAUSTED,
          "and the bearers are exhausted rather than retried for ever");
}


/* ------------------------------------------- the three transmit outcomes
 *
 * mcl_sdk_tx_fn defines three, deliberately: sent, DEFINITELY not sent, and
 * "the transport cannot tell". The coordinator used to collapse the third into
 * the first, so a frame that may already have reached the peer and a frame
 * that certainly did not were the same thing one layer above the place that
 * took care to separate them.
 *
 * The two consequences point opposite ways, which is why they cannot share a
 * branch:
 *
 *   DEFINITE REFUSAL -- the peer has nothing. Advancing state spends a timeout
 *   waiting for an answer to something never said, consumes a retry, and
 *   eventually reports NO_COMMON_BEARER about a peer that was never asked.
 *
 *   UNCERTAIN -- the peer may already have acted. Retrying from the top puts a
 *   second transaction on the air racing a live one. The safe move is to
 *   advance and let the idempotent retransmission settle it.
 *
 * So each control is driven through all three and the state asserted after
 * each.
 */
static void test_transmit_outcomes(void)
{
    room_t room;
    unit_t u;
    mcl_rdv_event_t ev;
    mcl_wire_tier0_t object;
    unsigned i;

    reset_harness();
    printf("[rendezvous] sent, definitely not sent, and cannot tell\n");

    /* ---- PRESENCE, definite refusal ---- */
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&u, &room, 0u, 0xD1D1D1D1u);
    room.tx_force = -1;
    room.tx_force_left = 1;
    for (i = 0u; i < 2000u && room.tx_forced == 0u; ++i) {
        (void)mcl_rdv_poll(&u.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(room.tx_forced == 1u, "the transport refused one PRESENCE");
    check(u.rdv.state == MCL_RDV_STATE_ANNOUNCING,
          "a refused PRESENCE does not make this machine a solicitor");
    check(u.rdv.announcements == 0u, "and does not count as an announcement");
    check(count_kind(&room, MCL_WIRE_KIND_PRESENCE) == 0u,
          "nothing reached the air");

    /* ---- PRESENCE, uncertain: it DOES advance ---- */
    room.tx_force = 1;
    room.tx_force_left = 1;
    room.tx_forced = 0u;
    for (i = 0u; i < 2000u && room.tx_forced == 0u; ++i) {
        (void)mcl_rdv_poll(&u.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(room.tx_forced == 1u, "the transport could not tell about the next");
    check(u.rdv.state == MCL_RDV_STATE_SOLICITING,
          "an uncertain PRESENCE DOES make this machine a solicitor");
    check(u.rdv.announcements == 1u,
          "and counts, because the bytes may well have gone out");
    room.tx_force = 0;

    /* ---- OFFER, definite refusal, then success ---- */
    reset_harness();
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&u, &room, 0u, 0xD2D2D2D2u);
    make_presence(&object, 0x0A0A0A0Au);
    deliver_object(&u.rdv, 1u, &object);
    check(u.rdv.state == MCL_RDV_STATE_HEARD, "it is a responder");
    room.tx_force = -1;
    room.tx_force_left = 1;
    for (i = 0u; i < 2000u && room.tx_forced == 0u; ++i) {
        (void)mcl_rdv_poll(&u.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(room.tx_forced == 1u, "the transport refused one OFFER");
    check(u.rdv.state != MCL_RDV_STATE_OFFERING,
          "a refused OFFER does not enter OFFERING, where an answer is awaited");
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_OFFER) == 0u,
          "and nothing reached the air");
    room.tx_force = 0;
    for (i = 0u; i < 4000u && u.rdv.state != MCL_RDV_STATE_OFFERING; ++i) {
        (void)mcl_rdv_poll(&u.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(u.rdv.state == MCL_RDV_STATE_OFFERING,
          "the offer goes out once the transport recovers");
    check(count_kind(&room, MCL_WIRE_KIND_TRANSPORT_OFFER) == 1u,
          "exactly once");

    /* ---- ACCEPT, definite refusal, then uncertain ---- */
    reset_harness();
    room_init(&room);
    g_room = &room;
    g_node_count = 1u;
    unit_start(&u, &room, 0u, 0xD3D3D3D3u);
    check(drive_to_soliciting(&u, &room), "it owns a round");
    settle(&u, &room, 1000u);
    make_offer(&object, 0x0B0B0B0Bu, 0x41414141u, 3u, 1u, 0xB0000001u);
    deliver_object(&u.rdv, 1u, &object);
    check(u.rdv.state == MCL_RDV_STATE_ACCEPTING, "a contender is selected");
    room.tx_force = -1;
    room.tx_force_left = 1;
    room.tx_forced = 0u;
    for (i = 0u; i < 2000u && room.tx_forced == 0u; ++i) {
        (void)mcl_rdv_poll(&u.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(room.tx_forced == 1u, "the transport refused one ACCEPT");
    check(u.rdv.has_pending_accept == 1u,
          "a refused ACCEPT stays armed rather than being considered sent");
    check(u.rdv.state == MCL_RDV_STATE_ACCEPTING,
          "and the machine is still waiting to send it");
    room.tx_force = 1;
    room.tx_force_left = 1;
    room.tx_forced = 0u;
    for (i = 0u; i < 2000u && room.tx_forced == 0u; ++i) {
        (void)mcl_rdv_poll(&u.rdv, &ev);
        room_advance(&room, 10u);
    }
    check(room.tx_forced == 1u, "the transport could not tell about the next");
    check(u.rdv.has_pending_accept == 0u,
          "an uncertain ACCEPT is treated as sent, not armed again");
    check(u.rdv.state == MCL_RDV_STATE_AGREED,
          "and the machine advances to await the challenge");
    check(u.rdv.session_ref != 0u, "holding one session for the contact");

    /*
     * MCL_RDV_TX_UNCERTAIN NEVER REACHES A CALLER IN THIS RELEASE.
     *
     * The distinction is real inside the coordinator -- it decides whether an
     * announcement counts, whether OFFERING is entered, and whether a prepared
     * acceptance stays armed -- but every consumer of it is a stage function
     * returning void, so the value does not escape. The header says so, and
     * this is what holds the header to it, exactly as the check above holds it
     * to never emitting SECURITY_ESTABLISHED. A declared-and-unreachable value
     * is only honest while somebody is checking.
     *
     * Every public entry point is driven here with the transport answering
     * "cannot tell" on every call.
     */
    {
        room_t r2;
        unit_t v;
        mcl_rdv_event_t e2;
        int leaked = 0;
        unsigned n;

        reset_harness();
        room_init(&r2);
        g_room = &r2;
        g_node_count = 1u;
        unit_start(&v, &r2, 0u, 0xD4D4D4D4u);
        r2.tx_unknown = 1;              /* every send answers "cannot tell" */
        for (n = 0u; n < 4000u; ++n) {
            if (mcl_rdv_poll(&v.rdv, &e2) == MCL_RDV_TX_UNCERTAIN) leaked = 1;
            room_advance(&r2, 10u);
        }
        if (mcl_rdv_start(&v.rdv) == MCL_RDV_TX_UNCERTAIN) leaked = 1;
        if (mcl_rdv_candidate_ready(&v.rdv) == MCL_RDV_TX_UNCERTAIN) leaked = 1;
        if (mcl_rdv_admit(&v.rdv) == MCL_RDV_TX_UNCERTAIN) leaked = 1;
        if (mcl_rdv_refuse(&v.rdv) == MCL_RDV_TX_UNCERTAIN) leaked = 1;
        {
            mcl_wire_tier0_t o;
            uint8_t buf[MAX_FRAME_SIZE];
            size_t written = 0u;
            make_presence(&o, 0x0E0E0E0Eu);
            if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &o, buf,
                                               sizeof(buf), &written)
                == MCL_WIRE_OK) {
                if (mcl_rdv_deliver(&v.rdv, 1u, buf, written)
                    == MCL_RDV_TX_UNCERTAIN) leaked = 1;
            }
        }
        check(!leaked,
              "no public entry point returns MCL_RDV_TX_UNCERTAIN, as declared");
    }
}

int main(void)
{
    printf("rendezvous coordinator\n");
    test_config_refusals();
    test_two_strangers_to_migration();
    test_third_party_offer_refused();
    test_contention_recovers();
    test_colliding_source_refs();
    test_no_common_bearer();
    test_profile_must_match();
    test_fresh_transaction_refs();
    test_self_echo_ignored();
    test_garbage_refused();
    test_clock_wrap();
    test_backoff_slotting();
    test_zero_source_ref();
    test_lost_handoff_controls();
    test_policy_refusal();
    test_same_slot_collides();
    test_first_offer_is_selected();
    test_responders_ignore_each_other();
    test_unselected_responder_stands_down();
    test_migration_ref_collision_aborts();
    test_lost_acceptance_recovers();
    test_forced_cycle_dissolves();
    test_presence_collision_converges();
    test_responder_refs_differ();
    test_allocator_boundary();
    test_allocator_refusal();
    test_transmit_outcomes();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

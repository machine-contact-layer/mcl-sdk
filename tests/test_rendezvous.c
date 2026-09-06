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
    t = &room->tx[room->count++];
    memset(t, 0, sizeof(*t));
    t->transport_id = transport_id;
    memcpy(t->bytes, data, size);
    t->size = size;
    t->from = ctx->index;
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
    return room->tx_unknown ? 1 : 0;
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
        if (t->start_ms <= room->now && room->now < t->end_ms) {
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
}

static void tick(room_t *room, mcl_rdv_event_t *events, uint32_t step_ms)
{
    unsigned j;

    for (j = 0u; j < g_node_count; ++j) {
        (void)mcl_rdv_poll(g_nodes[j], &events[j]);
    }
    room_advance(room, step_ms);
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
    cfg->announce_interval_ms = 1500u;
    cfg->response_timeout_ms = 4000u;
    cfg->backoff_slots = 16u;
    cfg->backoff_slot_ms = 250u;
    cfg->max_announcements = 40u;
    cfg->max_offer_retries = 1u;
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
    for (i = 0u; i < MAX_NODES; ++i) {
        room->seed[i] = 0x1234567u + i * 7919u;
    }
}

/* ------------------------------------------------------------ the cases */

static void test_config_refusals(void)
{
    mcl_rdv_t r;
    mcl_rdv_config_t cfg;
    mcl_rdv_platform_t plat;
    mcl_node_t node;
    room_t room;
    peer_ctx_t ctx;

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
    unsigned i;
    static const uint32_t refs[3] = { 0x51515151u, 0x52525252u, 0x53535353u };

    printf("[rendezvous] three machines contending on one medium\n");
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
    for (i = 0u; i < 3000u; ++i) {
        tick(&room, ev, 10u);
    }

    printf("       (%u delivered, %u lost to overlap)\n",
           room.delivered, room.collisions);
    check(room.delivered > 0u,
          "traffic gets through three simultaneous starters");
    /*
     * The load-bearing check. Under the old rule every reply to a shared
     * PRESENCE overlapped by construction, so a majority-collision outcome is
     * the signature of a scheme that cannot work at all.
     */
    check(room.delivered > room.collisions,
          "and most transmissions survive rather than most colliding");
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
    int discovered = 0;

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
    for (i = 0u; i < 2000u; ++i) {
        tick(&room, ev, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_PEER_DISCOVERED ||
            ev[1].kind == MCL_RDV_EVENT_PEER_DISCOVERED) discovered = 1;
    }
    check(discovered,
          "self-echo suppression uses the platform, not source_ref equality");
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
    for (i = 0u; i < 6000u; ++i) {
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
    printf("       (A state=%d bearer_index=%u | B state=%d bearer_index=%u)\n",
           (int)a.state, (unsigned)a.bearer_index,
           (int)b.state, (unsigned)b.bearer_index);
    check(!agreed, "disjoint bearer sets never reach agreement");
    check(none, "NO_COMMON_BEARER is reported, not left as a silence");
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

    printf("[rendezvous] self-echo\n");
    room_init(&room);
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 0xABCDEF01u, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&ca, 0xABCDEF01u, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);
    g_room = &room; g_nodes[0] = &a; g_node_count = 1u;

    /* Poll once so the node is actually transmitting, then feed its own bytes
       back while its emitter is live. */
    (void)mcl_rdv_poll(&a, &ev);

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

    printf("[rendezvous] the millisecond clock wrap\n");
    room_init(&room);
    room.now = 0xFFFFFF00u;
    xa.room = &room; xa.index = 0u;
    base_platform(&pa, &xa);
    base_node(&na, 0x5A5A5A5Bu, MCL_CONTACT_ROLE_INITIATOR, &xa);
    base_config(&ca, 0x5A5A5A5Bu, MCL_CONTACT_ROLE_INITIATOR);
    ca.announce_interval_ms = 100u;
    ca.backoff_slots = 1u;           /* isolate the deadline from the backoff */
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);
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
    uint16_t seen[16];
    int in_range = 1, aligned = 1;

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

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

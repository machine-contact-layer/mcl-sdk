/*
 * Rendezvous coordinator checks.
 *
 * TWO COORDINATORS, ONE SIMULATED ROOM
 *
 * Most of this file runs two independently configured coordinators against
 * each other through a simulated bearer and a simulated clock. That is the
 * only arrangement in which the interesting failures appear at all: a single
 * coordinator driven by a script never glares, never receives a stale
 * acceptance, and never discovers that the peer speaks nothing it speaks.
 *
 * The clock is simulated on purpose rather than for speed. It lets the reply
 * slotting, the retry bounds and the 2^32 wrap be tested exactly, and none of
 * those can be tested reliably against a real clock.
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

#define MAX_FRAMES 32u
#define MAX_FRAME_SIZE 64u

typedef struct {
    uint8_t transport_id;
    uint8_t bytes[MAX_FRAME_SIZE];
    size_t size;
    uint8_t from;          /* index of the emitting node */
} air_frame_t;

typedef struct {
    uint32_t now;
    air_frame_t frames[MAX_FRAMES];
    unsigned count;
    /* When set, tx reports "cannot tell" rather than success. */
    int tx_unknown;
    /* When set, every emission is dropped: a bearer nobody is listening on. */
    int deaf;
} room_t;

typedef struct {
    room_t *room;
    uint8_t index;
} peer_ctx_t;

static int32_t room_tx(void *user, uint8_t transport_id,
                       const uint8_t *data, size_t size)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user;
    room_t *room = ctx->room;

    if (room->deaf) {
        return room->tx_unknown ? 1 : 0;
    }
    if (room->count < MAX_FRAMES && size <= MAX_FRAME_SIZE) {
        air_frame_t *f = &room->frames[room->count++];
        f->transport_id = transport_id;
        memcpy(f->bytes, data, size);
        f->size = size;
        f->from = ctx->index;
    }
    return room->tx_unknown ? 1 : 0;
}

static uint32_t room_now(void *user)
{
    return ((peer_ctx_t *)user)->room->now;
}

/* Deliver everything in the room to every node except its emitter. */
static void room_flush(room_t *room, mcl_rdv_t **nodes, unsigned node_count)
{
    unsigned i, j;
    air_frame_t pending[MAX_FRAMES];
    unsigned n = room->count;

    memcpy(pending, room->frames, sizeof(air_frame_t) * n);
    room->count = 0u;

    for (i = 0u; i < n; ++i) {
        for (j = 0u; j < node_count; ++j) {
            if (pending[i].from == (uint8_t)j) continue;
            (void)mcl_rdv_deliver(nodes[j], pending[i].transport_id,
                                  pending[i].bytes, pending[i].size);
        }
    }
}

/* One tick: poll every node, then move whatever they emitted. */
static void tick(room_t *room, mcl_rdv_t **nodes, mcl_rdv_event_t *events,
                 unsigned node_count, uint32_t step_ms)
{
    unsigned j;

    for (j = 0u; j < node_count; ++j) {
        (void)mcl_rdv_poll(nodes[j], &events[j]);
    }
    room_flush(room, nodes, node_count);
    room->now += step_ms;
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
    cfg->announce_interval_ms = 100u;
    cfg->response_timeout_ms = 300u;
    cfg->reply_slot_ms = 50u;
    cfg->max_announcements = 20u;
    cfg->max_offer_retries = 1u;
    cfg->capability_tag = 0x000004u;
    cfg->presence_ttl = 60u;
}

/* ------------------------------------------------------------ the cases */

static void test_config_refusals(void)
{
    mcl_rdv_t r;
    mcl_rdv_config_t cfg;
    mcl_rdv_platform_t plat;
    mcl_node_t node;
    mcl_node_config_t ncfg;
    room_t room;
    peer_ctx_t ctx;

    memset(&room, 0, sizeof(room));
    ctx.room = &room; ctx.index = 0u;
    memset(&plat, 0, sizeof(plat));
    plat.tx = room_tx; plat.now_ms = room_now; plat.user = &ctx;

    memset(&ncfg, 0, sizeof(ncfg));
    ncfg.supported_wire_majors_mask = 1u << 1;
    ncfg.source_ref = 0xA1u;
    ncfg.transport_id = 1u;
    ncfg.role = MCL_CONTACT_ROLE_INITIATOR;
    (void)mcl_node_init(&node, &ncfg);

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    check(mcl_rdv_init(NULL, &cfg, &plat, &node) == MCL_RDV_ERR_NULL,
          "init refuses a NULL coordinator");

    /*
     * transport_id 0 is reserved so an uninitialised field never names a
     * medium. A coordinator that accepted it would put a meaningless offer on
     * the air, where the peer -- correctly -- ignores it, and the deployment
     * would look like a bearer mismatch instead of a configuration error.
     */
    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    cfg.bearer_transport_id[1] = 0u;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses a bearer with transport_id 0");

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    cfg.bootstrap_transport_id = 0u;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses bootstrap transport_id 0");

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    cfg.bearer_count = 0u;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses an empty bearer list");

    base_config(&cfg, 0xA1u, MCL_CONTACT_ROLE_INITIATOR);
    plat.now_ms = NULL;
    check(mcl_rdv_init(&r, &cfg, &plat, &node) == MCL_RDV_ERR_CONFIG,
          "init refuses a platform with no clock");
}

/*
 * Two strangers, no prearrangement of any kind: neither is told the other's
 * address, token, or bearer list. They reach agreement on a bearer both
 * deployments mandate.
 */
static void test_two_strangers(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_t *nodes[2];
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    mcl_node_config_t nca, ncb;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int agreed_a = 0, agreed_b = 0, discovered = 0, security_claimed = 0;

    memset(&room, 0, sizeof(room));
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;

    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;
    pb = pa; pb.user = &xb;

    memset(&nca, 0, sizeof(nca));
    nca.supported_wire_majors_mask = 1u << 1;
    nca.source_ref = 0x11111111u;
    nca.transport_id = 1u;
    nca.role = MCL_CONTACT_ROLE_INITIATOR;
    ncb = nca;
    ncb.source_ref = 0x22222222u;
    ncb.role = MCL_CONTACT_ROLE_RESPONDER;
    (void)mcl_node_init(&na, &nca);
    (void)mcl_node_init(&nb, &ncb);

    base_config(&ca, 0x11111111u, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x22222222u, MCL_CONTACT_ROLE_RESPONDER);

    check(mcl_rdv_init(&a, &ca, &pa, &na) == MCL_RDV_OK, "A initialises");
    check(mcl_rdv_init(&b, &cb, &pb, &nb) == MCL_RDV_OK, "B initialises");
    check(mcl_rdv_start(&a) == MCL_RDV_OK, "A starts");
    check(mcl_rdv_start(&b) == MCL_RDV_OK, "B starts");
    check(mcl_rdv_start(&a) == MCL_RDV_ERR_STATE,
          "start refuses a second time");

    nodes[0] = &a; nodes[1] = &b;
    for (i = 0u; i < 200u; ++i) {
        tick(&room, nodes, ev, 2u, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_PEER_DISCOVERED ||
            ev[1].kind == MCL_RDV_EVENT_PEER_DISCOVERED) discovered = 1;
        if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed_a = 1;
        if (ev[1].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed_b = 1;
        if (ev[0].kind == MCL_RDV_EVENT_SECURITY_ESTABLISHED ||
            ev[1].kind == MCL_RDV_EVENT_SECURITY_ESTABLISHED) {
            security_claimed = 1;
        }
    }

    check(discovered, "a peer is discovered from PRESENCE alone");
    check(agreed_a && agreed_b, "both peers reach BEARER_AGREED");
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_AGREED, "A ends AGREED");
    check(mcl_rdv_state(&b) == MCL_RDV_STATE_AGREED, "B ends AGREED");

    /*
     * The point of this check is that it can never pass in this release. MCL
     * has no Stable SECURITY class and no assigned feature bits, so there is
     * nowhere legal for handshake bytes to travel; a coordinator that emitted
     * SECURITY_ESTABLISHED would be claiming a property no code here provides.
     * The event exists so the vocabulary survives MCL-S1 landing.
     */
    check(!security_claimed,
          "no build of this release ever claims security is established");

    /*
     * THE CHECK THAT MAKES THE GLARE MEAN ANYTHING.
     *
     * Both peers announce, both hear, both offer -- so every run of this case
     * IS a simultaneous-offer collision. "Both reached AGREED" does not show it
     * was resolved: two peers that simply accepted each other would also both
     * be AGREED, while holding two different live transactions on one contact.
     *
     * Convergence on ONE migration_ref is what distinguishes a resolved
     * collision from two unresolved ones, and it is asserted here rather than
     * inferred from the states.
     */
    check(a.migration_ref == b.migration_ref && a.migration_ref != 0u,
          "after glare both peers hold the SAME migration transaction");
    check(a.peer_ref == cb.source_ref && b.peer_ref == ca.source_ref,
          "each peer learned the other's contact reference, and only that");
}

/*
 * Two peers whose deployments mandate disjoint bearer sets. They hear each
 * other perfectly and still cannot continue -- and that outcome is REPORTED
 * rather than left as a silence, because an operator needs to tell "nobody is
 * there" from "somebody is there and we share nothing".
 */
static void test_no_common_bearer(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_t *nodes[2];
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int none_a = 0, agreed = 0;

    memset(&room, 0, sizeof(room));
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;
    pb = pa; pb.user = &xb;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0x33333333u;
    nc.transport_id = 1u;
    nc.role = MCL_CONTACT_ROLE_INITIATOR;
    (void)mcl_node_init(&na, &nc);
    nc.source_ref = 0x44444444u;
    (void)mcl_node_init(&nb, &nc);

    base_config(&ca, 0x33333333u, MCL_CONTACT_ROLE_INITIATOR);
    ca.bearer_count = 1u;
    ca.bearer_transport_id[0] = 3u;      /* BLE only */

    base_config(&cb, 0x44444444u, MCL_CONTACT_ROLE_RESPONDER);
    cb.bearer_count = 1u;
    cb.bearer_transport_id[0] = 4u;      /* UWB only: shares nothing with A */

    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    nodes[0] = &a; nodes[1] = &b;
    for (i = 0u; i < 400u; ++i) {
        tick(&room, nodes, ev, 2u, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_NO_COMMON_BEARER) none_a = 1;
        if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED ||
            ev[1].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed = 1;
    }

    check(!agreed, "disjoint bearer sets never reach agreement");
    check(none_a, "NO_COMMON_BEARER is reported, not left as a silence");
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_EXHAUSTED,
          "the exhausted peer says so in its state");
}

/*
 * Nobody answers. This must NOT be reported as NO_COMMON_BEARER: no bearer was
 * ever tried, because no peer was ever heard, and telling an operator their
 * bearers are incompatible when the room was empty sends them to the wrong
 * problem.
 */
static void test_empty_room(void)
{
    mcl_rdv_t a;
    mcl_rdv_t *nodes[1];
    mcl_rdv_event_t ev[1];
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa;
    unsigned i;
    int wrong_event = 0;

    memset(&room, 0, sizeof(room));
    room.deaf = 1;
    xa.room = &room; xa.index = 0u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0x55555555u;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);

    base_config(&ca, 0x55555555u, MCL_CONTACT_ROLE_INITIATOR);
    ca.max_announcements = 5u;
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    nodes[0] = &a;
    for (i = 0u; i < 200u; ++i) {
        tick(&room, nodes, ev, 1u, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_NO_COMMON_BEARER) wrong_event = 1;
    }
    check(!wrong_event,
          "an empty room is not reported as a bearer incompatibility");
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_IDLE,
          "announcing gives up back to IDLE");
}

/*
 * A transport that cannot tell whether it sent must not stall the bootstrap.
 * sdk.h requires such a transport to return > 0, and the coordinator treats
 * that as "possibly sent" rather than as a failure.
 */
static void test_tx_unknown(void)
{
    mcl_rdv_t a, b;
    mcl_rdv_t *nodes[2];
    mcl_rdv_event_t ev[2];
    mcl_rdv_config_t ca, cb;
    mcl_rdv_platform_t pa, pb;
    mcl_node_t na, nb;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa, xb;
    unsigned i;
    int agreed = 0;

    memset(&room, 0, sizeof(room));
    room.tx_unknown = 1;
    xa.room = &room; xa.index = 0u;
    xb.room = &room; xb.index = 1u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;
    pb = pa; pb.user = &xb;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0x66666666u;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);
    nc.source_ref = 0x77777777u;
    (void)mcl_node_init(&nb, &nc);

    base_config(&ca, 0x66666666u, MCL_CONTACT_ROLE_INITIATOR);
    base_config(&cb, 0x77777777u, MCL_CONTACT_ROLE_RESPONDER);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_init(&b, &cb, &pb, &nb);
    (void)mcl_rdv_start(&a);
    (void)mcl_rdv_start(&b);

    nodes[0] = &a; nodes[1] = &b;
    for (i = 0u; i < 200u; ++i) {
        tick(&room, nodes, ev, 2u, 10u);
        if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed = 1;
    }
    check(agreed, "a transport that cannot vouch for a send does not stall");
}

/*
 * A delayed TRANSPORT_ACCEPT from an abandoned transaction must not be taken
 * for the acceptance of the current one. transport_id and profile_id are
 * usually identical across a retry, so migration_ref is the only field that
 * can tell them apart -- which is exactly why wire.h gives it one job.
 */
static void test_stale_accept_refused(void)
{
    mcl_rdv_t a;
    mcl_rdv_t *nodes[1];
    mcl_rdv_event_t ev[1];
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa;
    mcl_wire_tier0_t stale;
    uint8_t bytes[MCL_WIRE_TIER0_MAX_SIZE];
    size_t written = 0u;
    unsigned i;
    int agreed = 0;

    memset(&room, 0, sizeof(room));
    xa.room = &room; xa.index = 0u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0x88888888u;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);

    base_config(&ca, 0x88888888u, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    /* Drive it into OFFERING by delivering a peer PRESENCE. */
    {
        mcl_wire_tier0_t presence;
        memset(&presence, 0, sizeof(presence));
        presence.kind = MCL_WIRE_KIND_PRESENCE;
        presence.priority = 1u;
        presence.source_ref = 0x99999999u;
        presence.body.presence.capability_tag = 1u;
        presence.body.presence.ttl = 60u;
        (void)mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &presence,
                                             bytes, sizeof(bytes), &written);
        (void)mcl_rdv_deliver(&a, 1u, bytes, written);
    }
    nodes[0] = &a;
    for (i = 0u; i < 20u; ++i) {
        tick(&room, nodes, ev, 1u, 10u);
        if (mcl_rdv_state(&a) == MCL_RDV_STATE_OFFERING) break;
    }
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_OFFERING,
          "a heard peer leads to an offer");

    memset(&stale, 0, sizeof(stale));
    stale.kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
    stale.priority = 1u;
    stale.source_ref = 0x99999999u;
    stale.body.transport_accept.migration_ref = 0x0BADBEEFu;  /* not ours */
    stale.body.transport_accept.transport_id = 3u;            /* right bearer */
    stale.body.transport_accept.profile_id = 1u;              /* right profile */
    stale.body.transport_accept.session_ref = 0x99999999u;
    (void)mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &stale,
                                         bytes, sizeof(bytes), &written);
    (void)mcl_rdv_deliver(&a, 1u, bytes, written);
    (void)mcl_rdv_poll(&a, &ev[0]);
    if (ev[0].kind == MCL_RDV_EVENT_BEARER_AGREED) agreed = 1;

    check(!agreed,
          "an ACCEPT for another migration_ref is not taken as agreement");
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_OFFERING,
          "and the offer is still outstanding");
}

/* An emission heard back must not be treated as a peer. */
static void test_self_echo_ignored(void)
{
    mcl_rdv_t a;
    mcl_rdv_event_t ev;
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa;
    mcl_wire_tier0_t echo;
    uint8_t bytes[MCL_WIRE_TIER0_MAX_SIZE];
    size_t written = 0u;

    memset(&room, 0, sizeof(room));
    xa.room = &room; xa.index = 0u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0xABCDEF01u;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);

    base_config(&ca, 0xABCDEF01u, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    memset(&echo, 0, sizeof(echo));
    echo.kind = MCL_WIRE_KIND_PRESENCE;
    echo.priority = 1u;
    echo.source_ref = 0xABCDEF01u;          /* our own */
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
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa;
    static const uint8_t junk[] = { 0xFFu, 0x00u, 0x7Fu, 0x13u, 0x99u };

    memset(&room, 0, sizeof(room));
    xa.room = &room; xa.index = 0u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0x0F0F0F0Fu;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);

    base_config(&ca, 0x0F0F0F0Fu, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    check(mcl_rdv_deliver(&a, 1u, junk, sizeof(junk)) == MCL_RDV_OK,
          "undecodable bytes are refused without reporting a fault");
    check(mcl_rdv_state(&a) == MCL_RDV_STATE_ANNOUNCING,
          "and the coordinator has not moved");
}

/*
 * Every deadline is compared by subtraction so it survives the 2^32 wrap of a
 * monotonic millisecond clock. Started near the wrap, the machine must still
 * announce -- the naive `now >= deadline` form stops firing entirely at about
 * 49.7 days of uptime, which is a defect that only ever appears in the field.
 */
static void test_clock_wrap(void)
{
    mcl_rdv_t a;
    mcl_rdv_t *nodes[1];
    mcl_rdv_event_t ev[1];
    mcl_rdv_config_t ca;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa;
    unsigned i;
    unsigned emissions;

    memset(&room, 0, sizeof(room));
    room.now = 0xFFFFFF00u;              /* 256 ms before the wrap */
    xa.room = &room; xa.index = 0u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 0x5A5A5A5Au;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);

    base_config(&ca, 0x5A5A5A5Au, MCL_CONTACT_ROLE_INITIATOR);
    ca.announce_interval_ms = 100u;
    ca.max_announcements = 10u;
    (void)mcl_rdv_init(&a, &ca, &pa, &na);
    (void)mcl_rdv_start(&a);

    nodes[0] = &a;
    emissions = 0u;
    for (i = 0u; i < 60u; ++i) {
        (void)mcl_rdv_poll(&a, &ev[0]);
        emissions += room.count;
        room.count = 0u;
        room.now += 50u;                 /* walks straight through the wrap */
    }
    check(emissions >= 5u,
          "deadlines still fire across the 2^32 millisecond wrap");
}

/*
 * Reply slotting. Contention is protocol: ten machines answering one PRESENCE
 * at once defeat a perfect modem, so a reply is placed in a slot. With a
 * random source the slot is spread across the window; without one it is
 * derived from source_ref, which separates two peers and not ten -- the test
 * records both so the weaker guarantee is visible rather than assumed.
 */
static void test_reply_slotting(void)
{
    mcl_rdv_t r;
    mcl_rdv_config_t cfg;
    mcl_rdv_platform_t pa;
    mcl_node_t na;
    mcl_node_config_t nc;
    room_t room;
    peer_ctx_t xa;
    uint16_t d;
    int within = 1;
    unsigned i;
    unsigned distinct = 0u;
    uint16_t seen[8];

    memset(&room, 0, sizeof(room));
    xa.room = &room; xa.index = 0u;
    memset(&pa, 0, sizeof(pa));
    pa.tx = room_tx; pa.now_ms = room_now; pa.user = &xa;

    memset(&nc, 0, sizeof(nc));
    nc.supported_wire_majors_mask = 1u << 1;
    nc.source_ref = 1u;
    nc.transport_id = 1u;
    (void)mcl_node_init(&na, &nc);

    /* No random source: the deterministic derivation. */
    for (i = 0u; i < 8u; ++i) {
        base_config(&cfg, 1000u + i * 37u, MCL_CONTACT_ROLE_INITIATOR);
        cfg.reply_slot_ms = 400u;
        (void)mcl_rdv_init(&r, &cfg, &pa, &na);
        d = mcl_rdv_reply_delay_ms(&r);
        if (d >= 400u) within = 0;
        seen[i] = d;
    }
    check(within, "a reply delay always lands inside the slot window");

    for (i = 0u; i < 8u; ++i) {
        unsigned j;
        int dup = 0;
        for (j = 0u; j < i; ++j) if (seen[j] == seen[i]) dup = 1;
        if (!dup) distinct++;
    }
    /*
     * Not asserted as a strong property: with no randomness the delay is a
     * function of source_ref alone, so two nodes whose refs are congruent
     * modulo the slot width collide every single time. Recording the count
     * keeps that visible instead of letting a passing test imply a guarantee
     * the implementation does not make.
     */
    printf("  (deterministic slotting: %u distinct delays from 8 refs)\n",
           distinct);
    check(distinct >= 2u,
          "deterministic slotting separates at least some peers");
}

int main(void)
{
    printf("rendezvous coordinator\n");
    test_config_refusals();
    test_two_strangers();
    test_no_common_bearer();
    test_empty_room();
    test_tx_unknown();
    test_stale_accept_refused();
    test_self_echo_ignored();
    test_garbage_refused();
    test_clock_wrap();
    test_reply_slotting();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

/*
 * The integration facade, exercised the way an integrator uses it.
 *
 * WHAT THIS FILE IS FOR, AND WHAT IT DELIBERATELY DOES NOT REPEAT
 *
 * `test_rendezvous.c` proves the protocol: airtime, collisions, solicitation
 * epochs, retransmission, irrevocable COMMIT. None of that is repeated here.
 *
 * This file asks a different question: can somebody who has read only
 * `mcl/machine.h` write five callbacks and get a contact? So it uses the
 * facade and NOTHING else -- no `mcl_rdv_*` call appears below, no
 * `mcl_node_*` call, no wire object is built by hand. If a step turns out to
 * need one, the facade is not finished, and that would be this test failing to
 * be writable rather than failing to pass.
 *
 * The room is correspondingly small: point-to-point delivery, a simulated
 * clock, no overlap modelling. Contention lives one layer down and is tested
 * there.
 */

#include "mcl/machine.h"

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

/* --------------------------------------------------------------- the room */

#define MAX_NODES 2u
#define MAX_TX 32u
#define MAX_FRAME 128u

typedef struct {
    uint8_t transport_id;
    uint8_t bytes[MAX_FRAME];
    size_t size;
    uint8_t from;
    uint32_t deliver_at;
    uint8_t delivered;
} transmission_t;

typedef struct room_s {
    uint32_t now;
    transmission_t tx[MAX_TX];
    unsigned count;
    unsigned dropped_full;
    uint32_t seed[MAX_NODES];
    /* Which bearers this room can actually carry. A bearer that is not here
       is one the peer does not have, which is the case NO_COMMON_BEARER is
       for. */
    uint8_t carries_ble;
    uint8_t carries_ip;
    /* Set by the platform hook, read by the test: what the integrator was
       asked to open, and with what. */
    uint8_t last_open_transport[MAX_NODES];
    uint32_t last_open_peer_token[MAX_NODES];
    uint32_t last_open_local_token[MAX_NODES];
    unsigned opens[MAX_NODES];
    unsigned closes[MAX_NODES];
    unsigned policy_calls[MAX_NODES];
    /* Make candidate_open answer PENDING, so the deferred path is exercised
       rather than only the easy one. */
    int pending_open;
    int refuse_open;
    int refuse_policy;
} room_t;

typedef struct {
    room_t *room;
    unsigned index;
    mcl_machine_t *machine;
} peer_t;

static room_t g_room;
static peer_t g_peers[MAX_NODES];
static mcl_machine_t g_machines[MAX_NODES];
static unsigned g_node_count;

static uint32_t plat_clock(void *user)
{
    peer_t *p = (peer_t *)user;
    return p->room->now;
}

/* Deterministic, so a "random" run is reproducible and a result is not luck. */
static int plat_random(void *user, uint8_t *out, size_t size)
{
    peer_t *p = (peer_t *)user;
    size_t i;
    for (i = 0u; i < size; ++i) {
        p->room->seed[p->index] =
            p->room->seed[p->index] * 1664525u + 1013904223u;
        out[i] = (uint8_t)(p->room->seed[p->index] >> 24);
    }
    return 0;
}

static int plat_medium_busy(void *user) { (void)user; return 0; }
static int plat_self_tx(void *user) { (void)user; return 0; }

static int32_t plat_send(void *user, uint8_t transport_id,
                         const uint8_t *data, size_t size)
{
    peer_t *p = (peer_t *)user;
    room_t *room = p->room;
    transmission_t *t;

    if (size > MAX_FRAME) { return -1; }
    if (room->count >= MAX_TX) {
        room->dropped_full++;
        return -1;
    }
    if (transport_id == MCL_CONTACT_TRANSPORT_BLE && !room->carries_ble) {
        return -1;
    }
    if (transport_id == MCL_CONTACT_TRANSPORT_IP && !room->carries_ip) {
        return -1;
    }
    t = &room->tx[room->count++];
    memset(t, 0, sizeof(*t));
    t->transport_id = transport_id;
    memcpy(t->bytes, data, size);
    t->size = size;
    t->from = (uint8_t)p->index;
    t->deliver_at = room->now + 10u;
    return 0;
}

static mcl_machine_candidate_t plat_open(void *user, uint8_t transport_id,
                                         uint8_t profile_id,
                                         uint32_t peer_endpoint_token,
                                         uint32_t local_endpoint_token)
{
    peer_t *p = (peer_t *)user;
    (void)profile_id;
    p->room->opens[p->index]++;
    p->room->last_open_transport[p->index] = transport_id;
    p->room->last_open_peer_token[p->index] = peer_endpoint_token;
    p->room->last_open_local_token[p->index] = local_endpoint_token;
    if (p->room->refuse_open) { return MCL_MACHINE_CANDIDATE_REFUSED; }
    if (p->room->pending_open) { return MCL_MACHINE_CANDIDATE_PENDING; }
    return MCL_MACHINE_CANDIDATE_READY;
}

static void plat_close(void *user, uint8_t transport_id)
{
    peer_t *p = (peer_t *)user;
    (void)transport_id;
    p->room->closes[p->index]++;
}

static int plat_policy(void *user, uint32_t peer_ref, uint8_t transport_id)
{
    peer_t *p = (peer_t *)user;
    (void)peer_ref;
    (void)transport_id;
    p->room->policy_calls[p->index]++;
    return p->room->refuse_policy ? 0 : 1;
}

static void base_platform(mcl_platform_t *plat, peer_t *peer, int with_policy)
{
    memset(plat, 0, sizeof(*plat));
    plat->clock_ms = plat_clock;
    plat->random_bytes = plat_random;
    plat->transport_send = plat_send;
    plat->medium_busy = plat_medium_busy;
    plat->self_transmitting = plat_self_tx;
    plat->candidate_open = plat_open;
    plat->candidate_close = plat_close;
    plat->policy_admit = with_policy ? plat_policy : NULL;
    plat->user = peer;
}

static void room_reset(void)
{
    unsigned i;
    memset(&g_room, 0, sizeof(g_room));
    g_room.carries_ble = 1u;
    g_room.carries_ip = 1u;
    for (i = 0u; i < MAX_NODES; ++i) {
        g_room.seed[i] = 0x1234567u + i * 7919u;
        g_peers[i].room = &g_room;
        g_peers[i].index = i;
        g_peers[i].machine = &g_machines[i];
    }
    g_node_count = 0u;
}

static void room_advance(uint32_t step_ms)
{
    unsigned i, j, keep = 0u;

    g_room.now += step_ms;
    for (i = 0u; i < g_room.count; ++i) {
        transmission_t *t = &g_room.tx[i];
        if (t->delivered || g_room.now < t->deliver_at) { continue; }
        t->delivered = 1u;
        for (j = 0u; j < g_node_count; ++j) {
            if (t->from == (uint8_t)j) { continue; }
            (void)mcl_machine_receive(&g_machines[j], t->transport_id,
                                      t->bytes, t->size);
        }
    }
    for (i = 0u; i < g_room.count; ++i) {
        if (g_room.tx[i].delivered) { continue; }
        if (keep != i) { g_room.tx[keep] = g_room.tx[i]; }
        keep++;
    }
    g_room.count = keep;
}

/* The whole integration loop an adopter writes. It is this short on purpose:
   if it were longer, the facade would not be doing its job. */
static void tick(uint32_t step_ms, mcl_machine_event_t *events)
{
    unsigned i;
    for (i = 0u; i < g_node_count; ++i) {
        (void)mcl_machine_poll(&g_machines[i], &events[i]);
        if (events[i].kind == MCL_MACHINE_EVENT_POLICY_REQUIRED) {
            if (g_room.refuse_policy) {
                (void)mcl_machine_refuse(&g_machines[i]);
            } else {
                (void)mcl_machine_admit(&g_machines[i]);
            }
        }
        if (g_room.pending_open && g_room.opens[i] > 0u) {
            /* The integrator's bearer is up now. Idempotent by construction:
               a second call is refused, and the test relies on that. */
            (void)mcl_machine_candidate_ready(&g_machines[i]);
        }
    }
    room_advance(step_ms);
}

/* ------------------------------------------------------------- the cases */

static void test_config_from_deployment(void)
{
    mcl_machine_config_t cfg;

    printf("[machine] the named deployment fills the configuration\n");
    check(mcl_machine_config_deployment(NULL, MCL_DEPLOYMENT_REFERENCE_1,
                                        1u, MCL_CONTACT_ROLE_INITIATOR)
          == MCL_MACHINE_ERR_NULL, "a NULL configuration is refused");

    check(mcl_machine_config_deployment(&cfg, (mcl_deployment_t)99, 1u,
                                        MCL_CONTACT_ROLE_INITIATOR)
          == MCL_MACHINE_ERR_CONFIG, "an unknown deployment is refused");

    /*
     * source_ref 0 is what an uninitialised field looks like. A machine
     * announcing it is indistinguishable from one that forgot to set it, and
     * every peer in range would correlate them all together.
     */
    check(mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_REFERENCE_1, 0u,
                                        MCL_CONTACT_ROLE_INITIATOR)
          == MCL_MACHINE_ERR_CONFIG, "source_ref 0 is refused");

    check(mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_REFERENCE_1,
                                        0xA1B2C3D4u, MCL_CONTACT_ROLE_INITIATOR)
          == MCL_MACHINE_OK, "the reference deployment is accepted");

    /* These four values ARE MCL-REFERENCE-DEPLOYMENT-1. If the published
       profile changes and this does not, check-reference-deployment.sh fails
       the build; this only checks the constant was applied at all. */
    check(cfg.bootstrap_transport_id == MCL_CONTACT_TRANSPORT_AP,
          "bootstrap is the acoustic transport");
    check(cfg.bearer_count == 2u, "two continuation bearers");
    check(cfg.bearer_transport_id[0] == MCL_CONTACT_TRANSPORT_BLE &&
          cfg.bearer_profile_id[0] == 1u,
          "BLE-GATT profile 1 is offered first, as the profile mandates it");
    check(cfg.bearer_transport_id[1] == MCL_CONTACT_TRANSPORT_IP &&
          cfg.bearer_profile_id[1] == 1u,
          "IP-DATAGRAM profile 1 second, which the profile makes optional");
    check(cfg.shared_medium == 1u, "the bootstrap medium is shared");
    check(cfg.deployment_profile_id != NULL &&
          strcmp(cfg.deployment_profile_id, "MCL-REFERENCE-DEPLOYMENT-1") == 0,
          "and the configuration names the profile it came from");
}

static void test_platform_refusals(void)
{
    mcl_machine_config_t cfg;
    mcl_platform_t plat;
    mcl_machine_t m;

    printf("[machine] a platform that cannot honour the deployment is refused\n");
    room_reset();
    (void)mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x11111111u, MCL_CONTACT_ROLE_INITIATOR);

    base_platform(&plat, &g_peers[0], 0);
    plat.clock_ms = NULL;
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG,
          "no clock is refused");

    base_platform(&plat, &g_peers[0], 0);
    plat.transport_send = NULL;
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG,
          "no way to transmit is refused");

    /*
     * The refusal that matters most, because without it the failure is a
     * contact that always stops at agreement and never says why.
     */
    base_platform(&plat, &g_peers[0], 0);
    plat.candidate_open = NULL;
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG,
          "no way to open a candidate bearer is refused");

    base_platform(&plat, &g_peers[0], 0);
    plat.random_bytes = NULL;
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG,
          "a shared-medium deployment without randomness is refused");

    base_platform(&plat, &g_peers[0], 0);
    plat.medium_busy = NULL;
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG,
          "a shared-medium deployment without medium sensing is refused");

    base_platform(&plat, &g_peers[0], 0);
    plat.self_transmitting = NULL;
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_ERR_CONFIG,
          "and one that cannot tell it is hearing itself is refused");

    base_platform(&plat, &g_peers[0], 0);
    check(mcl_machine_init(&m, &cfg, &plat) == MCL_MACHINE_OK,
          "the complete platform is accepted");
    check(mcl_machine_poll(&m, NULL) == MCL_MACHINE_ERR_NULL,
          "poll refuses a NULL event");
    {
        mcl_machine_event_t ev;
        check(mcl_machine_poll(&m, &ev) == MCL_MACHINE_ERR_STATE,
              "and refuses to run before start");
    }
    check(mcl_machine_start(&m) == MCL_MACHINE_OK, "start is accepted");
    check(mcl_machine_start(&m) == MCL_MACHINE_ERR_STATE,
          "start refuses a second time");
}

/*
 * THE CASE THE WHOLE FACADE EXISTS FOR.
 *
 * Two machines, five callbacks each, no protocol knowledge in the loop, and no
 * peer-specific configuration anywhere: no address, no token, no session, no
 * secret. The application waits for one event.
 */
static void run_to_contact(const char *title, int with_policy_callback,
                           int pending_open,
                           int *established_a, int *established_b,
                           int *policy_events)
{
    mcl_machine_config_t ca, cb;
    mcl_platform_t pa, pb;
    mcl_machine_event_t ev[MAX_NODES];
    unsigned i;

    printf("%s\n", title);
    room_reset();
    g_room.pending_open = pending_open;
    g_node_count = 2u;

    base_platform(&pa, &g_peers[0], with_policy_callback);
    base_platform(&pb, &g_peers[1], with_policy_callback);
    (void)mcl_machine_config_deployment(&ca, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x11111111u, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_machine_config_deployment(&cb, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x22222222u, MCL_CONTACT_ROLE_RESPONDER);

    check(mcl_machine_init(&g_machines[0], &ca, &pa) == MCL_MACHINE_OK,
          "A initialises");
    check(mcl_machine_init(&g_machines[1], &cb, &pb) == MCL_MACHINE_OK,
          "B initialises");
    check(mcl_machine_start(&g_machines[0]) == MCL_MACHINE_OK, "A starts");
    check(mcl_machine_start(&g_machines[1]) == MCL_MACHINE_OK, "B starts");

    *established_a = 0;
    *established_b = 0;
    *policy_events = 0;

    for (i = 0u; i < 4000u; ++i) {
        tick(10u, ev);
        if (ev[0].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) {
            *established_a = 1;
        }
        if (ev[1].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) {
            *established_b = 1;
        }
        if (ev[0].kind == MCL_MACHINE_EVENT_POLICY_REQUIRED ||
            ev[1].kind == MCL_MACHINE_EVENT_POLICY_REQUIRED) {
            (*policy_events)++;
        }
    }
}

static void test_two_strangers(void)
{
    int a = 0, b = 0, policy = 0;

    run_to_contact("[machine] two strangers, five callbacks, one event",
                   0, 0, &a, &b, &policy);

    check(a || b, "a contact is ESTABLISHED");
    check(policy > 0,
          "and policy was asked, because admitting a stranger is not MCL's call");

    /*
     * The role assignment nobody chose. `candidate_open` receives a non-zero
     * peer token on exactly one side -- the side that received the offer --
     * and a non-zero local token on the other. That asymmetry IS
     * BLE-ACTIVATE-1's advertiser/scanner assignment, and it arrives at the
     * integrator as data rather than as a rule to remember.
     */
    check(g_room.opens[0] > 0u || g_room.opens[1] > 0u,
          "the integrator was asked to open a candidate bearer");
    {
        const int acceptor_side =
            (g_room.last_open_peer_token[0] != 0u) ? 0 :
            ((g_room.last_open_peer_token[1] != 0u) ? 1 : -1);
        check(acceptor_side >= 0,
              "exactly one side was given a peer endpoint token to reach");
        if (acceptor_side >= 0) {
            const int offerer = 1 - acceptor_side;
            check(g_room.last_open_peer_token[offerer] == 0u,
                  "and the offering side was given none, as the wire carries none");
            check(g_room.last_open_local_token[offerer] != 0u,
                  "the offering side was given ITS OWN token, which it must advertise");
        }
    }
    check(g_room.last_open_transport[0] == MCL_CONTACT_TRANSPORT_BLE ||
          g_room.last_open_transport[1] == MCL_CONTACT_TRANSPORT_BLE,
          "the bearer opened is the one the deployment mandates first");
}

static void test_pending_candidate(void)
{
    int a = 0, b = 0, policy = 0;

    run_to_contact("[machine] a bearer that takes time to open still reaches contact",
                   0, 1, &a, &b, &policy);
    check(a || b,
          "CANDIDATE_PENDING then mcl_machine_candidate_ready() reaches a contact");
}

static void test_policy_callback_and_refusal(void)
{
    int a = 0, b = 0, policy = 0;

    run_to_contact("[machine] a policy callback answers without an event",
                   1, 0, &a, &b, &policy);
    check(a || b, "a contact is established through the callback");
    check(policy == 0,
          "and POLICY_REQUIRED is never raised when the platform answers it");
    check(g_room.policy_calls[0] + g_room.policy_calls[1] > 0u,
          "the callback was actually consulted");

    room_reset();
    g_room.refuse_policy = 1;
    {
        mcl_machine_config_t ca, cb;
        mcl_platform_t pa, pb;
        mcl_machine_event_t ev[MAX_NODES];
        unsigned i;
        int established = 0, lost = 0;

        printf("[machine] a refusal is a refusal, not a delayed acceptance\n");
        g_node_count = 2u;
        base_platform(&pa, &g_peers[0], 1);
        base_platform(&pb, &g_peers[1], 1);
        (void)mcl_machine_config_deployment(&ca, MCL_DEPLOYMENT_REFERENCE_1,
                                            0x33333333u, MCL_CONTACT_ROLE_INITIATOR);
        (void)mcl_machine_config_deployment(&cb, MCL_DEPLOYMENT_REFERENCE_1,
                                            0x44444444u, MCL_CONTACT_ROLE_RESPONDER);
        (void)mcl_machine_init(&g_machines[0], &ca, &pa);
        (void)mcl_machine_init(&g_machines[1], &cb, &pb);
        (void)mcl_machine_start(&g_machines[0]);
        (void)mcl_machine_start(&g_machines[1]);

        for (i = 0u; i < 4000u; ++i) {
            tick(10u, ev);
            if (ev[0].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED ||
                ev[1].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) {
                established = 1;
            }
            if (ev[0].kind == MCL_MACHINE_EVENT_CONTACT_LOST ||
                ev[1].kind == MCL_MACHINE_EVENT_CONTACT_LOST) {
                lost = 1;
            }
        }
        check(!established,
              "a refused stranger does not become a contact anyway");
        check(lost, "and the refusal is reported rather than left silent");
    }
}

/*
 * TWO MACHINES THAT CAN HEAR EACH OTHER AND SHARE NO BEARER.
 *
 * The first version of this case unplugged the bearers in the ROOM, which
 * proves nothing: OFFER and ACCEPT travel on the bootstrap medium, so both
 * machines still agreed on a bearer neither could use, and the failure arrived
 * later and by another name. The disagreement has to be between the two
 * CONFIGURATIONS -- which is the real deployment error this event is for: a
 * machine brought into a site whose profile it does not implement.
 *
 * The correct outcome is a reportable fact, not a silence. An operator whose
 * machines can hear each other and cannot reach each other needs telling.
 */
static void test_no_common_bearer(void)
{
    mcl_machine_config_t ca, cb;
    mcl_platform_t pa, pb;
    mcl_machine_event_t ev[MAX_NODES];
    unsigned i;
    int exhausted = 0, established = 0;

    printf("[machine] two machines with no bearer in common say so\n");
    room_reset();
    g_node_count = 2u;

    base_platform(&pa, &g_peers[0], 1);
    base_platform(&pb, &g_peers[1], 1);
    (void)mcl_machine_config_deployment(&ca, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x55555555u, MCL_CONTACT_ROLE_INITIATOR);

    /* B is configured by hand for a deployment that mandates a bearer A does
       not have. Hand configuration is a supported case: a site that is not one
       of the named deployments still has to be expressible. */
    (void)mcl_machine_config_deployment(&cb, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x66666666u, MCL_CONTACT_ROLE_RESPONDER);
    cb.bearer_count = 1u;
    cb.bearer_transport_id[0] = MCL_CONTACT_TRANSPORT_UWB;
    cb.bearer_profile_id[0] = 1u;
    cb.deployment_profile_id = "test-only, no shared bearer";

    (void)mcl_machine_init(&g_machines[0], &ca, &pa);
    (void)mcl_machine_init(&g_machines[1], &cb, &pb);
    (void)mcl_machine_start(&g_machines[0]);
    (void)mcl_machine_start(&g_machines[1]);

    for (i = 0u; i < 8000u; ++i) {
        tick(10u, ev);
        if (ev[0].kind == MCL_MACHINE_EVENT_NO_COMMON_BEARER ||
            ev[1].kind == MCL_MACHINE_EVENT_NO_COMMON_BEARER) {
            exhausted = 1;
        }
        if (ev[0].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED ||
            ev[1].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) {
            established = 1;
        }
    }
    check(exhausted,
          "NO_COMMON_BEARER is raised when every mandated bearer is refused");
    check(!established,
          "and no contact is claimed on a bearer that was never agreed");
}

static void test_candidate_refused(void)
{
    mcl_machine_config_t ca, cb;
    mcl_platform_t pa, pb;
    mcl_machine_event_t ev[MAX_NODES];
    unsigned i;
    int errored = 0, established = 0;

    printf("[machine] a bearer the machine cannot open is an error, not a contact\n");
    room_reset();
    g_room.refuse_open = 1;
    g_node_count = 2u;

    base_platform(&pa, &g_peers[0], 1);
    base_platform(&pb, &g_peers[1], 1);
    (void)mcl_machine_config_deployment(&ca, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x77777777u, MCL_CONTACT_ROLE_INITIATOR);
    (void)mcl_machine_config_deployment(&cb, MCL_DEPLOYMENT_REFERENCE_1,
                                        0x88888888u, MCL_CONTACT_ROLE_RESPONDER);
    (void)mcl_machine_init(&g_machines[0], &ca, &pa);
    (void)mcl_machine_init(&g_machines[1], &cb, &pb);
    (void)mcl_machine_start(&g_machines[0]);
    (void)mcl_machine_start(&g_machines[1]);

    for (i = 0u; i < 3000u; ++i) {
        tick(10u, ev);
        if (ev[0].kind == MCL_MACHINE_EVENT_ERROR ||
            ev[1].kind == MCL_MACHINE_EVENT_ERROR) {
            errored = 1;
        }
        if (ev[0].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED ||
            ev[1].kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) {
            established = 1;
        }
    }
    check(errored, "the refusal reaches the caller as an ERROR");
    check(!established, "and no contact is claimed");
}

static void test_names(void)
{
    printf("[machine] names exist for a log to print\n");
    check(strcmp(mcl_machine_event_name(MCL_MACHINE_EVENT_CONTACT_ESTABLISHED),
                 "CONTACT_ESTABLISHED") == 0, "events have names");
    check(strcmp(mcl_machine_state_name(NULL), "NULL") == 0,
          "and a NULL machine does not crash a log line");
}

int main(void)
{
    printf("=== MCL integration facade ===\n");
    test_config_from_deployment();
    test_platform_refusals();
    test_two_strangers();
    test_pending_candidate();
    test_policy_callback_and_refusal();
    test_no_common_bearer();
    test_candidate_refused();
    test_names();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

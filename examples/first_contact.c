/*
 * FIRST CONTACT, END TO END, ON ONE MACHINE.
 *
 * Two machines that were told nothing about each other find one another, agree
 * on a bearer, prove it reaches, ask their own policy, migrate, and report a
 * contact. It prints every step.
 *
 * WHAT THIS IS
 *
 * The whole `MCL-REFERENCE-DEPLOYMENT-1` sequence, driven through
 * `mcl/machine.h` and nothing else. It is the shortest complete answer to
 * "what does integrating MCL actually look like", and it is 60 lines of
 * platform code:
 *
 *     a clock, randomness, a way to move bytes, a way to open a bearer,
 *     and a policy answer.
 *
 * WHAT THIS IS NOT
 *
 * Two devices. Both machines run in this process, on a simulated medium and a
 * simulated clock, so nothing here establishes anything physical: no acoustic
 * path, no radio, no timing margin, no interoperability. It is a demonstration
 * of the API and the sequence, and the sequence is real -- every frame below is
 * the same canonical Wire and Link byte stream two boards exchange.
 *
 * For the same sequence between real machines with real radios, see
 * `hardware/dfr1154-autonomous-node/`.
 *
 * Built by the SDK's own CMake as `mcl_sdk_first_contact`.
 */

#include "mcl/machine.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------- a simulated room */

#define MACHINES 2
#define MAX_INFLIGHT 16
#define MAX_FRAME 128

typedef struct {
    uint8_t transport_id;
    uint8_t bytes[MAX_FRAME];
    size_t size;
    int from;
    uint32_t deliver_at_ms;
    int delivered;
} message_t;

typedef struct {
    uint32_t now_ms;
    message_t inflight[MAX_INFLIGHT];
    unsigned count;
    uint32_t seed[MACHINES];
} room_t;

typedef struct {
    room_t *room;
    int index;
    const char *name;
    /* What this machine's "radio" is doing. A real integration has a socket or
       a GATT connection here; this has a boolean. */
    int bearer_open;
} machine_ctx_t;

static room_t g_room;
static machine_ctx_t g_ctx[MACHINES];
static mcl_machine_t g_machine[MACHINES];

/* --------------------------------------------------- the platform: 5 + 2 */

static uint32_t plat_clock_ms(void *user)
{
    machine_ctx_t *c = (machine_ctx_t *)user;
    return c->room->now_ms;
}

static int plat_random_bytes(void *user, uint8_t *out, size_t size)
{
    machine_ctx_t *c = (machine_ctx_t *)user;
    size_t i;
    for (i = 0u; i < size; ++i) {
        c->room->seed[c->index] =
            c->room->seed[c->index] * 1664525u + 1013904223u;
        out[i] = (uint8_t)(c->room->seed[c->index] >> 24);
    }
    return 0;
}

/*
 * Return 0 for sent, negative for DEFINITELY not sent, positive for "cannot
 * tell". Do not collapse the last two: MCL uses the difference to decide
 * whether a COMMIT it just sent was irrevocable.
 */
static int32_t plat_transport_send(void *user, uint8_t transport_id,
                                   const uint8_t *data, size_t size)
{
    machine_ctx_t *c = (machine_ctx_t *)user;
    room_t *room = c->room;
    message_t *m;

    if (size > MAX_FRAME || room->count >= MAX_INFLIGHT) {
        return -1;
    }
    m = &room->inflight[room->count++];
    memset(m, 0, sizeof(*m));
    m->transport_id = transport_id;
    memcpy(m->bytes, data, size);
    m->size = size;
    m->from = c->index;
    m->deliver_at_ms = room->now_ms + 10u;
    return 0;
}

/* On a shared medium: is somebody transmitting, and is it me? A real machine
   answers from its receiver and its own transmit state. */
static int plat_medium_busy(void *user) { (void)user; return 0; }
static int plat_self_transmitting(void *user) { (void)user; return 0; }

/*
 * Open the agreed bearer. The one thing MCL cannot do for a machine.
 *
 * Nothing here chooses a role. `peer_endpoint_token` is non-zero only for the
 * peer that received a TRANSPORT_OFFER -- the only object that carries one --
 * so the peer that can be found advertises and the peer holding the token goes
 * and finds it. On BLE that is exactly BLE-ACTIVATE-1's advertiser/scanner
 * assignment, arriving as data rather than as a rule to remember.
 */
static mcl_machine_candidate_t plat_candidate_open(void *user,
                                                   uint8_t transport_id,
                                                   uint8_t profile_id,
                                                   uint32_t peer_endpoint_token,
                                                   uint32_t local_endpoint_token)
{
    machine_ctx_t *c = (machine_ctx_t *)user;
    (void)profile_id;

    if (peer_endpoint_token != 0u) {
        printf("  %s  open bearer %u: scanning for the peer's token %08lX\n",
               c->name, (unsigned)transport_id,
               (unsigned long)peer_endpoint_token);
    } else {
        printf("  %s  open bearer %u: advertising my own token %08lX\n",
               c->name, (unsigned)transport_id,
               (unsigned long)local_endpoint_token);
    }
    c->bearer_open = 1;
    /* A real central would answer PENDING here and call
       mcl_machine_candidate_ready() once connected. */
    return MCL_MACHINE_CANDIDATE_READY;
}

static void plat_candidate_close(void *user, uint8_t transport_id)
{
    machine_ctx_t *c = (machine_ctx_t *)user;
    (void)transport_id;
    c->bearer_open = 0;
}

/*
 * Local admission policy. MCL will not take this decision: reception is not
 * identity, is not authority, and is not obligation. A machine that hears a
 * stranger and declines has behaved correctly.
 */
static int plat_policy_admit(void *user, uint32_t peer_ref, uint8_t transport_id)
{
    machine_ctx_t *c = (machine_ctx_t *)user;
    (void)transport_id;
    printf("  %s  policy: admit peer %08lX? yes\n", c->name,
           (unsigned long)peer_ref);
    return 1;
}

/* -------------------------------------------------------------- the room */

static void room_advance(uint32_t step_ms)
{
    unsigned i, j, keep = 0u;

    g_room.now_ms += step_ms;
    for (i = 0u; i < g_room.count; ++i) {
        message_t *m = &g_room.inflight[i];
        if (m->delivered || g_room.now_ms < m->deliver_at_ms) { continue; }
        m->delivered = 1;
        for (j = 0u; j < MACHINES; ++j) {
            if (m->from == (int)j) { continue; }
            (void)mcl_machine_receive(&g_machine[j], m->transport_id,
                                      m->bytes, m->size);
        }
    }
    for (i = 0u; i < g_room.count; ++i) {
        if (g_room.inflight[i].delivered) { continue; }
        if (keep != i) { g_room.inflight[keep] = g_room.inflight[i]; }
        keep++;
    }
    g_room.count = keep;
}

int main(void)
{
    mcl_machine_config_t config;
    mcl_platform_t platform;
    mcl_machine_event_t event;
    unsigned tick;
    int established[MACHINES] = {0, 0};
    int both = 0;
    int i;

    static const char *names[MACHINES] = { "A", "B" };
    /* Correlation references, not identities. Randomise these in a product:
       two machines that share one are not wrong, only harder to tell apart. */
    static const uint32_t source_refs[MACHINES] = { 0xA1A1A1A1u, 0xB2B2B2B2u };
    static const mcl_contact_role_t roles[MACHINES] = {
        MCL_CONTACT_ROLE_INITIATOR, MCL_CONTACT_ROLE_RESPONDER
    };

    memset(&g_room, 0, sizeof(g_room));
    for (i = 0; i < MACHINES; ++i) {
        g_room.seed[i] = 0x1234567u + (uint32_t)i * 7919u;
        g_ctx[i].room = &g_room;
        g_ctx[i].index = i;
        g_ctx[i].name = names[i];
        g_ctx[i].bearer_open = 0;

        memset(&platform, 0, sizeof(platform));
        platform.clock_ms = plat_clock_ms;
        platform.random_bytes = plat_random_bytes;
        platform.transport_send = plat_transport_send;
        platform.medium_busy = plat_medium_busy;
        platform.self_transmitting = plat_self_transmitting;
        platform.candidate_open = plat_candidate_open;
        platform.candidate_close = plat_candidate_close;
        platform.policy_admit = plat_policy_admit;
        platform.user = &g_ctx[i];

        /* One named deployment. A builder should not have to choose among
           equivalent combinations before seeing MCL work once, and two
           builders who chose differently would never meet. */
        if (mcl_machine_config_deployment(&config, MCL_DEPLOYMENT_REFERENCE_1,
                                          source_refs[i], roles[i])
            != MCL_MACHINE_OK) {
            printf("configuration refused\n");
            return 1;
        }
        if (mcl_machine_init(&g_machine[i], &config, &platform)
            != MCL_MACHINE_OK) {
            printf("machine %s refused the platform\n", names[i]);
            return 1;
        }
        if (mcl_machine_start(&g_machine[i]) != MCL_MACHINE_OK) {
            printf("machine %s would not start\n", names[i]);
            return 1;
        }
    }

    printf("MCL first contact, %s\n", config.deployment_profile_id);
    printf("Two machines, no shared configuration, no peer addresses.\n\n");

    for (tick = 0u; tick < 4000u && !both; ++tick) {
        for (i = 0; i < MACHINES; ++i) {
            if (mcl_machine_poll(&g_machine[i], &event) != MCL_MACHINE_OK) {
                continue;
            }
            switch (event.kind) {
            case MCL_MACHINE_EVENT_NONE:
                break;
            case MCL_MACHINE_EVENT_PEER_DETECTED:
                printf("  %s  heard a peer, correlation %08lX\n",
                       names[i], (unsigned long)event.peer_ref);
                break;
            case MCL_MACHINE_EVENT_CONTACT_ESTABLISHED:
                printf("  %s  CONTACT ESTABLISHED on transport %u, "
                       "profile %u, session %08lX\n",
                       names[i], (unsigned)event.transport_id,
                       (unsigned)event.profile_id,
                       (unsigned long)event.session_ref);
                established[i] = 1;
                break;
            case MCL_MACHINE_EVENT_NO_COMMON_BEARER:
                printf("  %s  no bearer in common with that peer\n", names[i]);
                break;
            case MCL_MACHINE_EVENT_CONTACT_LOST:
                printf("  %s  contact lost\n", names[i]);
                break;
            case MCL_MACHINE_EVENT_ERROR:
                printf("  %s  error, status %d\n", names[i], (int)event.status);
                break;
            default:
                break;
            }
        }
        both = established[0] && established[1];
        room_advance(10u);
    }

    printf("\n");
    if (!both) {
        printf("Only %d of %d machines reported a contact after %u simulated "
               "seconds.\n", established[0] + established[1], MACHINES,
               (unsigned)(g_room.now_ms / 1000u));
        return 1;
    }
    printf("A is %s, B is %s, at t+%u.%03us.\n",
           mcl_machine_state_name(&g_machine[0]),
           mcl_machine_state_name(&g_machine[1]),
           (unsigned)(g_room.now_ms / 1000u),
           (unsigned)(g_room.now_ms % 1000u));
    printf("\n");
    printf("What just happened, and what it is worth:\n");
    printf("  PRESENCE -> contention -> OFFER/ACCEPT -> the bearer was opened\n");
    printf("  -> PATH_CHALLENGE/RESPONSE proved it reaches -> policy admitted\n");
    printf("  -> COMMIT/CONFIRM migrated the contact.\n");
    printf("\n");
    printf("  Established: REACHABILITY and CORRELATION.\n");
    printf("  NOT established: identity, authenticity, authority or trust.\n");
    printf("  MCL v1 has no cryptography. Anything in range can do what B did.\n");
    return 0;
}

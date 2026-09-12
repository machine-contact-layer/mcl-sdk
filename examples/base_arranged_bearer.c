/*
 * MCL Base 1 on a bearer both machines already share.
 *
 * This is the ORDINARY case, and it is a first-class claim rather than a
 * degraded one: a fleet provisioned by one owner, machines paired at
 * manufacture, a bearer arranged out of band. Section 4 of
 * mcl-core/spec/conformance-profiles-v1.md states it exactly:
 *
 *     given a bearer both machines already share, two `MCL Base 1`
 *     implementations interoperate.
 *
 * What that requires, and what this example therefore shows:
 *
 *   - Wire major 1, encoded and decoded
 *   - the Stable Tier-0 kernel, produced and consumed, at major 1
 *   - Link major 1 framing with the frozen class dispositions
 *   - refusal semantics: an inadmissible object is refused, never guessed
 *   - one transport binding, here an in-process arranged bearer
 *
 * What it deliberately does NOT show, because Base 1 does not have it:
 *
 *   - discovery of any kind. Nothing is looked for; the bearer is given.
 *   - candidate_open. No candidate is opened because none is proposed.
 *   - a microphone, a speaker, or a bootstrap medium.
 *   - an application payload channel. Base 1 carries contact and control
 *     objects. It is not a general message bus and this example does not
 *     pretend otherwise.
 *
 * The bearer here is two in-memory queues and a counter for a clock. The
 * SEQUENCE is real -- every byte is the canonical Wire and Link stream two
 * boards exchange -- but nothing physical is established: no radio, no timing
 * margin, no interoperability with anyone else's implementation.
 */

#include "mcl/machine.h"
#include "mcl/sdk.h"

#include <stdio.h>
#include <string.h>

#define BEARER_IP   MCL_CONTACT_TRANSPORT_IP
#define INBOX_MAX   4
#define FRAME_MAX   MCL_LINK_FRAME_MAX_SIZE

typedef struct {
    uint8_t frame[INBOX_MAX][FRAME_MAX];
    size_t  len[INBOX_MAX];
    unsigned count;
} inbox_t;

typedef struct {
    const char *name;
    inbox_t *peer_inbox;      /* where this machine's bytes land */
    uint32_t *clock;
} bearer_t;

static uint32_t bearer_clock(void *user)
{
    bearer_t *b = (bearer_t *)user;
    *b->clock += 1u;
    return *b->clock;
}

/*
 * Deliver into the peer's queue. Returns 0 for "certainly sent", which an
 * in-process queue can honestly claim; a real radio that cannot tell would
 * return > 0 instead of lying.
 */
static int32_t bearer_send(void *user, uint8_t transport_id,
                           const uint8_t *data, size_t size)
{
    bearer_t *b = (bearer_t *)user;
    inbox_t *in = b->peer_inbox;

    if (transport_id != BEARER_IP || size > FRAME_MAX ||
        in->count >= INBOX_MAX) {
        return -1;
    }
    memcpy(in->frame[in->count], data, size);
    in->len[in->count] = size;
    in->count += 1u;
    printf("  %-2s -> bearer %u: %zu bytes\n", b->name, transport_id, size);
    return 0;
}

/* Hand everything queued for the machine to it, then empty the queue. */
static void drain(mcl_machine_t *m, inbox_t *in)
{
    unsigned i;
    for (i = 0u; i < in->count; ++i) {
        (void)mcl_machine_receive(m, BEARER_IP, in->frame[i], in->len[i]);
    }
    in->count = 0u;
}

int main(void)
{
    mcl_machine_t a, b;
    mcl_machine_config_t cfg_a, cfg_b;
    mcl_platform_t plat_a, plat_b;
    inbox_t inbox_a, inbox_b;
    bearer_t bear_a, bear_b;
    uint32_t clock = 0u;
    unsigned round;
    int a_up = 0, b_up = 0;

    memset(&inbox_a, 0, sizeof(inbox_a));
    memset(&inbox_b, 0, sizeof(inbox_b));

    bear_a.name = "A"; bear_a.peer_inbox = &inbox_b; bear_a.clock = &clock;
    bear_b.name = "B"; bear_b.peer_inbox = &inbox_a; bear_b.clock = &clock;

    puts("MCL Base 1, MCL-BASE-DEPLOYMENT-1");
    puts("Two machines on a bearer that is already there. No rendezvous.\n");

    /*
     * The named deployment fills everything except who this machine is. Note
     * what is NOT set on the platform below: no candidate_open, no randomness,
     * no medium sensing. Base 1 needs none of them, and mcl_machine_init()
     * accepts their absence for an arranged bearer.
     */
    if (mcl_machine_config_deployment(&cfg_a, MCL_DEPLOYMENT_BASE_ARRANGED_1,
                                      0xA1A1A1A1u,
                                      MCL_CONTACT_ROLE_INITIATOR)
        != MCL_MACHINE_OK) {
        fputs("config A failed\n", stderr);
        return 1;
    }
    if (mcl_machine_config_deployment(&cfg_b, MCL_DEPLOYMENT_BASE_ARRANGED_1,
                                      0xB2B2B2B2u,
                                      MCL_CONTACT_ROLE_RESPONDER)
        != MCL_MACHINE_OK) {
        fputs("config B failed\n", stderr);
        return 1;
    }
    printf("  profile        %s\n", cfg_a.deployment_profile_id);
    printf("  wire major     %u  (Stable)\n", (unsigned)cfg_a.wire_major);
    printf("  bearer         transport %u, profile %u\n\n",
           (unsigned)cfg_a.bearer_transport_id[0],
           (unsigned)cfg_a.bearer_profile_id[0]);

    memset(&plat_a, 0, sizeof(plat_a));
    plat_a.clock_ms = bearer_clock;
    plat_a.transport_send = bearer_send;
    plat_a.user = &bear_a;

    memset(&plat_b, 0, sizeof(plat_b));
    plat_b.clock_ms = bearer_clock;
    plat_b.transport_send = bearer_send;
    plat_b.user = &bear_b;

    if (mcl_machine_init(&a, &cfg_a, &plat_a) != MCL_MACHINE_OK ||
        mcl_machine_init(&b, &cfg_b, &plat_b) != MCL_MACHINE_OK) {
        fputs("init failed\n", stderr);
        return 1;
    }

    /* Each announces itself on the bearer. This is the whole of contact. */
    if (mcl_machine_start(&a) != MCL_MACHINE_OK ||
        mcl_machine_start(&b) != MCL_MACHINE_OK) {
        fputs("start failed\n", stderr);
        return 1;
    }

    for (round = 0u; round < 8u && !(a_up && b_up); ++round) {
        mcl_machine_event_t ev;

        drain(&a, &inbox_a);
        drain(&b, &inbox_b);

        if (mcl_machine_poll(&a, &ev) == MCL_MACHINE_OK &&
            ev.kind != MCL_MACHINE_EVENT_NONE) {
            printf("  A  %s", mcl_machine_event_name(ev.kind));
            if (ev.peer_ref != 0u) { printf(", peer %08X", ev.peer_ref); }
            putchar('\n');
            if (ev.kind == MCL_MACHINE_EVENT_POLICY_REQUIRED) {
                (void)mcl_machine_admit(&a);
            }
            if (ev.kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) { a_up = 1; }
        }
        if (mcl_machine_poll(&b, &ev) == MCL_MACHINE_OK &&
            ev.kind != MCL_MACHINE_EVENT_NONE) {
            printf("  B  %s", mcl_machine_event_name(ev.kind));
            if (ev.peer_ref != 0u) { printf(", peer %08X", ev.peer_ref); }
            putchar('\n');
            if (ev.kind == MCL_MACHINE_EVENT_POLICY_REQUIRED) {
                (void)mcl_machine_admit(&b);
            }
            if (ev.kind == MCL_MACHINE_EVENT_CONTACT_ESTABLISHED) { b_up = 1; }
        }
    }

    if (!a_up || !b_up) {
        fputs("\ncontact was not established\n", stderr);
        return 1;
    }

    /*
     * Contact is up. A Base 1 machine exchanges Stable Tier-0 objects through
     * the node underneath, at the Stable pair. There is deliberately no
     * "send my application bytes" call: Base 1 is contact and control.
     */
    puts("\n  contact established on both sides; exchanging a Stable object");
    {
        uint8_t scratch[FRAME_MAX];
        mcl_wire_tier0_t obj;
        mcl_link_frame_t frame;
        mcl_wire_tier0_t got;
        uint8_t has_object = 0u;
        size_t consumed = 0u;
        size_t sent = 0u;

        memset(&obj, 0, sizeof(obj));
        obj.kind = MCL_WIRE_KIND_PRESENCE;
        obj.priority = 1u;
        obj.source_ref = cfg_a.source_ref;
        obj.body.presence.capability_tag = 0x00BEEFu;
        obj.body.presence.ttl = 30u;

        if (mcl_node_send_framed_tier0_at_major(
                mcl_machine_node(&a), &obj,
                MCL_WIRE_STABLE_MAJOR, MCL_LINK_STABLE_MAJOR,
                MCL_LINK_CLASS_DATA, 0u,
                scratch, sizeof(scratch), &sent) != MCL_SDK_OK) {
            fputs("stable send failed\n", stderr);
            return 1;
        }
        printf("  A  sent PRESENCE, Wire 1 in Link 1, %zu bytes\n", sent);

        if (inbox_b.count == 0u) {
            fputs("nothing arrived at B\n", stderr);
            return 1;
        }
        if (mcl_node_receive_framed(mcl_machine_node(&b), BEARER_IP,
                                    inbox_b.frame[0], inbox_b.len[0],
                                    &frame, &got, &has_object, &consumed)
                != MCL_SDK_OK || has_object == 0u) {
            fputs("B could not decode the Stable frame\n", stderr);
            return 1;
        }
        inbox_b.count = 0u;
        printf("  B  decoded PRESENCE, capability_tag %06X\n",
               got.body.presence.capability_tag);

        /*
         * Refusal is part of the profile, not an error path bolted on. HAZARD
         * is Candidate: its layout exists and its vectors pass, but its
         * MEANING may still change, so the Stable major does not carry it. A
         * Base 1 machine refuses it rather than quietly emitting it at a major
         * that does.
         */
        memset(&obj, 0, sizeof(obj));
        obj.kind = MCL_WIRE_KIND_HAZARD;
        obj.priority = 3u;
        obj.source_ref = cfg_a.source_ref;
        if (mcl_node_send_framed_tier0_at_major(
                mcl_machine_node(&a), &obj,
                MCL_WIRE_STABLE_MAJOR, MCL_LINK_STABLE_MAJOR,
                MCL_LINK_CLASS_DATA, 0u,
                scratch, sizeof(scratch), &sent) == MCL_SDK_OK) {
            fputs("a Candidate object was accepted at the Stable major\n",
                  stderr);
            return 1;
        }
        puts("  A  HAZARD at Wire 1 refused, as a Candidate object must be");
    }

    puts("\nMCL Base 1: OK");
    return 0;
}

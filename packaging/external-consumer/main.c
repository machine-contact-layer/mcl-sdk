/*
 * A clean external consumer of the product-facing MCL API.
 *
 * This file deliberately includes only mcl/machine.h and names no Wire,
 * Link, rendezvous, handoff, migration or frame-construction function. The
 * package gate builds it against an install prefix with no source-tree path.
 * Low-level APIs remain public for independent implementations; they are not
 * the normal OEM integration surface.
 */

#include "mcl/machine.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t now_ms;
    uint32_t rng;
    unsigned sends;
    uint8_t last_transport;
} platform_state_t;

static int checks;
static int failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static uint32_t platform_clock(void *user)
{
    return ((platform_state_t *)user)->now_ms;
}

static int platform_random(void *user, uint8_t *out, size_t size)
{
    platform_state_t *state = (platform_state_t *)user;
    size_t i;
    for (i = 0u; i < size; ++i) {
        state->rng = state->rng * 1664525u + 1013904223u;
        out[i] = (uint8_t)(state->rng >> 24);
    }
    return 0;
}

static int32_t platform_send(void *user, uint8_t transport_id,
                             const uint8_t *data, size_t size)
{
    platform_state_t *state = (platform_state_t *)user;
    if (data == NULL || size == 0u) {
        return -1;
    }
    state->sends++;
    state->last_transport = transport_id;
    return 0;
}

static int platform_medium_busy(void *user)
{
    (void)user;
    return 0;
}

static int platform_self_transmitting(void *user)
{
    (void)user;
    return 0;
}

static mcl_machine_candidate_t platform_candidate_open(
    void *user, uint8_t transport_id, uint8_t profile_id,
    uint32_t peer_endpoint_token, uint32_t local_endpoint_token)
{
    (void)user;
    (void)transport_id;
    (void)profile_id;
    (void)peer_endpoint_token;
    (void)local_endpoint_token;
    return MCL_MACHINE_CANDIDATE_READY;
}

int main(void)
{
    mcl_machine_config_t config;
    mcl_platform_v1_t platform;
    mcl_machine_t machine;
    mcl_machine_event_t event;
    platform_state_t state;
    unsigned tick;

    memset(&state, 0, sizeof(state));
    state.rng = 0x12345678u;
    memset(&platform, 0, sizeof(platform));
    platform.clock_ms = platform_clock;
    platform.random_bytes = platform_random;
    platform.transport_send = platform_send;
    platform.medium_busy = platform_medium_busy;
    platform.self_transmitting = platform_self_transmitting;
    platform.candidate_open = platform_candidate_open;
    platform.user = &state;

    check(mcl_machine_config_deployment(&config,
                                        MCL_DEPLOYMENT_REFERENCE_1,
                                        0x10203040u,
                                        MCL_CONTACT_ROLE_INITIATOR)
              == MCL_MACHINE_OK,
          "the named deployment configures");
    check(mcl_machine_init(&machine, &config, &platform) == MCL_MACHINE_OK,
          "the machine accepts the v1 platform");
    check(mcl_machine_start(&machine) == MCL_MACHINE_OK,
          "first contact starts without peer configuration");

    for (tick = 0u; tick < 1000u && state.sends == 0u; ++tick) {
        check(mcl_machine_poll(&machine, &event) == MCL_MACHINE_OK,
              "the high-level machine poll succeeds");
        state.now_ms += 10u;
    }
    check(state.sends > 0u, "MCL owns and emits the first announcement");
    check(state.last_transport == MCL_CONTACT_TRANSPORT_AP,
          "the named deployment selected its bootstrap bearer");

    printf("%d checks, %d failed\n", checks, failures);
    if (failures != 0) {
        puts("EXTERNAL MACHINE CONSUMER FAILED");
        return 1;
    }
    puts("EXTERNAL MACHINE CONSUMER OK");
    puts("Manual Wire/Link/rendezvous/migration code: 0");
    return 0;
}

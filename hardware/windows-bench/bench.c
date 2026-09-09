/* Host-only ABI bridge. All protocol, modulation and fragmentation are the
   canonical implementations. One calling thread owns each context. */
#include <stdlib.h>
#include <string.h>
#include "mcl/machine.h"
#include "mcl/ap_listen.h"
#include "mcl/ble_binding.h"
#define API __declspec(dllexport)
typedef struct {
    mcl_machine_t machine;
    mcl_ap_listener_t listener;
    mcl_ap_modem_scratch_t scratch;
    mcl_ble_reassembler_t fragments;
    int16_t window[96000];
} bench_t;
API void *bench_create(const mcl_platform_t *platform, uint32_t source, int role)
{
    bench_t *b = (bench_t *)calloc(1u, sizeof(*b));
    mcl_machine_config_t cfg;
    mcl_ap_listen_config_t audio;
    if (b == NULL) { return NULL; }
    mcl_ap_listen_default_config(&audio);
    audio.max_payload_bytes = 17u;
    if (mcl_ap_listen_min_window_samples(&audio) > 96000u ||
        mcl_ap_listen_init(&b->listener, &audio, b->window, 96000u) != MCL_AP_LISTEN_OK ||
        mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_REFERENCE_1, source,
            (mcl_contact_role_t)role) != MCL_MACHINE_OK ||
        mcl_machine_init(&b->machine, &cfg, platform) != MCL_MACHINE_OK) {
        free(b); return NULL;
    }
    return b;
}
API void bench_destroy(bench_t *b) { free(b); }
API int bench_start(bench_t *b) { return (int)mcl_machine_start(&b->machine); }
API int bench_poll(bench_t *b, uint32_t out[6])
{
    mcl_machine_event_t e = {0};
    int rc = (int)mcl_machine_poll(&b->machine, &e);
    out[0] = (uint32_t)e.kind; out[1] = e.transport_id; out[2] = e.profile_id;
    out[3] = e.peer_ref; out[4] = e.session_ref; out[5] = (uint32_t)e.status;
    return rc;
}
API int bench_ready(bench_t *b) { return (int)mcl_machine_candidate_ready(&b->machine); }
API int bench_refused(bench_t *b) { return (int)mcl_machine_candidate_refused(&b->machine); }
API int bench_admit(bench_t *b) { return (int)mcl_machine_admit(&b->machine); }
API const char *bench_state(bench_t *b) { return mcl_machine_state_name(&b->machine); }
API int bench_reset_audio(bench_t *b)
{
    mcl_ap_listen_config_t cfg;
    mcl_ap_listen_default_config(&cfg);
    cfg.max_payload_bytes = 17u;
    return (int)mcl_ap_listen_init(&b->listener, &cfg, b->window, 96000u);
}
API int bench_receive(bench_t *b, int transport, const uint8_t *data, size_t size)
{ return (int)mcl_machine_receive(&b->machine, (uint8_t)transport, data, size); }
API int bench_audio(bench_t *b, const int16_t *pcm, size_t count, uint8_t out[17], uint32_t info[3])
{
    mcl_ap_listen_event_t e;
    int result;
    if (mcl_ap_listen_push(&b->listener, pcm, count) != MCL_AP_LISTEN_OK) { return -1; }
    memset(&e, 0, sizeof(e));
    result = (int)mcl_ap_listen_poll(&b->listener, &b->scratch, out, 17u, &e);
    info[0] = (uint32_t)e.payload_bytes;
    info[1] = (uint32_t)b->listener.samples_unscanned;
    info[2] = (uint32_t)b->listener.contacts;
    return result;
}
API int bench_modulate(const uint8_t *data, size_t size, int16_t *pcm, size_t capacity)
{
    mcl_ap_modem_config_t cfg;
    size_t used = 0u;
    mcl_ap_modem_default_config(&cfg);
    if (mcl_ap_modem_encode(&cfg, data, size, pcm, capacity, &used) != MCL_AP_MODEM_OK) { return -1; }
    return (int)used;
}
API int bench_fragment(const uint8_t *data, size_t size, size_t index, uint8_t out[20])
{
    size_t used = 0u;
    if (mcl_ble_fragment(data, size, 23u, index, out, 20u, &used) != MCL_BLE_OK) { return -1; }
    return (int)used;
}
API void bench_reset_fragments(bench_t *b) { mcl_ble_reassembler_reset(&b->fragments); }
API int bench_reassemble(bench_t *b, const uint8_t *data, size_t size, uint8_t *out)
{
    size_t used = 0u;
    mcl_ble_status_t rc = mcl_ble_reassemble(&b->fragments, data, size, &used);
    if (rc == MCL_BLE_ERR_INCOMPLETE) { return 0; }
    if (rc != MCL_BLE_OK) { return -1; }
    memcpy(out, b->fragments.buffer, used);
    return (int)used;
}

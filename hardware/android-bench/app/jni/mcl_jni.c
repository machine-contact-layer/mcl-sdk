/*
 * JNI shim for the MCL Android bench.
 *
 * THE SHIM ADDS NO PROTOCOL.
 *
 * Every byte that goes on the air or comes off it is produced by the canonical
 * sources this file is compiled beside -- mcl-wire, mcl-link, mcl-ap, mcl-sdk,
 * staged at build time from their own repositories and hashed into the build
 * manifest. What is below converts between Java arrays and C buffers, and
 * nothing else. If it ever starts deciding something, the phone has stopped
 * running MCL and started running an imitation of it.
 *
 * That rule is why the acoustic work happens here rather than in Kotlin: a
 * second modulator written in Java would be a second implementation, and two
 * implementations that disagree about a waveform produce a band measurement
 * that is about the disagreement.
 *
 * WHAT JAVA KEEPS
 *
 * AudioTrack, AudioRecord, the BLE advertiser, scanner and GATT roles,
 * permissions, and the run lifecycle. Those are platform services with no
 * NDK equivalent, and they are the part a porting guide has to describe
 * anyway.
 */

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcl/wire.h"
#include "mcl/link.h"
#include "mcl/ap_modem.h"
#include "mcl/ap_listen.h"
#include "mcl/machine.h"

/* --------------------------------------------------------- machine facade */

typedef struct {
    mcl_machine_t machine;
    JNIEnv *active_env;
    jobject platform;
    jmethodID transmit;
    uint8_t medium_busy;
    uint8_t self_transmitting;
    uint8_t candidate_pending;
    uint8_t candidate_close_pending;
    uint8_t candidate_transport;
    uint8_t candidate_profile;
    uint32_t peer_endpoint_token;
    uint32_t local_endpoint_token;
} android_machine_t;

static uint32_t android_clock_ms(void *user)
{
    struct timespec now;
    (void)user;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint32_t)((uint64_t)now.tv_sec * 1000u
                      + (uint64_t)now.tv_nsec / 1000000u);
}

static int android_random_bytes(void *user, uint8_t *out, size_t size)
{
    FILE *random;
    size_t read;
    (void)user;
    random = fopen("/dev/urandom", "rb");
    if (random == NULL) {
        return -1;
    }
    read = fread(out, 1u, size, random);
    (void)fclose(random);
    return (read == size) ? 0 : -1;
}

static int32_t android_transport_send(void *user, uint8_t transport_id,
                                      const uint8_t *data, size_t size)
{
    android_machine_t *self = (android_machine_t *)user;
    jbyteArray bytes;
    jint result;
    if (self->active_env == NULL || self->platform == NULL ||
        self->transmit == NULL || size > (size_t)INT32_MAX) {
        return -1;
    }
    bytes = (*self->active_env)->NewByteArray(self->active_env, (jsize)size);
    if (bytes == NULL) {
        return -1;
    }
    (*self->active_env)->SetByteArrayRegion(
        self->active_env, bytes, 0, (jsize)size, (const jbyte *)data);
    result = (*self->active_env)->CallIntMethod(
        self->active_env, self->platform, self->transmit,
        (jint)transport_id, bytes);
    (*self->active_env)->DeleteLocalRef(self->active_env, bytes);
    if ((*self->active_env)->ExceptionCheck(self->active_env)) {
        (*self->active_env)->ExceptionClear(self->active_env);
        return -1;
    }
    return (int32_t)result;
}

static int android_medium_busy(void *user)
{
    return ((android_machine_t *)user)->medium_busy != 0u;
}

static int android_self_transmitting(void *user)
{
    return ((android_machine_t *)user)->self_transmitting != 0u;
}

static mcl_machine_candidate_t android_candidate_open(
    void *user, uint8_t transport_id, uint8_t profile_id,
    uint32_t peer_endpoint_token, uint32_t local_endpoint_token)
{
    android_machine_t *self = (android_machine_t *)user;
    self->candidate_pending = 1u;
    self->candidate_transport = transport_id;
    self->candidate_profile = profile_id;
    self->peer_endpoint_token = peer_endpoint_token;
    self->local_endpoint_token = local_endpoint_token;
    return MCL_MACHINE_CANDIDATE_PENDING;
}

static void android_candidate_close(void *user, uint8_t transport_id)
{
    android_machine_t *self = (android_machine_t *)user;
    self->candidate_close_pending = 1u;
    self->candidate_transport = transport_id;
}

JNIEXPORT jlong JNICALL
Java_org_mcl_bench_Mcl_machineCreate(JNIEnv *env, jclass cls,
                                     jint source_ref, jint role,
                                     jobject platform_object)
{
    android_machine_t *self;
    mcl_machine_config_t config;
    mcl_platform_v1_t platform_ops;
    jclass platform_class;
    (void)cls;
    if (platform_object == NULL) {
        return 0;
    }
    self = (android_machine_t *)calloc(1u, sizeof(*self));
    if (self == NULL) {
        return 0;
    }
    if (mcl_machine_config_deployment(
            &config, MCL_DEPLOYMENT_REFERENCE_1, (uint32_t)source_ref,
            (mcl_contact_role_t)role) != MCL_MACHINE_OK) {
        free(self);
        return 0;
    }
    self->platform = (*env)->NewGlobalRef(env, platform_object);
    platform_class = (*env)->GetObjectClass(env, platform_object);
    if (self->platform == NULL || platform_class == NULL) {
        if (self->platform != NULL) {
            (*env)->DeleteGlobalRef(env, self->platform);
        }
        free(self);
        return 0;
    }
    self->transmit = (*env)->GetMethodID(env, platform_class, "transmit", "(I[B)I");
    (*env)->DeleteLocalRef(env, platform_class);
    if (self->transmit == NULL) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, self->platform);
        free(self);
        return 0;
    }
    memset(&platform_ops, 0, sizeof(platform_ops));
    platform_ops.clock_ms = android_clock_ms;
    platform_ops.random_bytes = android_random_bytes;
    platform_ops.transport_send = android_transport_send;
    platform_ops.medium_busy = android_medium_busy;
    platform_ops.self_transmitting = android_self_transmitting;
    platform_ops.candidate_open = android_candidate_open;
    platform_ops.candidate_close = android_candidate_close;
    platform_ops.user = self;
    if (mcl_machine_init(&self->machine, &config, &platform_ops)
        != MCL_MACHINE_OK) {
        (*env)->DeleteGlobalRef(env, self->platform);
        free(self);
        return 0;
    }
    return (jlong)(intptr_t)self;
}

JNIEXPORT void JNICALL
Java_org_mcl_bench_Mcl_machineDestroy(JNIEnv *env, jclass cls, jlong handle)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    (void)cls;
    if (self != NULL) {
        (*env)->DeleteGlobalRef(env, self->platform);
        free(self);
    }
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machineStart(JNIEnv *env, jclass cls, jlong handle)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    (void)cls;
    if (self == NULL) {
        return (jint)MCL_MACHINE_ERR_NULL;
    }
    self->active_env = env;
    {
        jint status = (jint)mcl_machine_start(&self->machine);
        self->active_env = NULL;
        return status;
    }
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machinePoll(JNIEnv *env, jclass cls, jlong handle,
                                   jintArray info)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    mcl_machine_event_t event;
    mcl_machine_status_t status;
    jint values[6];
    (void)cls;
    if (self == NULL || info == NULL || (*env)->GetArrayLength(env, info) < 6) {
        return (jint)MCL_MACHINE_ERR_NULL;
    }
    memset(&event, 0, sizeof(event));
    self->active_env = env;
    status = mcl_machine_poll(&self->machine, &event);
    self->active_env = NULL;
    values[0] = (jint)event.kind;
    values[1] = (jint)event.transport_id;
    values[2] = (jint)event.profile_id;
    values[3] = (jint)event.peer_ref;
    values[4] = (jint)event.session_ref;
    values[5] = (jint)event.status;
    (*env)->SetIntArrayRegion(env, info, 0, 6, values);
    return (jint)status;
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machineReceive(JNIEnv *env, jclass cls, jlong handle,
                                      jint transport_id, jbyteArray bytes,
                                      jint size)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    jbyte *data;
    mcl_machine_status_t status;
    (void)cls;
    if (self == NULL || bytes == NULL || size < 0 ||
        size > (*env)->GetArrayLength(env, bytes)) {
        return (jint)MCL_MACHINE_ERR_NULL;
    }
    data = (*env)->GetByteArrayElements(env, bytes, NULL);
    if (data == NULL) {
        return (jint)MCL_MACHINE_ERR_STATE;
    }
    self->active_env = env;
    status = mcl_machine_receive(&self->machine, (uint8_t)transport_id,
                                 (const uint8_t *)data, (size_t)size);
    self->active_env = NULL;
    (*env)->ReleaseByteArrayElements(env, bytes, data, JNI_ABORT);
    return (jint)status;
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machineTakeCandidateRequest(JNIEnv *env, jclass cls,
                                                   jlong handle,
                                                   jintArray info)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    jint values[5];
    (void)cls;
    if (self == NULL || info == NULL || (*env)->GetArrayLength(env, info) < 5) {
        return -1;
    }
    if (self->candidate_pending == 0u && self->candidate_close_pending == 0u) {
        return 0;
    }
    values[0] = (jint)self->candidate_transport;
    values[1] = (jint)self->candidate_profile;
    values[2] = (jint)self->peer_endpoint_token;
    values[3] = (jint)self->local_endpoint_token;
    values[4] = (self->candidate_close_pending != 0u) ? 1 : 0;
    (*env)->SetIntArrayRegion(env, info, 0, 5, values);
    self->candidate_pending = 0u;
    self->candidate_close_pending = 0u;
    return 1;
}

JNIEXPORT void JNICALL
Java_org_mcl_bench_Mcl_machineSetMediumState(JNIEnv *env, jclass cls,
                                             jlong handle, jboolean busy,
                                             jboolean self_tx)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    (void)env;
    (void)cls;
    if (self != NULL) {
        self->medium_busy = busy ? 1u : 0u;
        self->self_transmitting = self_tx ? 1u : 0u;
    }
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machineCandidateReady(JNIEnv *env, jclass cls,
                                             jlong handle)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    (void)cls;
    if (self == NULL) {
        return (jint)MCL_MACHINE_ERR_NULL;
    }
    self->active_env = env;
    {
        jint status = (jint)mcl_machine_candidate_ready(&self->machine);
        self->active_env = NULL;
        return status;
    }
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machineCandidateRefused(JNIEnv *env, jclass cls,
                                               jlong handle)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    (void)cls;
    if (self == NULL) {
        return (jint)MCL_MACHINE_ERR_NULL;
    }
    self->active_env = env;
    {
        jint status = (jint)mcl_machine_candidate_refused(&self->machine);
        self->active_env = NULL;
        return status;
    }
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_machinePolicy(JNIEnv *env, jclass cls, jlong handle,
                                     jboolean admit)
{
    android_machine_t *self = (android_machine_t *)(intptr_t)handle;
    (void)cls;
    if (self == NULL) {
        return (jint)MCL_MACHINE_ERR_NULL;
    }
    self->active_env = env;
    {
        jint status = admit ? (jint)mcl_machine_admit(&self->machine)
                            : (jint)mcl_machine_refuse(&self->machine);
        self->active_env = NULL;
        return status;
    }
}

/* ------------------------------------------------------------------ band */

/*
 * Byte-for-byte the convention experiment 008's firmware uses, and the one the
 * DFR1154 node uses: the preamble chirp is derived as f0-1000 .. f1 with a
 * 500 Hz floor. Two rigs that derive the chirp differently are not measuring
 * the same band, and the failure looks like a dead channel rather than a
 * misconfigured one.
 */
static void apply_band(mcl_ap_modem_config_t *config, int band_low, int band_high)
{
    if (band_low <= 0 || band_high <= 0) {
        return;
    }
    config->fsk_freq_0_hz = (float)band_low;
    config->fsk_freq_1_hz = (float)band_high;
    config->preamble_f_start_hz = (float)band_low - 1000.0f;
    config->preamble_f_end_hz = (float)band_high;
    if (config->preamble_f_start_hz < 500.0f) {
        config->preamble_f_start_hz = 500.0f;
    }
}

/* --------------------------------------------------------------- version */

JNIEXPORT jstring JNICALL
Java_org_mcl_bench_Mcl_version(JNIEnv *env, jclass cls)
{
    char buffer[192];
    (void)cls;
    snprintf(buffer, sizeof(buffer),
             "wire_major=%u link_major=%u scratch=%u modem_max_payload=%u",
             (unsigned)MCL_WIRE_STABLE_MAJOR,
             (unsigned)MCL_LINK_STABLE_MAJOR,
             (unsigned)sizeof(mcl_ap_modem_scratch_t),
             (unsigned)MCL_AP_MODEM_MAX_PAYLOAD_BYTES);
    return (*env)->NewStringUTF(env, buffer);
}

/* ------------------------------------------------------------- modulate */

/*
 * Exactly the samples the reference modulator produces. Returns the sample
 * count, or a negative modem status.
 */
JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_modulate(JNIEnv *env, jclass cls, jbyteArray payload,
                                jshortArray out, jint band_low, jint band_high)
{
    mcl_ap_modem_config_t config;
    jbyte *in_bytes;
    jshort *out_samples;
    jsize in_len, out_len;
    size_t used = 0u;
    mcl_ap_modem_status_t status;

    (void)cls;
    in_len = (*env)->GetArrayLength(env, payload);
    out_len = (*env)->GetArrayLength(env, out);

    mcl_ap_modem_default_config(&config);
    apply_band(&config, band_low, band_high);

    in_bytes = (*env)->GetByteArrayElements(env, payload, NULL);
    out_samples = (*env)->GetShortArrayElements(env, out, NULL);
    status = mcl_ap_modem_encode(&config, (const uint8_t *)in_bytes,
                                 (size_t)in_len, (int16_t *)out_samples,
                                 (size_t)out_len, &used);
    (*env)->ReleaseByteArrayElements(env, payload, in_bytes, JNI_ABORT);
    (*env)->ReleaseShortArrayElements(env, out, out_samples, 0);

    if (status != MCL_AP_MODEM_OK) {
        return -(jint)status;
    }
    return (jint)used;
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_modulatedSamples(JNIEnv *env, jclass cls, jint payload_bytes,
                                        jint band_low, jint band_high)
{
    mcl_ap_modem_config_t config;
    (void)env;
    (void)cls;
    mcl_ap_modem_default_config(&config);
    apply_band(&config, band_low, band_high);
    return (jint)mcl_ap_modem_encoded_samples(&config, (size_t)payload_bytes);
}

/* -------------------------------------------------------------- listener */

/*
 * One listener, its window and its scratch, in one allocation the caller owns
 * through an opaque handle. Heap here rather than static: this is a phone, not
 * a microcontroller, and the union-arena discipline the DFR needs buys nothing
 * where there is a gigabyte.
 */
typedef struct {
    mcl_ap_listener_t listener;
    mcl_ap_listen_config_t config;
    mcl_ap_modem_scratch_t scratch;
    int16_t *window;
    size_t window_samples;
} bench_listener_t;

JNIEXPORT jlong JNICALL
Java_org_mcl_bench_Mcl_listenerCreate(JNIEnv *env, jclass cls, jint max_payload,
                                      jint band_low, jint band_high,
                                      jint window_samples)
{
    bench_listener_t *self;
    size_t needed;

    (void)env;
    (void)cls;
    self = (bench_listener_t *)calloc(1u, sizeof(*self));
    if (self == NULL) {
        return 0;
    }
    mcl_ap_listen_default_config(&self->config);
    self->config.max_payload_bytes = (uint8_t)max_payload;
    apply_band(&self->config.modem, band_low, band_high);

    /*
     * The minimum is asked for, not assumed. A window shorter than the profile
     * needs makes a machine report a dead room rather than a misconfiguration,
     * and the minimum moves with the payload size and the band.
     */
    needed = mcl_ap_listen_min_window_samples(&self->config);
    if (needed == 0u) {
        free(self);
        return 0;
    }
    self->window_samples = (size_t)window_samples;
    if (self->window_samples < needed) {
        self->window_samples = needed;
    }
    self->window = (int16_t *)calloc(self->window_samples, sizeof(int16_t));
    if (self->window == NULL) {
        free(self);
        return 0;
    }
    if (mcl_ap_listen_init(&self->listener, &self->config, self->window,
                           self->window_samples) != MCL_AP_LISTEN_OK) {
        free(self->window);
        free(self);
        return 0;
    }
    return (jlong)(intptr_t)self;
}

JNIEXPORT void JNICALL
Java_org_mcl_bench_Mcl_listenerDestroy(JNIEnv *env, jclass cls, jlong handle)
{
    bench_listener_t *self = (bench_listener_t *)(intptr_t)handle;
    (void)env;
    (void)cls;
    if (self == NULL) {
        return;
    }
    free(self->window);
    free(self);
}

JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_listenerPush(JNIEnv *env, jclass cls, jlong handle,
                                    jshortArray pcm, jint count)
{
    bench_listener_t *self = (bench_listener_t *)(intptr_t)handle;
    jshort *samples;
    mcl_ap_listen_status_t status;

    (void)cls;
    if (self == NULL) {
        return -1;
    }
    samples = (*env)->GetShortArrayElements(env, pcm, NULL);
    status = mcl_ap_listen_push(&self->listener, (const int16_t *)samples,
                               (size_t)count);
    (*env)->ReleaseShortArrayElements(env, pcm, samples, JNI_ABORT);
    return (jint)status;
}

/*
 * One poll, one result. `info` receives {payload_bytes, modem_status,
 * samples_unscanned, contacts, heard} -- the counters an operator needs to
 * tell a quiet room from a listener that was too busy to hear it.
 */
JNIEXPORT jint JNICALL
Java_org_mcl_bench_Mcl_listenerPoll(JNIEnv *env, jclass cls, jlong handle,
                                    jbyteArray out_payload, jintArray info)
{
    bench_listener_t *self = (bench_listener_t *)(intptr_t)handle;
    mcl_ap_listen_event_t event;
    mcl_ap_listen_result_t result;
    jbyte *payload;
    jsize capacity;
    jint values[5];

    (void)cls;
    if (self == NULL) {
        return -1;
    }
    memset(&event, 0, sizeof(event));
    capacity = (*env)->GetArrayLength(env, out_payload);
    payload = (*env)->GetByteArrayElements(env, out_payload, NULL);
    result = mcl_ap_listen_poll(&self->listener, &self->scratch,
                                (uint8_t *)payload, (size_t)capacity, &event);
    (*env)->ReleaseByteArrayElements(env, out_payload, payload, 0);

    values[0] = (jint)event.payload_bytes;
    values[1] = (jint)event.modem_status;
    values[2] = (jint)self->listener.samples_unscanned;
    values[3] = (jint)self->listener.contacts;
    values[4] = (jint)self->listener.heard;
    (*env)->SetIntArrayRegion(env, info, 0, 5, values);
    return (jint)result;
}

/* ------------------------------------------------------------ Tier-0 */

/*
 * The three bootstrap objects, encoded at the STABLE major by the canonical
 * encoder. AP-BOOTSTRAP-1 carries exactly these and nothing else:
 *
 *   PRESENCE          10 bytes
 *   TRANSPORT_ACCEPT  16 bytes
 *   TRANSPORT_OFFER   17 bytes
 */
static jbyteArray encoded(JNIEnv *env, const mcl_wire_tier0_t *object)
{
    uint8_t buffer[64];
    size_t written = 0u;
    jbyteArray out;

    if (mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, object, buffer,
                                       sizeof(buffer), &written) != MCL_WIRE_OK) {
        return NULL;
    }
    out = (*env)->NewByteArray(env, (jsize)written);
    if (out == NULL) {
        return NULL;
    }
    (*env)->SetByteArrayRegion(env, out, 0, (jsize)written, (const jbyte *)buffer);
    return out;
}

JNIEXPORT jbyteArray JNICALL
Java_org_mcl_bench_Mcl_encodePresence(JNIEnv *env, jclass cls, jint source_ref,
                                      jint capability_tag, jint ttl)
{
    mcl_wire_tier0_t object;
    (void)cls;
    memset(&object, 0, sizeof(object));
    object.kind = MCL_WIRE_KIND_PRESENCE;
    object.source_ref = (uint32_t)source_ref;
    object.body.presence.capability_tag = (uint32_t)capability_tag;
    object.body.presence.ttl = (uint8_t)ttl;
    return encoded(env, &object);
}

JNIEXPORT jbyteArray JNICALL
Java_org_mcl_bench_Mcl_encodeTransportOffer(JNIEnv *env, jclass cls, jint source_ref,
                                            jint migration_ref, jint transport_id,
                                            jint profile_id, jint endpoint_token,
                                            jint validity)
{
    mcl_wire_tier0_t object;
    (void)cls;
    memset(&object, 0, sizeof(object));
    object.kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
    object.source_ref = (uint32_t)source_ref;
    object.body.transport_offer.migration_ref = (uint32_t)migration_ref;
    object.body.transport_offer.transport_id = (uint8_t)transport_id;
    object.body.transport_offer.profile_id = (uint8_t)profile_id;
    object.body.transport_offer.endpoint_token = (uint32_t)endpoint_token;
    object.body.transport_offer.validity = (uint8_t)validity;
    return encoded(env, &object);
}

JNIEXPORT jbyteArray JNICALL
Java_org_mcl_bench_Mcl_encodeTransportAccept(JNIEnv *env, jclass cls, jint source_ref,
                                             jint migration_ref, jint transport_id,
                                             jint profile_id, jint session_ref)
{
    mcl_wire_tier0_t object;
    (void)cls;
    memset(&object, 0, sizeof(object));
    object.kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
    object.source_ref = (uint32_t)source_ref;
    object.body.transport_accept.migration_ref = (uint32_t)migration_ref;
    object.body.transport_accept.transport_id = (uint8_t)transport_id;
    object.body.transport_accept.profile_id = (uint8_t)profile_id;
    object.body.transport_accept.session_ref = (uint32_t)session_ref;
    return encoded(env, &object);
}

/*
 * Decode for the log. Returns a description, or NULL if the bytes are not a
 * canonical Tier-0 object -- which is a normal outcome on a public medium and
 * not an error.
 */
JNIEXPORT jstring JNICALL
Java_org_mcl_bench_Mcl_describeTier0(JNIEnv *env, jclass cls, jbyteArray payload,
                                     jint length)
{
    mcl_wire_tier0_t object;
    jbyte *bytes;
    size_t consumed = 0u;
    mcl_wire_status_t status;
    char buffer[192];

    (void)cls;
    bytes = (*env)->GetByteArrayElements(env, payload, NULL);
    status = mcl_wire_tier0_decode((const uint8_t *)bytes, (size_t)length,
                                   &object, &consumed);
    (*env)->ReleaseByteArrayElements(env, payload, bytes, JNI_ABORT);
    if (status != MCL_WIRE_OK) {
        return NULL;
    }

    switch (object.kind) {
    case MCL_WIRE_KIND_PRESENCE:
        snprintf(buffer, sizeof(buffer),
                 "PRESENCE source_ref=%08lX capability_tag=%lu ttl=%u",
                 (unsigned long)object.source_ref,
                 (unsigned long)object.body.presence.capability_tag,
                 (unsigned)object.body.presence.ttl);
        break;
    case MCL_WIRE_KIND_TRANSPORT_OFFER:
        snprintf(buffer, sizeof(buffer),
                 "TRANSPORT_OFFER source_ref=%08lX migration_ref=%08lX "
                 "transport=%u profile=%u endpoint_token=%08lX validity=%u",
                 (unsigned long)object.source_ref,
                 (unsigned long)object.body.transport_offer.migration_ref,
                 (unsigned)object.body.transport_offer.transport_id,
                 (unsigned)object.body.transport_offer.profile_id,
                 (unsigned long)object.body.transport_offer.endpoint_token,
                 (unsigned)object.body.transport_offer.validity);
        break;
    case MCL_WIRE_KIND_TRANSPORT_ACCEPT:
        snprintf(buffer, sizeof(buffer),
                 "TRANSPORT_ACCEPT source_ref=%08lX migration_ref=%08lX "
                 "transport=%u profile=%u session_ref=%08lX",
                 (unsigned long)object.source_ref,
                 (unsigned long)object.body.transport_accept.migration_ref,
                 (unsigned)object.body.transport_accept.transport_id,
                 (unsigned)object.body.transport_accept.profile_id,
                 (unsigned long)object.body.transport_accept.session_ref);
        break;
    default:
        snprintf(buffer, sizeof(buffer), "kind=%u consumed=%u",
                 (unsigned)object.kind, (unsigned)consumed);
        break;
    }
    return (*env)->NewStringUTF(env, buffer);
}

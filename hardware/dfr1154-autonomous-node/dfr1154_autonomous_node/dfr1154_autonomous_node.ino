/*
 * MCL autonomous node, DFR1154 (FireBeetle 2 ESP32-S3).
 *
 * LAB / EXPERIMENTAL. This is a test node, not a product and not a second
 * implementation of anything.
 *
 * WHAT IS NEW HERE, AND WHY IT NEEDED A NEW SKETCH
 *
 * Two rigs already exist and neither can answer the remaining question.
 *
 *   mcl-ap experiment 008 runs Wire, Link and the AP modem on the board, but
 *   the laptop decides what happens next: it sends SEND WIRE, it sends LISTEN
 *   FRAME, it chooses the moment. The board is an MCL endpoint with a host for
 *   a nervous system.
 *
 *   mcl-sdk hardware/esp32-dual-peer holds ONE contact across a live UDP
 *   socket and a live BLE link and migrates between them, which is what proved
 *   migration is real. But the host drives the sequence, and the board never
 *   hears anybody: there is no microphone in that rig at all.
 *
 * The release gate's remaining row asks something neither can reach: can two
 * machines that were told NOTHING about each other find each other, agree on a
 * bearer, reach it, validate it and migrate -- with no host in the loop making
 * the decisions that are supposed to be the protocol's? A node that waits for
 * a serial command cannot answer that, because the command is the answer.
 *
 * So this firmware owns its own clock, randomness, microphone, speaker, radios
 * and rendezvous state machine, and runs a scenario to completion with nothing
 * attached. The host arms it and reads the log afterwards.
 *
 * THE RULE THAT MAKES THE EVIDENCE WORTH ANYTHING
 *
 *   THE LAB CONTROL PLANE IS NOT THE MCL DATA PLANE.
 *
 * This node exposes an HTTP API over its own SoftAP so a scenario can be armed
 * and a log retrieved without a cable. That is instrumentation. It would be
 * very easy, and completely invisible in a result, to let it also carry a peer
 * address, a token or a session reference -- and then report "two strangers
 * discovered each other" about a run where one of them was told the answer.
 *
 * Two things stop that here, and only the second one is real:
 *
 *   1. The configuration surface has no field for peer-specific data. There is
 *      nowhere to put a peer IP, UDP port, BLE address, source_ref,
 *      endpoint_token, migration_ref, session_ref, secret or pairing state.
 *      This is a good fence and it is only as strong as the next person to
 *      edit the struct.
 *
 *   2. EVERY PEER-IDENTIFYING VALUE THE NODE USES IS LOGGED WITH ITS
 *      PROVENANCE. Each one records whether it was learned from the air,
 *      learned from a bearer, generated locally, or configured. A run is
 *      zero-prior if and only if no such value reads CONFIGURED, and that is
 *      checkable from the log by someone who does not trust this comment.
 *      `/api/result` reports it as `zero_prior`, computed from the tags rather
 *      than asserted.
 *
 * The difference matters because provenance is exactly what this project has
 * been caught on before: a value that arrived from an instrument looks
 * identical, in every log, to one that arrived from the peer.
 *
 * WHAT THIS NODE CANNOT ESTABLISH
 *
 *   - Independent interoperability. Both ends of any run compile the same
 *     mcl-sdk, mcl-wire, mcl-link, mcl-ap, mcl-ip and mcl-ble sources. A
 *     shared misreading of the specification passes on both sides and is
 *     invisible. Nothing in this directory substitutes for a second
 *     implementation.
 *   - Security. There is none. The SoftAP has a password and the BLE link uses
 *     Just Works; every MCL reference crosses both media in the clear. A
 *     listener that heard first contact can complete the sequence and be
 *     accepted exactly as an honest peer would. That is the honest limit of
 *     correlation and reachability without cryptography, and MCL v1 claims
 *     nothing else.
 *   - Stranger discovery over IP. The UDP endpoint in the IP scenario is
 *     harness configuration and is recorded as such. MCL defines no global
 *     discovery port and this rig does not invent one.
 *
 * MEMORY, WHICH DECIDED THE DESIGN
 *
 * Measured, not estimated: the AP listener's window for a 17-byte maximum
 * payload is 47 360 samples (92 KB), the modem's receive scratch is 76 868
 * bytes (75 KB), and modulating a 17-byte object produces 66 560 samples
 * (130 KB). Against roughly 320 KB of usable internal DRAM, those three plus a
 * Wi-Fi stack plus a BLE stack do not fit, and experiment 008 already found
 * the linker saying so in the honest way (`.dram0.bss will not fit`).
 *
 * Two decisions follow.
 *
 *   ONE ARENA, NOT TWO BUFFERS. Transmit and receive never overlap: while this
 *   machine's speaker is driven, its microphone hears its own emission and
 *   nothing else, which is why mcl_rdv_platform_t has self_transmitting at
 *   all. So one arena, sized by the larger use (the waveform), serves as the
 *   listener's window and is repurposed for modulation around a transmission,
 *   with the listener reset either side. What that costs is any frame
 *   half-buffered when we start talking -- which the physics was going to take
 *   anyway.
 *
 *   PSRAM IS NOT USED. The board has 8 MB of it and it is the obvious way out,
 *   and experiment 008 declined it for a reason that still holds: the
 *   correlation inner loop reads the window tens of millions of times per
 *   acquisition, and this project has never verified the QSPI/OPI mode option
 *   for this part. A rig that boots differently depending on a board option
 *   nobody checked is not an instrument. The radios are brought up and down
 *   instead -- see the quiesce rule below, which the evidence wanted anyway.
 *
 * SAFETY
 *
 * Flash the APPLICATION PARTITION ONLY, at 0x20000, with esptool. Do not
 * upload through the Arduino CLI: its upload step can also rewrite the
 * bootloader and the partition table. build-firmware.ps1 builds without
 * uploading and refuses to run if the factory backup is missing.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include "ESP_I2S.h"

#include "mcl/ap_listen.h"
#include "mcl/ap_modem.h"
#include "mcl/wire.h"
#include "mcl/link.h"

namespace {

/* ------------------------------------------------------------------ pins */

constexpr uint32_t  kSampleRateHz    = 48000;
constexpr gpio_num_t kPdmClockPin    = GPIO_NUM_38;
constexpr gpio_num_t kPdmDataPin     = GPIO_NUM_39;
constexpr gpio_num_t kActivityLedPin = GPIO_NUM_3;
constexpr uint8_t   kAmpBclkPin      = 45;
constexpr uint8_t   kAmpLrclkPin     = 46;
constexpr uint8_t   kAmpDataPin      = 42;

/* --------------------------------------------------------------- identity */

/*
 * A source_ref is a correlation reference for semantic origin. It is NOT
 * identity and carries no uniqueness property (mcl-wire/spec). Two unrelated
 * builders may legally choose the same one, which is exactly why the
 * rendezvous coordinator refuses to use it to detect self-echo and refuses to
 * derive contention backoff from it. It is randomised at boot here so that two
 * of these boards in one room do not start life sharing one.
 */
uint32_t g_source_ref = 0u;

/* ----------------------------------------------------------------- arena */

/*
 * 66 560 samples = 130 KB. Sized by the largest thing this node emits: a
 * 17-byte TRANSPORT_OFFER. As a listener window it is far above the 47 360
 * sample minimum for a 17-byte maximum payload, which costs nothing and buys
 * patience.
 */
constexpr size_t kArenaSamples = 66560;
int16_t g_arena[kArenaSamples];

/* The modem's receive working set: 75 KB, static because a FreeRTOS task
   stack is not where 75 KB goes. */
mcl_ap_modem_scratch_t g_scratch;

mcl_ap_listener_t      g_listener;
mcl_ap_listen_config_t g_listen_config;

/*
 * AP-BOOTSTRAP-1 carries exactly three objects and the largest is 17 bytes.
 * Declaring 64 here would cost about 1.4 s of extra patience on every frame
 * that fails, and would more than double the window.
 */
constexpr uint8_t kMaxBootstrapPayload = 17;

bool g_microphone_ready = false;
bool g_speaker_ready    = false;
volatile bool g_transmitting = false;

I2SClass microphone;
I2SClass speaker;

/* ------------------------------------------------------------ provenance */

/*
 * Where a peer-identifying value came from. This is the mechanism that makes
 * a zero-prior claim checkable instead of asserted; see the header.
 *
 * CONFIGURED is not a value this firmware ever writes. It exists so that if
 * somebody later adds a configuration path for one of these values, the log
 * says so in every run rather than silently becoming untrue.
 */
enum Provenance : uint8_t {
    PROV_LOCAL      = 0,  /* this node generated it */
    PROV_FROM_AIR   = 1,  /* learned from an acoustic frame */
    PROV_FROM_BEARER= 2,  /* learned over BLE/IP after the candidate opened */
    PROV_CONFIGURED = 3   /* supplied by the lab control plane -- NOT zero-prior */
};

const char *provenance_name(uint8_t p) {
    switch (p) {
        case PROV_LOCAL:       return "LOCAL";
        case PROV_FROM_AIR:    return "FROM_AIR";
        case PROV_FROM_BEARER: return "FROM_BEARER";
        case PROV_CONFIGURED:  return "CONFIGURED";
        default:               return "UNKNOWN";
    }
}

struct TaggedValue {
    const char *name;
    uint32_t    value;
    uint8_t     provenance;
    bool        set;
};

constexpr size_t kMaxTagged = 8;
TaggedValue g_tagged[kMaxTagged];
size_t      g_tagged_count = 0;

void tag_value(const char *name, uint32_t value, Provenance prov) {
    for (size_t i = 0; i < g_tagged_count; ++i) {
        if (strcmp(g_tagged[i].name, name) == 0) {
            g_tagged[i].value = value;
            g_tagged[i].provenance = static_cast<uint8_t>(prov);
            g_tagged[i].set = true;
            return;
        }
    }
    if (g_tagged_count >= kMaxTagged) { return; }
    g_tagged[g_tagged_count].name = name;
    g_tagged[g_tagged_count].value = value;
    g_tagged[g_tagged_count].provenance = static_cast<uint8_t>(prov);
    g_tagged[g_tagged_count].set = true;
    ++g_tagged_count;
}

/* A run is zero-prior when no peer-identifying value was configured. */
bool run_is_zero_prior() {
    for (size_t i = 0; i < g_tagged_count; ++i) {
        if (g_tagged[i].set &&
            g_tagged[i].provenance == static_cast<uint8_t>(PROV_CONFIGURED)) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------- run log */

/*
 * A fixed ring. No heap, no growth, and an explicit dropped counter: a log
 * that silently discards its oldest entries turns a run that overflowed it
 * into a run that looks complete.
 */
constexpr size_t kLogEntries = 128;
constexpr size_t kLogTextMax = 96;

struct LogEntry {
    uint32_t at_ms;
    char     text[kLogTextMax];
};

LogEntry g_log[kLogEntries];
size_t   g_log_head = 0;     /* next write position */
size_t   g_log_count = 0;    /* entries currently held */
uint32_t g_log_dropped = 0;  /* entries overwritten before being read */

void log_line(const char *fmt, ...) {
    LogEntry &e = g_log[g_log_head];
    e.at_ms = millis();

    va_list args;
    va_start(args, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);

    g_log_head = (g_log_head + 1u) % kLogEntries;
    if (g_log_count < kLogEntries) {
        ++g_log_count;
    } else {
        ++g_log_dropped;
    }

    /* Serial stays a mirror, never the only copy: a run with nothing attached
       is the point of this node, and the log must survive it. */
    Serial.print("MCLAUTO ");
    Serial.println(e.text);
}

/* ---------------------------------------------------------- lab control */

/*
 * THE CONFIGURATION SURFACE.
 *
 * Read the field list as a contract: there is deliberately nowhere here to put
 * a peer IP, a UDP port, a BLE address, a source_ref, an endpoint_token, a
 * migration_ref, a session_ref, a secret or a pairing state. Adding one makes
 * every zero-prior claim this node produces false, and if you add one anyway,
 * tag the value PROV_CONFIGURED so the log says so.
 */
struct ScenarioConfig {
    uint8_t  scenario;          /* which scenario to run */
    uint32_t duration_ms;       /* bound on the whole run */
    uint16_t band_low_hz;       /* 0 = modem default */
    uint16_t band_high_hz;
    uint8_t  emit_gain_pct;     /* transmit level */
    bool     quiesce_wifi;      /* take Wi-Fi down for the run */
};

ScenarioConfig g_config = {
    /* scenario      */ 0,
    /* duration_ms   */ 60000,
    /* band_low_hz   */ 0,
    /* band_high_hz  */ 0,
    /* emit_gain_pct */ 100,
    /* quiesce_wifi  */ false
};

enum RunState : uint8_t {
    RUN_IDLE = 0,
    RUN_ARMED = 1,
    RUN_RUNNING = 2,
    RUN_DONE = 3,
    RUN_STOPPED = 4,
    RUN_FAILED = 5
};

const char *run_state_name(uint8_t s) {
    switch (s) {
        case RUN_IDLE:    return "IDLE";
        case RUN_ARMED:   return "ARMED";
        case RUN_RUNNING: return "RUNNING";
        case RUN_DONE:    return "DONE";
        case RUN_STOPPED: return "STOPPED";
        case RUN_FAILED:  return "FAILED";
        default:          return "UNKNOWN";
    }
}

volatile uint8_t g_run_state = RUN_IDLE;
uint32_t g_run_started_ms = 0;
uint32_t g_run_ended_ms = 0;
char     g_run_failure[64] = {0};

/* Counters that make a run readable without parsing every log line. */
struct RunCounters {
    uint32_t frames_heard;      /* preamble found, payload lost */
    uint32_t frames_recovered;  /* CRC verified */
    uint32_t frames_emitted;
    uint32_t objects_decoded;
    uint32_t samples_unscanned; /* audio discarded before it was searched */
};
RunCounters g_counters = {0, 0, 0, 0, 0};

/* ------------------------------------------------------------ Wi-Fi/HTTP */

/*
 * The SoftAP is the lab control plane. Its credentials are compiled in and
 * are not secret; nothing on this network is trusted and nothing on it is
 * part of an MCL result.
 */
const char *kApSsid = "mcl-auto-node";
const char *kApPass = "mcl-lab-node";

WebServer   g_http(80);
bool        g_wifi_up = false;

void wifi_up() {
    if (g_wifi_up) { return; }
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
    g_wifi_up = true;
    log_line("wifi up ssid=%s ip=%s", kApSsid, WiFi.softAPIP().toString().c_str());
}

void wifi_down() {
    if (!g_wifi_up) { return; }
    g_http.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    g_wifi_up = false;
    log_line("wifi down (quiesced for the run)");
}

/* --------------------------------------------------------------- audio */

/*
 * Move the whole waveform, not just the two tones.
 *
 * The receiver correlates against a chirp derived as f0-1000 .. f1, so a band
 * change that moved only the tones would leave the preamble sweeping where the
 * transmitter may not be able to drive -- and the failure would look like a
 * dead channel rather than a misconfigured one. This is byte-for-byte the
 * convention experiment 008's firmware uses, deliberately: two rigs that
 * derive the chirp differently are not measuring the same band.
 *
 * Experiment 011 found the other half of this trap: a rig that applies a band
 * on the host and never sends it to the board reports about a different band
 * at a perfectly healthy signal level. Here there is no host to disagree with
 * -- the node applies the band to its own transmitter and its own receiver
 * from one function, and the run log records it.
 */
void apply_band(mcl_ap_modem_config_t *config) {
    if (g_config.band_low_hz == 0u || g_config.band_high_hz == 0u) { return; }
    config->fsk_freq_0_hz = static_cast<float>(g_config.band_low_hz);
    config->fsk_freq_1_hz = static_cast<float>(g_config.band_high_hz);
    config->preamble_f_start_hz = static_cast<float>(g_config.band_low_hz) - 1000.0f;
    config->preamble_f_end_hz = static_cast<float>(g_config.band_high_hz);
    if (config->preamble_f_start_hz < 500.0f) {
        config->preamble_f_start_hz = 500.0f;
    }
}

void listener_reset() {
    mcl_ap_listen_default_config(&g_listen_config);
    g_listen_config.max_payload_bytes = kMaxBootstrapPayload;
    apply_band(&g_listen_config.modem);
    const mcl_ap_listen_status_t st =
        mcl_ap_listen_init(&g_listener, &g_listen_config, g_arena, kArenaSamples);
    if (st != MCL_AP_LISTEN_OK) {
        log_line("listener init refused rc=%ld", static_cast<long>(st));
    }
}

/*
 * Emit one payload acoustically.
 *
 * The listener is reset either side, because the arena it is looking at is
 * about to become the waveform. Everything buffered is discarded, which is the
 * truth about what a microphone hears while its own speaker is driven.
 */
bool emit_payload(const uint8_t *payload, size_t len) {
    if (!g_speaker_ready) {
        log_line("emit refused: amplifier not ready");
        return false;
    }
    if (len > kMaxBootstrapPayload) {
        log_line("emit refused: %u bytes exceeds bootstrap maximum %u",
                 static_cast<unsigned>(len),
                 static_cast<unsigned>(kMaxBootstrapPayload));
        return false;
    }

    mcl_ap_modem_config_t modem = g_listen_config.modem;

    size_t used = 0;
    g_transmitting = true;
    const mcl_ap_modem_status_t st =
        mcl_ap_modem_encode(&modem, payload, len, g_arena, kArenaSamples, &used);
    if (st != MCL_AP_MODEM_OK) {
        g_transmitting = false;
        log_line("emit refused: modulate rc=%ld", static_cast<long>(st));
        listener_reset();
        return false;
    }

    if (g_config.emit_gain_pct < 100u) {
        const int32_t g = static_cast<int32_t>(g_config.emit_gain_pct);
        for (size_t i = 0; i < used; ++i) {
            g_arena[i] = static_cast<int16_t>((static_cast<int32_t>(g_arena[i]) * g) / 100);
        }
    }

    digitalWrite(kActivityLedPin, HIGH);
    speaker.write(reinterpret_cast<uint8_t *>(g_arena),
                  used * sizeof(int16_t));
    digitalWrite(kActivityLedPin, LOW);
    g_transmitting = false;

    ++g_counters.frames_emitted;
    log_line("emitted %u bytes, %u samples", static_cast<unsigned>(len),
             static_cast<unsigned>(used));

    listener_reset();
    return true;
}

/*
 * Pull whatever the microphone has and offer it to the listener. Does no
 * correlation, so it is cheap and can be called from the main loop at any
 * cadence; being late costs samples_unscanned, which is counted rather than
 * hidden.
 */
void capture_pump() {
    if (!g_microphone_ready || g_transmitting) { return; }

    /* A small stack block. The window is the arena; this is only the handoff. */
    static int16_t block[1024];
    const size_t want = sizeof(block);
    const int got = microphone.readBytes(reinterpret_cast<char *>(block),
                                         static_cast<int>(want));
    if (got <= 0) { return; }
    (void)mcl_ap_listen_push(&g_listener, block,
                             static_cast<size_t>(got) / sizeof(int16_t));
}

/* ------------------------------------------------------------ HTTP API */

void send_json(int code, const String &body) {
    g_http.send(code, "application/json", body);
}

String json_escape(const char *s) {
    String out;
    for (const char *p = s; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if (*p == '\n') { out += "\\n"; }
        else if (static_cast<unsigned char>(*p) < 0x20) { /* drop control bytes */ }
        else { out += *p; }
    }
    return out;
}

void handle_status() {
    String j = "{";
    j += "\"node\":\"dfr1154-autonomous\",";
    j += "\"firmware\":\"v1\",";
    j += "\"wire_major\":" + String(static_cast<unsigned>(MCL_WIRE_STABLE_MAJOR)) + ",";
    j += "\"link_major\":" + String(static_cast<unsigned>(MCL_LINK_STABLE_MAJOR)) + ",";
    j += "\"run_state\":\"" + String(run_state_name(g_run_state)) + "\",";
    j += "\"scenario\":" + String(static_cast<unsigned>(g_config.scenario)) + ",";
    j += "\"uptime_ms\":" + String(millis()) + ",";
    j += "\"free_heap\":" + String(static_cast<unsigned long>(ESP.getFreeHeap())) + ",";
    j += "\"microphone\":" + String(g_microphone_ready ? "true" : "false") + ",";
    j += "\"speaker\":" + String(g_speaker_ready ? "true" : "false") + ",";
    j += "\"arena_samples\":" + String(static_cast<unsigned>(kArenaSamples)) + ",";
    j += "\"log_held\":" + String(static_cast<unsigned>(g_log_count)) + ",";
    j += "\"log_dropped\":" + String(g_log_dropped);
    j += "}";
    send_json(200, j);
}

void handle_config() {
    if (g_run_state == RUN_RUNNING) {
        send_json(409, "{\"error\":\"run in progress\"}");
        return;
    }
    /*
     * Arguments are read individually and by name. There is no generic
     * "apply every field the client sent" path, because that is how a
     * peer-specific field gets accepted by a surface that never declared one.
     */
    if (g_http.hasArg("scenario")) {
        g_config.scenario = static_cast<uint8_t>(g_http.arg("scenario").toInt());
    }
    if (g_http.hasArg("duration_ms")) {
        g_config.duration_ms = static_cast<uint32_t>(g_http.arg("duration_ms").toInt());
    }
    if (g_http.hasArg("band_low_hz")) {
        g_config.band_low_hz = static_cast<uint16_t>(g_http.arg("band_low_hz").toInt());
    }
    if (g_http.hasArg("band_high_hz")) {
        g_config.band_high_hz = static_cast<uint16_t>(g_http.arg("band_high_hz").toInt());
    }
    if (g_http.hasArg("emit_gain_pct")) {
        const long v = g_http.arg("emit_gain_pct").toInt();
        if (v < 1 || v > 100) {
            send_json(400, "{\"error\":\"emit_gain_pct out of range 1..100\"}");
            return;
        }
        g_config.emit_gain_pct = static_cast<uint8_t>(v);
    }
    if (g_http.hasArg("quiesce_wifi")) {
        g_config.quiesce_wifi = (g_http.arg("quiesce_wifi") == "1" ||
                                 g_http.arg("quiesce_wifi") == "true");
    }

    listener_reset();
    log_line("config scenario=%u dur=%lu band=%u/%u gain=%u quiesce=%u",
             static_cast<unsigned>(g_config.scenario),
             static_cast<unsigned long>(g_config.duration_ms),
             static_cast<unsigned>(g_config.band_low_hz),
             static_cast<unsigned>(g_config.band_high_hz),
             static_cast<unsigned>(g_config.emit_gain_pct),
             static_cast<unsigned>(g_config.quiesce_wifi ? 1 : 0));
    send_json(200, "{\"ok\":true}");
}

void handle_run() {
    if (g_run_state == RUN_RUNNING) {
        send_json(409, "{\"error\":\"run in progress\"}");
        return;
    }
    g_log_count = 0;
    g_log_head = 0;
    g_log_dropped = 0;
    g_tagged_count = 0;
    g_counters = RunCounters{0, 0, 0, 0, 0};
    g_run_failure[0] = '\0';
    g_run_state = RUN_ARMED;
    log_line("armed scenario=%u", static_cast<unsigned>(g_config.scenario));
    send_json(200, "{\"ok\":true,\"state\":\"ARMED\"}");
}

void handle_stop() {
    if (g_run_state == RUN_RUNNING || g_run_state == RUN_ARMED) {
        g_run_state = RUN_STOPPED;
        g_run_ended_ms = millis();
        log_line("stopped by control plane");
    }
    send_json(200, "{\"ok\":true}");
}

void handle_result() {
    String j = "{";
    j += "\"run_state\":\"" + String(run_state_name(g_run_state)) + "\",";
    j += "\"scenario\":" + String(static_cast<unsigned>(g_config.scenario)) + ",";
    j += "\"started_ms\":" + String(g_run_started_ms) + ",";
    j += "\"ended_ms\":" + String(g_run_ended_ms) + ",";
    j += "\"zero_prior\":" + String(run_is_zero_prior() ? "true" : "false") + ",";
    j += "\"failure\":\"" + json_escape(g_run_failure) + "\",";
    j += "\"counters\":{";
    j += "\"frames_heard\":" + String(g_counters.frames_heard) + ",";
    j += "\"frames_recovered\":" + String(g_counters.frames_recovered) + ",";
    j += "\"frames_emitted\":" + String(g_counters.frames_emitted) + ",";
    j += "\"objects_decoded\":" + String(g_counters.objects_decoded) + ",";
    j += "\"samples_unscanned\":" + String(g_counters.samples_unscanned);
    j += "},";
    j += "\"values\":[";
    for (size_t i = 0; i < g_tagged_count; ++i) {
        if (i != 0) { j += ","; }
        j += "{\"name\":\"" + String(g_tagged[i].name) + "\",";
        j += "\"value\":" + String(g_tagged[i].value) + ",";
        j += "\"provenance\":\"" + String(provenance_name(g_tagged[i].provenance)) + "\"}";
    }
    j += "]}";
    send_json(200, j);
}

/* NDJSON: one object per line, so a long log streams without being assembled
   in RAM first. */
void handle_log() {
    g_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_http.send(200, "application/x-ndjson", "");

    const size_t start = (g_log_count < kLogEntries)
                       ? 0
                       : g_log_head;
    for (size_t n = 0; n < g_log_count; ++n) {
        const size_t i = (start + n) % kLogEntries;
        String line = "{\"at_ms\":" + String(g_log[i].at_ms) +
                      ",\"text\":\"" + json_escape(g_log[i].text) + "\"}\n";
        g_http.sendContent(line);
    }
    g_http.sendContent("");
}

void handle_root() {
    /* Deliberately tiny. No framework, no bundle, no camera. */
    static const char kPage[] PROGMEM =
        "<!doctype html><title>MCL autonomous node</title>"
        "<style>body{font:14px system-ui;margin:2rem;max-width:40rem}"
        "pre{background:#f4f4f4;padding:.75rem;overflow:auto}"
        "button{margin-right:.5rem}</style>"
        "<h1>MCL autonomous node</h1>"
        "<p>Lab control plane. Not part of any MCL exchange.</p>"
        "<button onclick=\"go('/api/run',1)\">run</button>"
        "<button onclick=\"go('/api/stop',1)\">stop</button>"
        "<button onclick=\"go('/api/status')\">status</button>"
        "<button onclick=\"go('/api/result')\">result</button>"
        "<button onclick=\"go('/api/log')\">log</button>"
        "<pre id=o>ready</pre>"
        "<script>function go(u,p){fetch(u,{method:p?'POST':'GET'})"
        ".then(r=>r.text()).then(t=>{document.getElementById('o').textContent=t})}"
        "</script>";
    g_http.send_P(200, "text/html", kPage);
}

void http_begin() {
    g_http.on("/", handle_root);
    g_http.on("/api/status", HTTP_GET, handle_status);
    g_http.on("/api/config", HTTP_POST, handle_config);
    g_http.on("/api/run", HTTP_POST, handle_run);
    g_http.on("/api/stop", HTTP_POST, handle_stop);
    g_http.on("/api/result", HTTP_GET, handle_result);
    g_http.on("/api/log", HTTP_GET, handle_log);
    g_http.begin();
}

/* ------------------------------------------------------------ scenarios */

/*
 * Scenario 0: LISTEN ONLY.
 *
 * The node listens for the run's duration and reports what it heard, keeping
 * QUIET / HEARD / CONTACT distinct. It emits nothing, so it is the scenario to
 * run while somebody else is measured, and it is the one that establishes the
 * room's own noise before anything is concluded from a failure.
 */
void scenario_listen_tick() {
    mcl_ap_listen_event_t event;
    uint8_t payload[kMaxBootstrapPayload];

    const mcl_ap_listen_result_t r =
        mcl_ap_listen_poll(&g_listener, &g_scratch, payload, sizeof(payload), &event);

    switch (r) {
        case MCL_AP_LISTEN_CONTACT: {
            ++g_counters.frames_recovered;
            log_line("CONTACT %u bytes at sample %llu",
                     static_cast<unsigned>(event.payload_bytes),
                     static_cast<unsigned long long>(event.stream_index));

            mcl_wire_tier0_t obj;
            size_t consumed = 0;
            const mcl_wire_status_t ws =
                mcl_wire_tier0_decode(payload, event.payload_bytes, &obj, &consumed);
            if (ws == MCL_WIRE_OK) {
                ++g_counters.objects_decoded;
                log_line("  object kind=%u source_ref=%lu consumed=%u",
                         static_cast<unsigned>(obj.kind),
                         static_cast<unsigned long>(obj.source_ref),
                         static_cast<unsigned>(consumed));
                /* Learned from the air. This is the tag that makes the run
                   zero-prior rather than the comment above it. */
                tag_value("peer_source_ref", obj.source_ref, PROV_FROM_AIR);
            } else {
                log_line("  object decode refused rc=%ld", static_cast<long>(ws));
            }
            break;
        }
        case MCL_AP_LISTEN_HEARD:
            ++g_counters.frames_heard;
            log_line("HEARD (preamble, payload lost) modem_rc=%ld",
                     static_cast<long>(event.modem_status));
            break;
        case MCL_AP_LISTEN_WAITING:
        case MCL_AP_LISTEN_QUIET:
        default:
            break;
    }
}

/*
 * Scenario 1: ANNOUNCE AND LISTEN.
 *
 * Emit a PRESENCE at a randomised cadence and listen the rest of the time.
 * This is the smallest scenario that puts two of these boards in a room and
 * lets them hear each other with no host deciding who speaks.
 */
uint32_t g_next_announce_ms = 0;

void schedule_next_announce() {
    /* Randomised so two boards do not lock into the same cadence. Bounded
       randomness matters here for the same reason it does in contention. */
    uint32_t r = esp_random();
    g_next_announce_ms = millis() + 3000u + (r % 4000u);
}

void scenario_announce_tick() {
    scenario_listen_tick();

    if (static_cast<int32_t>(millis() - g_next_announce_ms) < 0) { return; }

    mcl_wire_tier0_t obj;
    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.source_ref = g_source_ref;
    /* capability_tag is a sender-controlled opaque revision token. It must
       change when advertised capabilities change and must never be compared
       across peers; a constant is correct for a node whose capabilities are
       fixed for the run. ttl 0 is "no stated validity". */
    obj.body.presence.capability_tag = 1u;
    obj.body.presence.ttl = 0u;

    uint8_t buf[kMaxBootstrapPayload];
    size_t written = 0;
    const mcl_wire_status_t ws =
        mcl_wire_tier0_encode_at_major(MCL_WIRE_STABLE_MAJOR, &obj,
                                       buf, sizeof(buf), &written);
    if (ws != MCL_WIRE_OK) {
        log_line("PRESENCE encode refused rc=%ld", static_cast<long>(ws));
        schedule_next_announce();
        return;
    }

    (void)emit_payload(buf, written);
    schedule_next_announce();
}

void scenario_start() {
    g_run_started_ms = millis();
    g_run_ended_ms = 0;
    g_run_state = RUN_RUNNING;

    /* This node's own reference is generated here, not configured. */
    tag_value("own_source_ref", g_source_ref, PROV_LOCAL);

    listener_reset();
    schedule_next_announce();

    if (g_config.quiesce_wifi) {
        /*
         * Taking Wi-Fi down is not tidiness. It removes the control plane from
         * the air for the duration of the exchange, so a result cannot depend
         * on it, and it returns the radio's memory to a node whose audio
         * buffers are the reason this firmware is tight in the first place.
         */
        wifi_down();
    }
    log_line("run start scenario=%u duration=%lu quiesce=%u",
             static_cast<unsigned>(g_config.scenario),
             static_cast<unsigned long>(g_config.duration_ms),
             static_cast<unsigned>(g_config.quiesce_wifi ? 1 : 0));
}

void scenario_end(uint8_t final_state, const char *why) {
    g_run_ended_ms = millis();
    g_run_state = final_state;
    if (why != nullptr) {
        snprintf(g_run_failure, sizeof(g_run_failure), "%s", why);
    }
    g_counters.samples_unscanned =
        static_cast<uint32_t>(g_listener.samples_unscanned);
    log_line("run end state=%s heard=%lu recovered=%lu emitted=%lu unscanned=%lu",
             run_state_name(final_state),
             static_cast<unsigned long>(g_counters.frames_heard),
             static_cast<unsigned long>(g_counters.frames_recovered),
             static_cast<unsigned long>(g_counters.frames_emitted),
             static_cast<unsigned long>(g_counters.samples_unscanned));

    if (g_config.quiesce_wifi) {
        wifi_up();
        http_begin();
        log_line("wifi restored; result available");
    }
}

void scenario_tick() {
    if (static_cast<int32_t>(millis() - (g_run_started_ms + g_config.duration_ms)) >= 0) {
        scenario_end(RUN_DONE, nullptr);
        return;
    }

    capture_pump();

    switch (g_config.scenario) {
        case 0: scenario_listen_tick(); break;
        case 1: scenario_announce_tick(); break;
        default:
            scenario_end(RUN_FAILED, "unknown scenario");
            break;
    }
}

}  /* namespace */

/* ------------------------------------------------------------------ boot */

void setup() {
    pinMode(kActivityLedPin, OUTPUT);
    digitalWrite(kActivityLedPin, LOW);

    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(500);
    Serial.begin(921600);
    const uint32_t waited = millis();
    while (!Serial && millis() - waited < 3000) { delay(10); }

    /* Randomised at boot: see the note at g_source_ref. */
    g_source_ref = esp_random();

    microphone.setPinsPdmRx(kPdmClockPin, kPdmDataPin);
    g_microphone_ready = microphone.begin(I2S_MODE_PDM_RX, kSampleRateHz,
                                          I2S_DATA_BIT_WIDTH_16BIT,
                                          I2S_SLOT_MODE_MONO);
    if (!g_microphone_ready) { Serial.println("MCLAUTO WARN PDM_INIT_FAILED"); }

    speaker.setPins(kAmpBclkPin, kAmpLrclkPin, kAmpDataPin);
    g_speaker_ready = speaker.begin(I2S_MODE_STD, kSampleRateHz,
                                    I2S_DATA_BIT_WIDTH_16BIT,
                                    I2S_SLOT_MODE_MONO);
    if (!g_speaker_ready) { Serial.println("MCLAUTO WARN AMP_INIT_FAILED"); }

    listener_reset();

    wifi_up();
    http_begin();

    Serial.printf("MCLAUTO READY wire_major=%u link_major=%u arena=%u scratch=%u "
                  "free_heap=%lu source_ref=%lu\n",
                  static_cast<unsigned>(MCL_WIRE_STABLE_MAJOR),
                  static_cast<unsigned>(MCL_LINK_STABLE_MAJOR),
                  static_cast<unsigned>(sizeof(g_arena)),
                  static_cast<unsigned>(sizeof(g_scratch)),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(g_source_ref));
}

void loop() {
    if (g_wifi_up) { g_http.handleClient(); }

    switch (g_run_state) {
        case RUN_ARMED:
            scenario_start();
            break;
        case RUN_RUNNING:
            scenario_tick();
            break;
        default:
            /* Idle nodes still listen, so a board left powered reports what
               the room is doing rather than sitting deaf until armed. */
            capture_pump();
            delay(1);
            break;
    }
}

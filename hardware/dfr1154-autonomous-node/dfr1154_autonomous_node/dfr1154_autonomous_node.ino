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
 *      nowhere to put a peer IP, a UDP port, a BLE address, a source_ref, an
 *      endpoint_token, a migration_ref, a session_ref, a secret or a pairing
 *      state. This is a good fence and it is only as strong as the next person
 *      to edit the struct.
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
 *   - Stranger discovery over IP. The UDP scenario listens on a port of its
 *     own and answers whoever writes to it. It learns the peer's address from
 *     the datagram rather than being told it, which keeps the run zero-prior,
 *     and it is still NOT stranger discovery: MCL defines no global discovery
 *     port and this rig does not invent one.
 *
 * MEMORY, WHICH DECIDED THE DESIGN, AND WAS MEASURED RATHER THAN ESTIMATED
 *
 * See spike-ble-memory/README.md for the full campaign. Three results shape
 * this file:
 *
 *   ONE UNION ARENA. The receive path needs the listener's window AND the
 *   modem's scratch at the same time; the transmit path needs neither, only a
 *   waveform. Sized separately they are 209 988 bytes and the image does not
 *   link with a BLE stack -- `.dram0.bss will not fit`. Sized as one block by
 *   the larger USE rather than the sum, they are 171 588, and it links at
 *   221 820 bytes of static RAM.
 *
 *   THE ARENA IS STATIC, NOT HEAP, AND THAT COST US THE EASY OPTION. On the
 *   heap it is far more comfortable -- BLE even comes up with Wi-Fi still
 *   running -- but it cannot be taken BACK afterwards: after the radios have
 *   run, 265 KB free contains no 168 KB block, and tearing the BLE stack down
 *   completely does not repair it. A node that can listen once and never again
 *   is not a node.
 *
 *   WI-FI AND BLE ARE EXCLUSIVE HERE. Not a preference: `BLEDevice::init()` is
 *   refused with the SoftAP up in this layout, and succeeds once it is down.
 *   The `quiesce_wifi` option was added for an evidence reason -- a control
 *   plane on the air during an exchange contaminates the result -- and the
 *   measurement says it is also the only way the second radio comes up at all.
 *   The honesty requirement and the physical constraint want the same thing.
 *
 *   PSRAM IS STILL NOT USED. The board has 8 MB and it is the obvious way out.
 *   Experiment 008 declined it for a reason that still holds: the correlation
 *   inner loop reads the window tens of millions of times per acquisition, and
 *   this project has never verified the QSPI/OPI mode option for this part. A
 *   rig that boots differently depending on a board option nobody checked is
 *   not an instrument.
 *
 * THE PHASES, AND WHY THEY ARE EXPLICIT
 *
 *   CONTROL     Wi-Fi + HTTP up. Armed from here. No MCL traffic.
 *   QUIESCE     Wi-Fi down. The control plane leaves the air and returns its
 *               memory before either radio is asked for anything.
 *   RENDEZVOUS  AP-BOOTSTRAP-1: PRESENCE, contention, OFFER/ACCEPT.
 *   ACTIVATE    BLE-ACTIVATE-1: the offerer advertises, the acceptor scans and
 *               connects. Roles are DERIVED from the wire, never chosen.
 *   VALIDATE    PATH_CHALLENGE / PATH_RESPONSE on the candidate.
 *   POLICY      admit or refuse. Before the first irrevocable act on each side.
 *   MIGRATE     COMMIT / CONFIRM.
 *   TEARDOWN    radios down.
 *   REPORT      Wi-Fi + HTTP restored, result readable.
 *
 * A phase machine rather than a flag soup because the memory budget makes the
 * ordering load-bearing: two radios cannot be up together, so "which one is up
 * now" is not an implementation detail.
 *
 * SAFETY
 *
 * Flash the APPLICATION PARTITION ONLY, at 0x20000, with esptool. Do not
 * upload through the Arduino CLI: its upload step can also rewrite the
 * bootloader and the partition table. build-firmware.ps1 builds without
 * uploading and refuses to run if the factory backup is missing.
 */

#include <Arduino.h>
#include <atomic>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include "ESP_I2S.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "host/ble_gap.h"

#include "mcl/ap_listen.h"
#include "mcl/ap_modem.h"
#include "mcl/wire.h"
#include "mcl/link.h"
#include "mcl/contact.h"
#include "mcl/sdk.h"
#include "mcl/machine.h"
#include "mcl/ble_binding.h"
#include "mcl/ip_binding.h"

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
 * ONE BLOCK, SIZED BY THE LARGER USE RATHER THAN THE SUM.
 *
 *   receiving    window 94 720 + modem scratch 76 868 = 171 588  <- the larger
 *   transmitting waveform for a 17-byte object        = 133 120
 *
 * Transmit and receive never overlap: while this machine's speaker is driven
 * its microphone hears its own emission and nothing else, which is why
 * mcl_rdv_platform_t has self_transmitting at all. So the transmit waveform is
 * allowed to sit on top of both, with the listener reset either side. What
 * that costs is any frame half-buffered when we start talking -- which the
 * physics was going to take anyway.
 *
 * 47 360 samples is the MINIMUM window for a 17-byte payload, not a chosen
 * one, and mcl_ap_listen_min_window_samples() is asked at boot rather than
 * trusted from this comment.
 */
constexpr size_t kListenSamples   = 47360;
constexpr size_t kWindowBytes     = kListenSamples * sizeof(int16_t);
constexpr size_t kTxWaveformBytes = 133120;
constexpr size_t kScratchOffset   = kWindowBytes;   /* 4-byte aligned */
constexpr size_t kArenaBytes =
    (kWindowBytes + sizeof(mcl_ap_modem_scratch_t) > kTxWaveformBytes)
        ? (kWindowBytes + sizeof(mcl_ap_modem_scratch_t))
        : kTxWaveformBytes;

alignas(8) uint8_t g_arena[kArenaBytes];

int16_t *window_ptr() { return reinterpret_cast<int16_t *>(g_arena); }
int16_t *waveform_ptr() { return reinterpret_cast<int16_t *>(g_arena); }
constexpr size_t kWaveformSamples = kArenaBytes / sizeof(int16_t);

mcl_ap_modem_scratch_t *scratch_ptr() {
    return reinterpret_cast<mcl_ap_modem_scratch_t *>(g_arena + kScratchOffset);
}

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

constexpr size_t kMaxTagged = 16;
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
constexpr size_t kLogTextMax = 88;

struct LogEntry {
    uint32_t at_ms;
    char     text[kLogTextMax];
};

/* Cold evidence storage, not the decoder's hot working set. Reserving this
   in PSRAM leaves internal memory for the BLE worker and controller. */
LogEntry *g_log = nullptr;
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
 * a peer IP, a UDP port belonging to somebody else, a BLE address, a
 * source_ref, an endpoint_token, a migration_ref, a session_ref, a secret or a
 * pairing state. Adding one makes every zero-prior claim this node produces
 * false, and if you add one anyway, tag the value PROV_CONFIGURED so the log
 * says so.
 *
 * `candidate_transport` names a MEDIUM, not a peer: which bearer this
 * deployment offers as the continuation. That is a deployment-profile
 * decision, and it tells this node nothing about who is on the other end.
 */
struct ScenarioConfig {
    uint8_t  scenario;            /* which scenario to run */
    uint32_t duration_ms;         /* bound on the whole run */
    uint16_t band_low_hz;         /* 0 = modem default */
    uint16_t band_high_hz;
    uint8_t  emit_gain_pct;       /* transmit level */
    bool     quiesce_wifi;        /* take Wi-Fi down for the run */
    uint8_t  candidate_transport; /* 3 = BLE (MCL_CONTACT_TRANSPORT_BLE) */
    uint8_t  admit_policy;        /* 1 = admit, 0 = refuse. Local policy. */
};

ScenarioConfig g_config = {
    /* scenario            */ 0,
    /* duration_ms         */ 60000,
    /* band_low_hz         */ 0,
    /* band_high_hz        */ 0,
    /* emit_gain_pct       */ 100,
    /* quiesce_wifi        */ false,
    /* candidate_transport */ MCL_CONTACT_TRANSPORT_BLE,
    /* admit_policy        */ 1
};

/*
 * ARMING GOES THROUGH A REBOOT, AND THAT IS A MEMORY DECISION.
 *
 * Wi-Fi does not give back everything it takes. Measured on this board: 113 468
 * bytes of heap at boot, 54 796 with the SoftAP up, and 92 156 after it is shut
 * down again -- about 21 KB stays with the network stack underneath the driver,
 * and `esp_wifi_deinit()` answers ESP_ERR_WIFI_NOT_INIT because the driver has
 * already gone. Bluedroid needs roughly 70 KB. With this firmware's static
 * footprint, a run armed over HTTP would enter its BLE phase about 2 KB short
 * of bringing the radio up at all.
 *
 * So the armed configuration is written to RTC memory, which survives a
 * software reset, and the node restarts INTO the run. Every run therefore
 * begins from a boot where Wi-Fi has never been initialised, whichever control
 * plane armed it, and the memory state of a run does not depend on how it was
 * started. The alternative -- "arm over serial for BLE runs, HTTP for the rest"
 * -- makes the result depend on the instrument, which is the class of mistake
 * this rig exists to avoid.
 */
RTC_NOINIT_ATTR uint32_t g_armed_magic;
RTC_NOINIT_ATTR uint8_t  g_armed_blob[32];
constexpr uint32_t kArmedMagic = 0x4D434C41u;   /* "MCLA" */

/*
 * THE POST-MORTEM. A node with no serial cable cannot say "I crashed".
 *
 * This board is driven over its own SoftAP for exactly the runs that cannot
 * coexist with a USB host, which means the operator sees the node only after
 * it has come back. A run that panics and a run that finished both present as
 * RUN_IDLE with a short uptime and an empty log, and reading the first as the
 * second is how a rig manufactures a passing result out of a crash.
 *
 * So the live run footprint is mirrored into RTC memory as it changes, and
 * read back one boot later beside esp_reset_reason(). If the previous boot was
 * inside a run and did not reach an ending, the node says so on /api/status
 * and the evidence is thrown away rather than filed.
 */
RTC_NOINIT_ATTR uint32_t g_post_magic;
RTC_NOINIT_ATTR uint8_t  g_post_scenario;
RTC_NOINIT_ATTR uint8_t  g_post_phase;
RTC_NOINIT_ATTR uint8_t  g_post_run_state;
RTC_NOINIT_ATTR uint8_t  g_post_ble_role;
RTC_NOINIT_ATTR uint32_t g_post_uptime_ms;
RTC_NOINIT_ATTR uint32_t g_post_free_heap;
constexpr uint32_t kPostMagic = 0x4D434C50u;   /* "MCLP" */

/* The previous boot's footprint, copied out before this boot overwrites it. */
bool     g_prev_valid = false;
bool     g_prev_incomplete = false;
uint8_t  g_prev_scenario = 0;
uint8_t  g_prev_phase = 0;
uint8_t  g_prev_run_state = 0;
uint8_t  g_prev_ble_role = 0;
uint32_t g_prev_uptime_ms = 0;
uint32_t g_prev_free_heap = 0;
int      g_reset_reason = 0;

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

/* The phase machine. See the header: with two radios that cannot be up at the
   same time, "which one is up now" is protocol-visible, not bookkeeping. */
enum Phase : uint8_t {
    PHASE_CONTROL = 0,
    PHASE_QUIESCE = 1,
    PHASE_RENDEZVOUS = 2,
    PHASE_ACTIVATE = 3,
    PHASE_VALIDATE = 4,
    PHASE_POLICY = 5,
    PHASE_MIGRATED = 6,
    PHASE_TEARDOWN = 7,
    PHASE_REPORT = 8
};

const char *phase_name(uint8_t p) {
    switch (p) {
        case PHASE_CONTROL:    return "CONTROL";
        case PHASE_QUIESCE:    return "QUIESCE";
        case PHASE_RENDEZVOUS: return "RENDEZVOUS";
        case PHASE_ACTIVATE:   return "ACTIVATE";
        case PHASE_VALIDATE:   return "VALIDATE";
        case PHASE_POLICY:     return "POLICY";
        case PHASE_MIGRATED:   return "MIGRATED";
        case PHASE_TEARDOWN:   return "TEARDOWN";
        case PHASE_REPORT:     return "REPORT";
        default:               return "UNKNOWN";
    }
}

volatile uint8_t g_run_state = RUN_IDLE;
bool g_finish_pending = false;
uint8_t g_finish_state = RUN_STOPPED;
uint8_t  g_phase = PHASE_CONTROL;
uint32_t g_run_started_ms = 0;
uint32_t g_run_ended_ms = 0;
char     g_run_failure[64] = {0};

void set_phase(uint8_t phase) {
    if (g_phase == phase) { return; }
    g_phase = phase;
    log_line("phase %s free_heap=%lu", phase_name(phase),
             static_cast<unsigned long>(ESP.getFreeHeap()));
}

/* Counters that make a run readable without parsing every log line. */
struct RunCounters {
    uint32_t frames_heard;      /* preamble found, payload lost */
    uint32_t frames_recovered;  /* CRC verified */
    uint32_t frames_emitted;
    uint32_t objects_decoded;
    uint32_t samples_unscanned; /* audio discarded before it was searched */
    uint32_t ble_frames_tx;
    uint32_t ble_frames_rx;
    uint32_t ble_frag_rejected;
    uint32_t scan_matches;      /* advertisements matching UUID *and* beacon */
    uint32_t scan_uuid_only;    /* right protocol, wrong transaction */
    uint32_t scan_slices;       /* scan restarts; see ble_become_central */
    /*
     * EVERY advertisement handed to the callback, MCL or not. Without this a
     * scan that survives proves nothing: a quiet room and a fixed leak look
     * identical from the outside, and the retention path this firmware had to
     * fix is driven by foreign advertisements, not by MCL ones.
     */
    uint32_t scan_seen;
};
RunCounters g_counters = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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
void http_begin();

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
    /* The driver releases its heap asynchronously. Without this the next
       measurement is taken before the shutdown has finished, and a BLE bring-up
       attempted immediately after can be refused for memory that was about to
       arrive. */
    delay(500);
    log_line("wifi down (quiesced) free_heap=%lu",
             static_cast<unsigned long>(ESP.getFreeHeap()));
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
        mcl_ap_listen_init(&g_listener, &g_listen_config, window_ptr(), kListenSamples);
    if (st != MCL_AP_LISTEN_OK) {
        log_line("listener init refused rc=%ld", static_cast<long>(st));
    }
}

bool listener_prepare() {
    const uint32_t started = millis();
    const mcl_ap_modem_status_t st =
        mcl_ap_modem_prepare(&g_listen_config.modem, scratch_ptr());
    log_line("listener prepare rc=%ld ms=%lu",
             static_cast<long>(st),
             static_cast<unsigned long>(millis() - started));
    return st == MCL_AP_MODEM_OK;
}

/*
 * Emit one payload acoustically.
 *
 * The listener is reset either side, because the arena it is looking at is
 * about to become the waveform -- the union layout in the header is what makes
 * that necessary and what makes the image link. Everything buffered is
 * discarded, which is the truth about what a microphone hears while its own
 * speaker is driven.
 */
void trace_ap(const char *direction, const uint8_t *bytes, size_t size) {
    Serial.printf("MCLTRACE ms=%lu AP %s bytes=%u hex=", static_cast<unsigned long>(millis()), direction, static_cast<unsigned>(size));
    for (size_t i = 0; i < size; ++i) { Serial.printf("%02X", bytes[i]); }
    Serial.println();
}

bool emit_payload(const uint8_t *payload, size_t len) {
    trace_ap("TX_ATTEMPT", payload, len);
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
        mcl_ap_modem_encode(&modem, payload, len, waveform_ptr(), kWaveformSamples, &used);
    if (st != MCL_AP_MODEM_OK) {
        g_transmitting = false;
        log_line("emit refused: modulate rc=%ld", static_cast<long>(st));
        mcl_ap_modem_scratch_invalidate(scratch_ptr());
        listener_reset();
        (void)listener_prepare();
        return false;
    }

    if (g_config.emit_gain_pct < 100u) {
        const int32_t g = static_cast<int32_t>(g_config.emit_gain_pct);
        int16_t *w = waveform_ptr();
        for (size_t i = 0; i < used; ++i) {
            w[i] = static_cast<int16_t>((static_cast<int32_t>(w[i]) * g) / 100);
        }
    }

    digitalWrite(kActivityLedPin, HIGH);
    speaker.write(reinterpret_cast<uint8_t *>(waveform_ptr()),
                  used * sizeof(int16_t));
    digitalWrite(kActivityLedPin, LOW);
    g_transmitting = false;

    ++g_counters.frames_emitted;
    log_line("emitted %u bytes, %u samples", static_cast<unsigned>(len),
             static_cast<unsigned>(used));

    /* The transmit waveform shares the union arena with the receive scratch.
       Its first 38 KB overwrite the cached reference while leaving the cache
       magic at the far end untouched, so the overlap must be made explicit. */
    mcl_ap_modem_scratch_invalidate(scratch_ptr());
    listener_reset();
    (void)listener_prepare();
    return true;
}

/*
 * Pull whatever the microphone has and offer it to the listener. Does no
 * correlation, so it is cheap and can be called from the main loop at any
 * cadence; being late costs samples_unscanned, which is counted rather than
 * hidden.
 */
/*
 * THE CAPTURE LEVEL METER.
 *
 * heard=0 is not a diagnosis. A microphone that never delivers a sample, a
 * transmitter pointed the other way, and a receiver configured for a band
 * nobody is transmitting in all report exactly zero, and this project has
 * already lost time to the third of those. Peak and mean amplitude separate
 * them: near-silence means the input path is not delivering anything to
 * disagree with, and a healthy level with no acquisition means the signal is
 * arriving and the receiver is rejecting it.
 *
 * These are counters over the run, not a signal quality measure, and they are
 * deliberately not called SNR. They say how loud the room was, nothing more.
 */
/* Where the loop's time actually goes, in milliseconds. */
uint32_t g_read_ms_total = 0;
uint32_t g_read_ms_max = 0;
uint32_t g_poll_ms_total = 0;
uint32_t g_poll_ms_max = 0;
uint32_t g_poll_calls = 0;

uint32_t g_cap_blocks = 0;
uint32_t g_cap_samples = 0;
uint16_t g_cap_peak = 0;
uint64_t g_cap_abs_sum = 0;

/* A completed run is a receipt, not a live room meter.  Idle nodes continue
   sampling by design, so the four values exposed by /api/result are sealed at
   scenario_end() instead of aliasing counters that keep moving afterward. */
uint32_t g_run_cap_blocks = 0;
uint32_t g_run_cap_samples = 0;
uint16_t g_run_cap_peak = 0;
uint32_t g_run_cap_mean = 0;

/*
 * AUDIO ACQUISITION IS A PRODUCER, DECODING IS A CONSUMER.
 *
 * A valid frame can send the portable decoder through its whole-frame timing
 * retry for roughly a second on this MCU. Reading I2S from the same loop makes
 * the microphone deaf for that second no matter how fast the quiet detector
 * is. The board carries 8 MB of embedded OPI PSRAM, so a dedicated task keeps
 * draining DMA into a 2.73 s SPSC queue while the internal-RAM modem works.
 * PSRAM is used only for sequential captured PCM; the correlation reference,
 * listener window and scratch remain in internal DRAM.
 */
constexpr size_t kCaptureRingSamples = 131072u;
int16_t *g_capture_ring = nullptr;
constexpr size_t kCaptureConsumerSamples = 1024u;
int16_t *g_capture_consumer_block = nullptr;
volatile uint32_t g_capture_write = 0;
volatile uint32_t g_capture_read = 0;
volatile uint32_t g_capture_dropped = 0;
volatile bool g_capture_active = false;
TaskHandle_t g_capture_task = nullptr;
portMUX_TYPE g_capture_mux = portMUX_INITIALIZER_UNLOCKED;

size_t capture_queued() {
    size_t queued;
    portENTER_CRITICAL(&g_capture_mux);
    queued = static_cast<size_t>(g_capture_write - g_capture_read);
    portEXIT_CRITICAL(&g_capture_mux);
    return queued;
}

void capture_queue_reset() {
    portENTER_CRITICAL(&g_capture_mux);
    g_capture_read = g_capture_write;
    g_capture_dropped = 0;
    portEXIT_CRITICAL(&g_capture_mux);
}

void capture_level_reset() {
    g_read_ms_total = 0;
    g_read_ms_max = 0;
    g_poll_ms_total = 0;
    g_poll_ms_max = 0;
    g_poll_calls = 0;
    g_cap_blocks = 0;
    g_cap_samples = 0;
    g_cap_peak = 0;
    g_cap_abs_sum = 0;
    capture_queue_reset();
}

void capture_task_main(void *) {
    /* A task-local handoff block. The persistent queue lives in PSRAM. */
    static int16_t block[1024];
    for (;;) {
        if (!g_capture_active || !g_microphone_ready || g_transmitting ||
            g_capture_ring == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const uint32_t read_started = millis();
        const int got = microphone.readBytes(reinterpret_cast<char *>(block),
                                             static_cast<int>(sizeof(block)));
        const uint32_t read_ms = millis() - read_started;
        if (got <= 0) { continue; }
        const size_t n = static_cast<size_t>(got) / sizeof(int16_t);

        uint16_t block_peak = 0;
        uint64_t block_sum = 0;
        for (size_t i = 0; i < n; ++i) {
            const int32_t v = block[i];
            const uint32_t a = static_cast<uint32_t>((v < 0) ? -v : v);
            if (a > block_peak) { block_peak = static_cast<uint16_t>(a); }
            block_sum += a;
        }

        portENTER_CRITICAL(&g_capture_mux);
        g_read_ms_total += read_ms;
        if (read_ms > g_read_ms_max) { g_read_ms_max = read_ms; }
        ++g_cap_blocks;
        g_cap_samples += static_cast<uint32_t>(n);
        if (block_peak > g_cap_peak) { g_cap_peak = block_peak; }
        g_cap_abs_sum += block_sum;

        size_t used = static_cast<size_t>(g_capture_write - g_capture_read);
        if (used + n > kCaptureRingSamples) {
            const size_t drop = used + n - kCaptureRingSamples;
            g_capture_read += static_cast<uint32_t>(drop);
            g_capture_dropped += static_cast<uint32_t>(drop);
        }
        size_t at = static_cast<size_t>(g_capture_write)
                    & (kCaptureRingSamples - 1u);
        const size_t first = (n < kCaptureRingSamples - at)
                                 ? n : kCaptureRingSamples - at;
        memcpy(g_capture_ring + at, block, first * sizeof(int16_t));
        if (first < n) {
            memcpy(g_capture_ring, block + first,
                   (n - first) * sizeof(int16_t));
        }
        g_capture_write += static_cast<uint32_t>(n);
        portEXIT_CRITICAL(&g_capture_mux);
    }
}

void capture_pump() {
    if (g_capture_ring == nullptr || g_capture_consumer_block == nullptr) { return; }
    /* Cold handoff staging, copied into the unchanged internal listener arena.
       Reserve scarce DMA-capable internal RAM for the BLE controller. */
    int16_t *block = g_capture_consumer_block;
    size_t n;

    portENTER_CRITICAL(&g_capture_mux);
    n = static_cast<size_t>(g_capture_write - g_capture_read);
    if (n > kCaptureConsumerSamples) {
        n = kCaptureConsumerSamples;
    }
    if (n != 0u) {
        const size_t at = static_cast<size_t>(g_capture_read)
                          & (kCaptureRingSamples - 1u);
        const size_t first = (n < kCaptureRingSamples - at)
                                 ? n : kCaptureRingSamples - at;
        memcpy(block, g_capture_ring + at, first * sizeof(int16_t));
        if (first < n) {
            memcpy(block + first, g_capture_ring,
                   (n - first) * sizeof(int16_t));
        }
        g_capture_read += static_cast<uint32_t>(n);
    }
    portEXIT_CRITICAL(&g_capture_mux);

    if (n != 0u) {
        (void)mcl_ap_listen_push(&g_listener, block, n);
    }
}

/* ============================================================ BLE
 *
 * BLE-GATT-1 carries frames over an ESTABLISHED connection. BLE-ACTIVATE-1
 * says how two strangers reach one, and the rule that makes it work between
 * builders who never coordinate is that NOBODY CHOOSES A ROLE:
 *
 *   only TRANSPORT_OFFER carries an endpoint_token, and it is the offerer's
 *   own. So the offerer is the only peer that can be FOUND, and it therefore
 *   advertises and is the GATT peripheral. The acceptor holds that token, so
 *   it scans for it and connects as the GATT central.
 *
 * Any other assignment needs information the wire does not carry.
 */

#define MCL_SERVICE_UUID "6d636c00-0001-4d43-4c00-6d636c626c65"
#define MCL_RX_CHAR_UUID "6d636c00-0002-4d43-4c00-6d636c626c65"
#define MCL_TX_CHAR_UUID "6d636c00-0003-4d43-4c00-6d636c626c65"

enum BleRole : uint8_t {
    BLE_ROLE_NONE = 0,
    BLE_ROLE_PERIPHERAL = 1,   /* offerer: advertises, serves */
    BLE_ROLE_CENTRAL = 2       /* acceptor: scans, connects */
};

const char *ble_role_name(uint8_t r) {
    switch (r) {
        case BLE_ROLE_PERIPHERAL: return "PERIPHERAL_OFFERER";
        case BLE_ROLE_CENTRAL:    return "CENTRAL_ACCEPTOR";
        default:                  return "NONE";
    }
}

bool     g_ble_up = false;
uint8_t  g_ble_role = BLE_ROLE_NONE;
std::atomic<bool> g_ble_connected{false};
std::atomic<bool> g_ble_subscribed{false};

BLEServer            *g_server   = nullptr;
BLECharacteristic    *g_tx_char  = nullptr;   /* peripheral -> central, notify */
BLEClient            *g_client   = nullptr;
BLERemoteCharacteristic *g_remote_rx = nullptr;  /* central -> peripheral, write */

mcl_ble_reassembler_t g_reasm;

/*
 * Frames arrive on a BLE callback task and are processed in loop(). A single
 * mailbox rather than a queue: this protocol is strictly sequential on the
 * candidate, one control at a time, and a queue would only hide a scheduling
 * bug behind a buffer.
 */
/* Publish the payload across the BLE callback and loop tasks. Volatile alone
 * does not make the payload writes visible before the ready flag. */
std::atomic<bool> g_ble_frame_ready{false};
size_t        g_ble_frame_size = 0;
uint8_t       g_ble_frame[MCL_LINK_FRAME_MAX_SIZE];
uint32_t      g_ble_frames_lost = 0;   /* mailbox occupied when a frame landed */

void ble_deliver_frame(const uint8_t *frame, size_t size) {
    if (size == 0u || size > sizeof(g_ble_frame)) { return; }
    if (g_ble_frame_ready) {
        /* Counted, never silently dropped: a lost handoff control looks
           exactly like a peer that never answered. */
        ++g_ble_frames_lost;
        return;
    }
    memcpy(g_ble_frame, frame, size);
    g_ble_frame_size = size;
    g_ble_frame_ready = true;
    ++g_counters.ble_frames_rx;
}

class NodeServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        g_ble_subscribed = false;
        g_ble_connected = true;
        mcl_ble_reassembler_reset(&g_reasm);
    }
    void onDisconnect(BLEServer *s) override {
        g_ble_subscribed = false;
        g_ble_connected = false;
        /* Keep advertising: the activation window may still be open and the
           acceptor is entitled to retry. */
        s->startAdvertising();
    }
};

class NodeTxCallbacks : public BLECharacteristicCallbacks {
    void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *, uint16_t sub_value) override {
        g_ble_subscribed = (sub_value & 1u) != 0u;
    }
};
NodeTxCallbacks g_tx_callbacks;

class NodeRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        String v = c->getValue();
        const uint8_t *data = reinterpret_cast<const uint8_t *>(v.c_str());
        const size_t len = v.length();
        size_t frame_size = 0;

        if (len == 0u) { return; }

        const mcl_ble_status_t st =
            mcl_ble_reassemble(&g_reasm, data, len, &frame_size);
        if (st == MCL_BLE_OK) {
            ble_deliver_frame(g_reasm.buffer, frame_size);
        } else if (st == MCL_BLE_ERR_INCOMPLETE) {
            /* Normal: more fragments are expected. */
        } else {
            ++g_counters.ble_frag_rejected;
        }
    }
};

NodeServerCallbacks g_server_callbacks;
NodeRxCallbacks     g_rx_callbacks;

void client_notify_cb(BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    size_t frame_size = 0;
    if (length == 0u) { return; }
    const mcl_ble_status_t st =
        mcl_ble_reassemble(&g_reasm, data, length, &frame_size);
    if (st == MCL_BLE_OK) {
        ble_deliver_frame(g_reasm.buffer, frame_size);
    } else if (st != MCL_BLE_ERR_INCOMPLETE) {
        ++g_counters.ble_frag_rejected;
    }
}

/* ------------------------------------------------------- the beacon match */

/*
 * BLE-ACTIVATE-1 §3. The beacon is the endpoint_token, zero-extended and
 * BIG-ENDIAN, in the 8 bytes of Service Data for the MCL 128-bit UUID:
 *
 *   AD structure: length(1) | type 0x21 | UUID(16, little-endian) | beacon(8)
 *
 * A scanner MUST match on the UUID *and* the full beacon, and MUST NOT connect
 * to an advertiser that matches only the UUID. The UUID names the protocol;
 * the beacon names the transaction.
 */
void beacon_from_token(uint32_t token, uint8_t out[MCL_RENDEZVOUS_BEACON_SIZE]) {
    memset(out, 0, MCL_RENDEZVOUS_BEACON_SIZE);
    out[4] = static_cast<uint8_t>((token >> 24) & 0xFFu);
    out[5] = static_cast<uint8_t>((token >> 16) & 0xFFu);
    out[6] = static_cast<uint8_t>((token >> 8) & 0xFFu);
    out[7] = static_cast<uint8_t>(token & 0xFFu);
}

const uint8_t kServiceUuidLe[MCL_BLE_SERVICE_UUID_SIZE] = MCL_BLE_SERVICE_UUID_BYTES;

/*
 * Walk the raw advertising payload rather than asking the library for
 * "service data".
 *
 * The library's accessor would answer for a structure that merely resembles
 * this one. What has to be true for two independent implementations to
 * interoperate is that the exact 26 bytes are on the air in the exact order,
 * so the raw AD is what is inspected -- and what is logged when a scan finds
 * the right protocol but the wrong transaction.
 */
bool advertisement_matches(const uint8_t *payload, size_t len,
                           const uint8_t *want_beacon, bool *uuid_seen) {
    size_t i = 0;
    *uuid_seen = false;
    while (i + 1u < len) {
        const uint8_t field_len = payload[i];
        if (field_len == 0u || i + 1u + field_len > len) { break; }
        const uint8_t type = payload[i + 1u];
        const uint8_t *value = &payload[i + 2u];
        const size_t value_len = static_cast<size_t>(field_len) - 1u;

        if (type == MCL_BLE_AD_TYPE_SERVICE_DATA_128 &&
            value_len >= MCL_BLE_SERVICE_UUID_SIZE) {
            if (memcmp(value, kServiceUuidLe, MCL_BLE_SERVICE_UUID_SIZE) == 0) {
                *uuid_seen = true;
                if (value_len == MCL_BLE_SERVICE_UUID_SIZE + MCL_RENDEZVOUS_BEACON_SIZE &&
                    memcmp(value + MCL_BLE_SERVICE_UUID_SIZE, want_beacon,
                           MCL_RENDEZVOUS_BEACON_SIZE) == 0) {
                    return true;
                }
            }
        }
        i += 1u + field_len;
    }
    return false;
}

/* What the scanner found, handed to the main loop rather than acted on in the
   BLE task. */
std::atomic<bool> g_scan_hit{false};
uint8_t  g_scan_addr[6] = {0};
uint8_t  g_scan_addr_type = 0;
uint8_t  g_scan_raw[62] = {0};
portMUX_TYPE g_scan_mux = portMUX_INITIALIZER_UNLOCKED;
size_t   g_scan_raw_len = 0;
uint32_t g_wanted_token = 0;   /* the peer's, learned from the air */
uint8_t  g_wanted_beacon[MCL_RENDEZVOUS_BEACON_SIZE] = {0};

class NodeScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override {
        ++g_counters.scan_seen;
        if (g_scan_hit) { return; }
        const uint8_t *payload = device.getPayload();
        const size_t len = device.getPayloadLength();
        if (payload == nullptr || len == 0u) { return; }

        bool uuid_seen = false;
        if (!advertisement_matches(payload, len, g_wanted_beacon, &uuid_seen)) {
            if (uuid_seen) { ++g_counters.scan_uuid_only; }
            return;
        }
        ++g_counters.scan_matches;

        BLEAddress addr = device.getAddress();
        portENTER_CRITICAL(&g_scan_mux);
        if (g_scan_hit.load()) { portEXIT_CRITICAL(&g_scan_mux); return; }
        memcpy(g_scan_addr, addr.getNative(), 6);
        g_scan_addr_type = device.getAddressType();
        g_scan_raw_len = (len > sizeof(g_scan_raw)) ? sizeof(g_scan_raw) : len;
        memcpy(g_scan_raw, payload, g_scan_raw_len);
        g_scan_hit = true;
        portEXIT_CRITICAL(&g_scan_mux);
    }
};
NodeScanCallbacks g_scan_callbacks;

/* Only the worker invokes blocking central operations. Main owns MCL, scan
   restarts and log writes. Release/acquire publication protects the request
   and receipt; a cancelled completion can never ready a later transaction. */
enum ActivationWork { WORK_IDLE, WORK_CONNECT, WORK_DONE };
std::atomic<int> g_activation_work{WORK_IDLE};
std::atomic<bool> g_activation_cancel{false};
std::atomic<int> g_gap_status{-1};
std::atomic<int> g_gap_handle{-1};
TaskHandle_t g_activation_task = nullptr;
ble_gap_event_listener g_gap_listener;
bool g_gap_listener_registered = false;
bool g_candidate_active = false;
bool g_candidate_close_pending = false;
uint32_t g_connect_started_ms = 0;
uint32_t g_connect_heartbeat_ms = 0;
uint32_t g_connect_loop_ticks = 0;
uint32_t g_client_creations = 0;
uint32_t g_retry_test_attempts = 0;
uint32_t g_retry_test_completions = 0;
uint32_t g_retry_test_next_ms = 0;
bool g_retry_test_ok = true;
uint32_t g_retry_test_first_heap = 0;
uint32_t g_retry_test_first_largest = 0;
constexpr uint32_t kActivationOperationMs = 30000;
struct HeapReceipt { uint32_t free_bytes, largest, dma_free, dma_largest; };
bool is_central_diagnostic(uint8_t scenario) { return scenario >= 7u && scenario <= 10u; }
struct ActivationReceipt {
    HeapReceipt before_create, after_create, before_connect, after_connect, cleanup;
    bool connected, ready, retryable, cleanup_ok;
    uint32_t elapsed_ms, stack_free;
    int gap_status;
    uint16_t connection_handle, rx_handle, negotiated_mtu;
    uint8_t diagnostic_fragments;
    int diagnostic_rc[3];
};
ActivationReceipt g_activation_receipt{};
uint8_t g_connect_addr[6];
uint8_t g_connect_addr_type = 0;
int g_cancel_status = -1;
int g_terminate_status = -1;
uint32_t g_cancel_last_ms = 0;

/* getNative() is already little-endian on NimBLE. Its uint8_t[] constructor
   reverses bytes; its ble_addr_t constructor preserves native order/type. */
BLEAddress address_from_native(const uint8_t *bytes, uint8_t type) {
    ble_addr_t native{};
    memcpy(native.val, bytes, sizeof(native.val));
    native.type = type;
    return BLEAddress(native);
}

int activation_connection_handle() {
    ble_addr_t native{};
    memcpy(native.val, g_connect_addr, sizeof(native.val));
    native.type = g_connect_addr_type;
    ble_gap_conn_desc descriptor{};
    return ble_gap_conn_find_by_addr(&native, &descriptor) == 0
        ? static_cast<int>(descriptor.conn_handle) : -1;
}

HeapReceipt activation_heap() {
    return {static_cast<uint32_t>(ESP.getFreeHeap()),
            static_cast<uint32_t>(ESP.getMaxAllocHeap()),
            static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
            static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA))};
}

int activation_gap_event(ble_gap_event *event, void *) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        g_gap_status.store(event->connect.status);
        if (event->connect.status == 0) { g_gap_handle.store(event->connect.conn_handle); }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        g_gap_handle.store(-1);
    }
    return 0;
}

/* NimBLE's connection attempt and discovery must be cancelled through its
   controller API. Do not delete a task/client out from under a GAP callback. */
void activation_cancel_io() {
    g_activation_cancel.store(true);
    if (!g_ble_up) { return; }
    if (millis() - g_cancel_last_ms < 100u) { return; }
    g_cancel_last_ms = millis();
    g_cancel_status = ble_gap_conn_cancel();
    const int handle = activation_connection_handle();
    if (handle >= 0) {
        g_terminate_status = ble_gap_terminate(static_cast<uint16_t>(handle), BLE_ERR_REM_USER_CONN_TERM);
    }
}

void activation_worker(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (g_activation_work.load(std::memory_order_acquire) != WORK_CONNECT) { continue; }
        ActivationReceipt receipt{};
        const uint32_t started = millis();
        receipt.before_create = activation_heap();
        if (g_client == nullptr && !g_activation_cancel.load()) {
            g_client = BLEDevice::createClient();
            if (g_client != nullptr) { ++g_client_creations; }
        }
        receipt.after_create = activation_heap();
        receipt.before_connect = activation_heap();
        receipt.retryable = (g_client != nullptr);
        if (g_client != nullptr && !g_activation_cancel.load()) {
            BLEAddress address = address_from_native(g_connect_addr, g_connect_addr_type);
            receipt.connected = g_client->connect(address, g_connect_addr_type);
        }
        receipt.after_connect = activation_heap();
        receipt.gap_status = g_gap_status.load();
        if (receipt.connected && !g_activation_cancel.load()) {
            /* A failed discovery ends this transaction: do not reuse a
               partially cached service tree for a different peer. */
            receipt.retryable = false;
            BLERemoteService *service = g_client->getService(BLEUUID(MCL_SERVICE_UUID));
            if (service != nullptr && !g_activation_cancel.load()) {
                BLERemoteCharacteristic *rx = service->getCharacteristic(BLEUUID(MCL_RX_CHAR_UUID));
                BLERemoteCharacteristic *tx = service->getCharacteristic(BLEUUID(MCL_TX_CHAR_UUID));
                if (rx != nullptr && tx != nullptr && rx->canWriteNoResponse() &&
                    tx->canNotify() && !g_activation_cancel.load()) {
                    receipt.ready = tx->subscribe(true, client_notify_cb, true);
                    if (receipt.ready) {
                        g_remote_rx = rx;
                        receipt.connection_handle = g_client->getConnId();
                        receipt.rx_handle = rx->getHandle();
                        receipt.negotiated_mtu = ble_att_mtu(receipt.connection_handle);
                        /* Configured controls isolate task context and wrapper/native
                           dispatch. Scenario 9 is NOT BLE-GATT-1 qualification: it
                           deliberately requests ATT responses for diagnosis. */
                        if (g_config.scenario >= 8u && g_config.scenario <= 10u) {
                            uint8_t frame[40], pdu[MCL_BLE_ATT_DEFAULT_MTU];
                            for (size_t i = 0; i < sizeof(frame); ++i) { frame[i] = static_cast<uint8_t>(i + 1u); }
                            for (size_t i = 0; i < 3u && !g_activation_cancel.load(); ++i) {
                                size_t written = 0;
                                if (mcl_ble_fragment(frame, sizeof(frame), MCL_BLE_ATT_DEFAULT_MTU,
                                                     i, pdu, sizeof(pdu), &written) != MCL_BLE_OK) { break; }
                                const int rc = (g_config.scenario == 10u)
                                    ? ble_gattc_write_no_rsp_flat(receipt.connection_handle, receipt.rx_handle, pdu, written)
                                    : (rx->writeValue(pdu, written, g_config.scenario == 9u) ? 0 : -1);
                                receipt.diagnostic_rc[i] = rc;
                                ++receipt.diagnostic_fragments;
                                if (rc != 0) { break; }
                                delay(8);
                            }
                        }
                    }
                }
            }
        }
        if (g_activation_cancel.load()) { receipt.ready = false; }
        receipt.cleanup_ok = true;
        if (!receipt.ready) {
            g_remote_rx = nullptr;
            const int handle = activation_connection_handle();
            if (handle >= 0) {
                (void)ble_gap_terminate(static_cast<uint16_t>(handle), BLE_ERR_REM_USER_CONN_TERM);
            }
            const uint32_t cleanup_started = millis();
            while (activation_connection_handle() >= 0 && millis() - cleanup_started < 2000u) {
                delay(10);
            }
            receipt.cleanup_ok = activation_connection_handle() < 0;
        }
        receipt.cleanup = activation_heap();
        receipt.elapsed_ms = millis() - started;
        receipt.stack_free = uxTaskGetStackHighWaterMark(nullptr);
        g_activation_receipt = receipt;
        g_activation_work.store(WORK_DONE, std::memory_order_release);
    }
}

/* ------------------------------------------------------------ BLE bring-up */

bool ble_stack_up() {
    if (g_ble_up) { return true; }
    if (g_wifi_up) {
        /* Measured, not assumed: BLEDevice::init() is refused with the SoftAP
           up in this memory layout. See spike-ble-memory/README.md. */
        log_line("BLE refused: wifi still up (measured incompatible)");
        return false;
    }
    if (!BLEDevice::init("MCL-AUTO-NODE")) {
        log_line("BLE init refused free_heap=%lu",
                 static_cast<unsigned long>(ESP.getFreeHeap()));
        return false;
    }
    g_ble_up = true;
    g_gap_handle.store(-1);
    g_gap_status.store(-1);
    g_gap_listener_registered =
        ble_gap_event_listener_register(&g_gap_listener, activation_gap_event, nullptr) == 0;
    if (!g_gap_listener_registered) {
        log_line("BLE GAP instrumentation registration refused");
        BLEDevice::deinit(false);
        g_ble_up = false;
        return false;
    }
    mcl_ble_reassembler_reset(&g_reasm);
    log_line("BLE up free_heap=%lu largest=%lu",
             static_cast<unsigned long>(ESP.getFreeHeap()),
             static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    return true;
}

/*
 * ORDERLY, AND THE ORDER IS THE WHOLE POINT.
 *
 * Tearing the stack down under a live connection panics this part. The
 * post-mortem caught it: reset_reason 4 at 48 015 ms of a 45 000 ms run that
 * began at 3 015 ms -- exactly scenario_end -- in phase ACTIVATE, every time
 * the phone had connected as central. The panic takes the RAM log ring with
 * it, so without the RTC copy the run simply looked like a board that came
 * back idle.
 *
 * The central path always disconnected first. The peripheral path did not,
 * because when this node is the offerer it never calls connect() and there was
 * no obvious place to hang the hang-up. So both are handled here: stop
 * advertising so nothing new arrives, drop whatever is connected, and let the
 * host task run the disconnect to completion before the ground disappears.
 */
void ble_stack_down() {
    if (!g_ble_up) { return; }
    if (g_activation_work.load(std::memory_order_acquire) == WORK_CONNECT) {
        g_candidate_active = false;
        g_candidate_close_pending = true;
        activation_cancel_io();
        return;  /* the worker must acknowledge before its client is freed */
    }

    if (g_ble_role == BLE_ROLE_PERIPHERAL) {
        BLEDevice::stopAdvertising();
    }
    if (g_client != nullptr && g_client->isConnected()) {
        g_client->disconnect();
        delay(100);
    }
    if (g_server != nullptr && g_server->getConnectedCount() > 0u) {
        g_server->disconnect(g_server->getConnId());
    }
    /*
     * Wait for the disconnect rather than assuming a fixed delay is enough.
     * The bound is short and the loop exits as soon as the peer is gone; the
     * delay that used to be here was 100 ms and covered a client disconnect,
     * not a server one.
     */
    for (int waited = 0; waited < 60; ++waited) {
        const bool server_busy =
            (g_server != nullptr && g_server->getConnectedCount() > 0u);
        const bool client_busy =
            (g_client != nullptr && g_client->isConnected());
        if (!server_busy && !client_busy) { break; }
        delay(25);
    }
    g_ble_connected = false;

    if (g_gap_listener_registered) {
        (void)ble_gap_event_listener_unregister(&g_gap_listener);
        g_gap_listener_registered = false;
    }
    /* Free stack objects, but keep controller memory reclaimable by init.
       release_memory=true permanently prevents BLE reinitialization. */
    BLEDevice::deinit(false);
    g_ble_up = false;
    g_ble_connected = false;
    g_ble_subscribed = false;
    g_server = nullptr;
    g_tx_char = nullptr;
    g_client = nullptr;
    g_remote_rx = nullptr;
    g_ble_frame_ready = false;
    g_ble_role = BLE_ROLE_NONE;
    g_scan_hit.store(false);
    g_gap_handle.store(-1);
    g_client_creations = 0;
    delay(200);
    log_line("BLE down free_heap=%lu",
             static_cast<unsigned long>(ESP.getFreeHeap()));
}

/*
 * The offerer. Builds the GATT service and advertises the exact structure
 * BLE-ACTIVATE-1 §3 defines: ONE Service Data - 128-bit AD, and nothing else.
 * No device name, no TX power, no duplicated UUID list -- an advertisement
 * with extra structures is a different advertisement, and a legacy PDU has
 * 31 bytes to spend.
 */
bool ble_become_peripheral(uint32_t own_token) {
    if (!ble_stack_up()) { return false; }

    g_server = BLEDevice::createServer();
    g_server->setCallbacks(&g_server_callbacks);

    BLEService *service = g_server->createService(MCL_SERVICE_UUID);
    BLECharacteristic *rx_char = service->createCharacteristic(
        MCL_RX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx_char->setCallbacks(&g_rx_callbacks);

    g_tx_char = service->createCharacteristic(MCL_TX_CHAR_UUID,
                                              BLECharacteristic::PROPERTY_NOTIFY);
    g_tx_char->setCallbacks(&g_tx_callbacks);
    /* ESP32 core 3.x uses NimBLE, which creates the 0x2902 client
       configuration descriptor automatically for NOTIFY characteristics.
       Manually allocating a second one is deprecated and can leave two CCCDs
       describing one characteristic. */
    service->start();

    uint8_t beacon[MCL_RENDEZVOUS_BEACON_SIZE];
    beacon_from_token(own_token, beacon);

    String service_data;
    for (size_t i = 0; i < sizeof(beacon); ++i) {
        service_data += static_cast<char>(beacon[i]);
    }

    BLEAdvertisementData adv_data;
    adv_data.setServiceData(BLEUUID(MCL_SERVICE_UUID), service_data);

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->setAdvertisementData(adv_data);
    advertising->setScanResponse(false);
    BLEDevice::startAdvertising();

    g_ble_role = BLE_ROLE_PERIPHERAL;
    log_line("BLE advertising token=%08lX beacon=%02X%02X%02X%02X%02X%02X%02X%02X",
             static_cast<unsigned long>(own_token),
             beacon[0], beacon[1], beacon[2], beacon[3],
             beacon[4], beacon[5], beacon[6], beacon[7]);
    return true;
}

/*
 * How long one scan slice lasts. Long enough that a peer advertising at a
 * typical 100 ms interval is seen many times over, short enough that the
 * library's per-address retention (see ble_become_central) is emptied often.
 */
constexpr uint32_t kScanSliceSeconds = 20;
uint32_t g_scan_slice_ms = 0;

/* The acceptor. Scans for the token it heard, and connects to nothing else. */
bool ble_become_central(uint32_t peer_token) {
    if (!ble_stack_up()) { return false; }

    g_wanted_token = peer_token;
    beacon_from_token(peer_token, g_wanted_beacon);
    g_scan_hit = false;

    BLEScan *scan = BLEDevice::getScan();
    /*
     * shouldParse=false, AND THE SCAN IS SLICED. Both are here because this
     * board rebooted mid-run and the reason was not the obvious one.
     *
     * This core builds BLE on NimBLE, not Bluedroid, and the two halves of
     * BLEScan.cpp behave differently. On the NimBLE path `wantDuplicates` is
     * never read at all -- the flag is live only in the Bluedroid half -- so
     * setting it is not a fix, it is a comment that compiles. What that path
     * does instead is insert EVERY newly seen address into
     * m_scanResults.m_vectorAdvertisedDevices unconditionally, and only drop
     * it again when m_maxResults == 0. m_maxResults is initialised to 0xFF and
     * this version exposes no setter, so retention is unbounded by default:
     * one live allocation per distinct BLE address, in a room where phones
     * rotate their resolvable private address every fifteen minutes. A BLE
     * phase here runs with roughly 15 KB of heap in total. It does not take
     * long.
     *
     * The library does expose the cure, in start(): with is_continue = false
     * it calls clearResults() itself, under its own m_ignoreResults guard, so
     * the map is emptied at a moment when the host task is not inserting into
     * it. Calling clearResults() directly from this task while a scan is
     * running would race the NimBLE host task on a std::map instead. So the
     * scan runs in bounded slices and ble_scan_pump() starts the next one.
     *
     * shouldParse=false then skips parseAdvertisement, whose String and vector
     * allocations are the bulk of the per-advertisement churn. The callback
     * reads getPayload()/getPayloadLength(), which setPayload() fills in on
     * the unparsed path, plus the address and address type, which are set
     * explicitly either way. Nothing here needs a parsed view.
     */
    scan->setAdvertisedDeviceCallbacks(&g_scan_callbacks, false, false);
    /* Passive: everything needed is in the advertisement itself, and a scan
       request would put this machine on the air for no information. */
    scan->setActiveScan(false);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->start(kScanSliceSeconds, nullptr, false);   /* one slice */
    g_scan_slice_ms = millis();

    g_ble_role = BLE_ROLE_CENTRAL;
    log_line("BLE scanning for token=%08lX", static_cast<unsigned long>(peer_token));
    return true;
}

/*
 * Start the next scan slice once the previous one has expired. Called from
 * every path that waits on the scanner; doing it here rather than in the
 * callback keeps the restart on the application task, where the library's
 * clearResults() is safe.
 */
void ble_scan_pump() {
    if (g_ble_role != BLE_ROLE_CENTRAL || g_ble_connected || !g_ble_up ||
        g_candidate_close_pending ||
        g_activation_work.load(std::memory_order_acquire) != WORK_IDLE) { return; }
    BLEScan *scan = BLEDevice::getScan();
    if (scan->isScanning()) { return; }
    scan->start(kScanSliceSeconds, nullptr, false);
    g_scan_slice_ms = millis();
    ++g_counters.scan_slices;
}

bool ble_connect_to_hit() {
    if (g_activation_task == nullptr ||
        g_activation_work.load(std::memory_order_acquire) != WORK_IDLE) { return false; }
    BLEDevice::getScan()->stop();
    memcpy(g_connect_addr, g_scan_addr, sizeof(g_connect_addr));
    g_connect_addr_type = g_scan_addr_type;
    BLEAddress addr = address_from_native(g_connect_addr, g_connect_addr_type);
    if (memcmp(addr.getNative(), g_scan_addr, sizeof(g_scan_addr)) != 0 ||
        addr.getType() != g_scan_addr_type) {
        log_line("BLE native address round trip refused");
        return false;
    }
    log_line("BLE found %s type=%u adv_len=%u", addr.toString().c_str(),
             static_cast<unsigned>(g_connect_addr_type), static_cast<unsigned>(g_scan_raw_len));
    g_scan_hit.store(false);
    g_gap_status.store(-1);
    g_cancel_status = g_terminate_status = -1;
    g_cancel_last_ms = 0;
    g_activation_cancel.store(false);
    g_connect_started_ms = millis();
    g_connect_heartbeat_ms = g_connect_started_ms;
    g_connect_loop_ticks = 0;
    g_activation_work.store(WORK_CONNECT, std::memory_order_release);
    xTaskNotifyGive(g_activation_task);
    return true;  /* queued, NOT connected */
}

/* ---------------------------------------------------------- BLE egress */

/*
 * Fragmented at the SMALLEST MTU BLE permits, not at whatever this connection
 * negotiated. A peer that negotiates a large MTU would carry every frame in
 * one PDU and leave the fragmentation path completely untested; the minimum is
 * the case the scheme has to survive.
 */
int32_t ble_send_frame(const uint8_t *frame, size_t frame_size) {
    const uint16_t mtu = MCL_BLE_ATT_DEFAULT_MTU;
    uint8_t pdu[MCL_BLE_ATT_DEFAULT_MTU];
    size_t written = 0;

    if (!g_ble_connected) { return -1; }   /* definitely not sent: no link */

    const size_t count = mcl_ble_fragment_count(frame_size, mtu);
    if (count == 0u) { return -1; }

    for (size_t i = 0; i < count; ++i) {
        if (mcl_ble_fragment(frame, frame_size, mtu, i, pdu, sizeof(pdu),
                             &written) != MCL_BLE_OK) {
            /*
             * Partway through. Some fragments are already out, so the peer may
             * hold part of a frame it can never complete. Reported as
             * UNCERTAIN rather than "not sent", because bytes did leave.
             */
            return (i == 0u) ? -1 : 1;
        }
        if (g_ble_role == BLE_ROLE_PERIPHERAL) {
            if (g_tx_char == nullptr || !g_ble_subscribed) { return (i == 0u) ? -1 : 1; }
            g_tx_char->setValue(pdu, written);
            g_tx_char->notify();
        } else {
            if (g_remote_rx == nullptr) { return (i == 0u) ? -1 : 1; }
            const bool queued = g_remote_rx->writeValue(pdu, written, false);
            if (is_central_diagnostic(g_config.scenario)) {
                log_line("BLE TX main fragment=%u bytes=%u conn=%u handle=%u queued=%u",
                         static_cast<unsigned>(i), static_cast<unsigned>(written),
                         g_client->getConnId(), g_remote_rx->getHandle(), queued);
            }
            if (!queued) {
                return (i == 0u) ? -1 : 1;
            }
        }
        delay(8);   /* let the stack drain; a burst is dropped, not queued */
    }
    ++g_counters.ble_frames_tx;
    return 0;
}

/* ============================================================ rendezvous
 *
 * THE NODE DRIVES `mcl/machine.h`, NOT THE COORDINATOR UNDERNEATH IT.
 *
 * An earlier version of this file wired `mcl_node_*` and `mcl_rdv_*` together
 * by hand: two configurations to keep consistent, seven platform callbacks, a
 * poll loop, byte routing by transport, and the step every builder would have
 * written differently -- noticing BEARER_AGREED and opening the candidate
 * bearer before the coordinator emits anything on it.
 *
 * All of that is the facade's now. What is left below is what only this board
 * knows: how to put bytes on a speaker and a GATT link, how to open a BLE
 * candidate, and whether to admit a stranger. That is the correct division,
 * and this file is the check that the division is possible -- if a real
 * integration still needed `mcl_rdv_*`, the facade would not be finished.
 */

mcl_machine_t g_machine;
bool     g_machine_running = false;
uint32_t g_own_token = 0;        /* what the facade minted for this transaction */
uint32_t g_activation_deadline_ms = 0;

/*
 * The transmit callback, for everything MCL sends on any medium.
 *
 * Returns 0 for accepted, negative for DEFINITELY not transmitted, positive
 * for an outcome this board cannot vouch for. The three-way answer is what
 * lets MCL decide whether sending a COMMIT was irrevocable.
 */
int32_t plat_send(void *user, uint8_t transport_id,
                  const uint8_t *data, size_t size) {
    (void)user;
    switch (transport_id) {
        case MCL_CONTACT_TRANSPORT_AP:
            /*
             * The speaker is a definite medium in one direction only: we know
             * whether the amplifier accepted the samples, never whether anyone
             * heard them. Accepted means emitted, not delivered.
             */
            return emit_payload(data, size) ? 0 : -1;
        case MCL_CONTACT_TRANSPORT_BLE:
            return ble_send_frame(data, size);
        default:
            return -1;   /* a transport this board does not have */
    }
}

uint32_t plat_clock_ms(void *) { return millis(); }

int plat_random_bytes(void *, uint8_t *out, size_t size) {
    if (out == nullptr) { return -1; }
    esp_fill_random(out, size);
    return 0;
}

/*
 * Is the shared medium busy?
 *
 * `pending` means the listener has acquired a preamble and is waiting for the
 * body: a transmission is in progress. Answering only with our own transmit
 * state would make the backoff a delay rather than a deferral, which
 * AP-BOOTSTRAP-1 section 7 says is not enough on a medium where one frame is
 * longer than the contention window.
 */
int plat_medium_busy(void *) {
    return (g_listener.pending != 0u || g_transmitting) ? 1 : 0;
}

int plat_self_transmitting(void *) { return g_transmitting ? 1 : 0; }

/*
 * OPEN THE AGREED BEARER. The one thing MCL cannot do for a machine.
 *
 * The role is not chosen here and could not be: `peer_endpoint_token` is
 * non-zero only for the peer that received a TRANSPORT_OFFER, because that is
 * the only object carrying one. So the peer that can be found advertises, and
 * the peer holding the token scans. BLE-ACTIVATE-1 section 2, arriving as data.
 */
mcl_machine_candidate_t plat_candidate_open(void *, uint8_t transport_id,
                                            uint8_t profile_id,
                                            uint32_t peer_endpoint_token,
                                            uint32_t local_endpoint_token) {
    (void)profile_id;
    if (g_candidate_close_pending ||
        g_activation_work.load(std::memory_order_acquire) != WORK_IDLE) {
        return MCL_MACHINE_CANDIDATE_REFUSED;
    }
    g_candidate_active = true;
    if (transport_id != MCL_CONTACT_TRANSPORT_BLE) {
        g_candidate_active = false;
        log_line("candidate on transport %u not implemented by this node",
                 static_cast<unsigned>(transport_id));
        return MCL_MACHINE_CANDIDATE_REFUSED;
    }

    set_phase(PHASE_ACTIVATE);
    g_activation_deadline_ms =
        millis() + (MCL_RDV_MAX_HANDOFF_RETRIES + 1u) * MCL_RDV_AP1_RESPONSE_TIMEOUT_MS;

    if (peer_endpoint_token != 0u) {
        tag_value("peer_endpoint_token", peer_endpoint_token, PROV_FROM_AIR);
        log_line("acceptor: peer token %08lX learned from the air",
                 static_cast<unsigned long>(peer_endpoint_token));
        if (!ble_become_central(peer_endpoint_token)) {
            g_candidate_active = false;
            g_candidate_close_pending = g_ble_up;
            return MCL_MACHINE_CANDIDATE_REFUSED;
        }
        /* Scanning, connecting: not usable yet. */
        return MCL_MACHINE_CANDIDATE_PENDING;
    }

    if (local_endpoint_token == 0u) {
        g_candidate_active = false;
        /* BLE-ACTIVATE-1 section 3: a zero token names nothing, because this
           profile's discovery IS the token. Refuse rather than advertise
           something no scanner can select. */
        log_line("activation refused: no local token for a BLE candidate");
        return MCL_MACHINE_CANDIDATE_REFUSED;
    }
    g_own_token = local_endpoint_token;
    tag_value("own_endpoint_token", local_endpoint_token, PROV_LOCAL);
    log_line("offerer: advertising own token %08lX",
             static_cast<unsigned long>(local_endpoint_token));
    if (!ble_become_peripheral(local_endpoint_token)) {
        g_candidate_active = false;
        g_candidate_close_pending = g_ble_up;
        return MCL_MACHINE_CANDIDATE_REFUSED;
    }
    /* Advertisement is discoverability, not usable bidirectional carriage.
       The main loop publishes ready only after connection and CCCD setup. */
    return MCL_MACHINE_CANDIDATE_PENDING;
}

void plat_candidate_close(void *, uint8_t transport_id) {
    log_line("candidate closed on transport %u",
             static_cast<unsigned>(transport_id));
    g_candidate_active = false;
    g_candidate_close_pending = true;
    g_activation_deadline_ms = 0;
    if (g_ble_up && g_ble_role == BLE_ROLE_CENTRAL) { BLEDevice::getScan()->stop(); }
    activation_cancel_io();
}

/*
 * Local admission policy, and nothing else. Reception is not identity, is not
 * authority and is not obligation: a node that hears a stranger and declines
 * has behaved correctly, and MCL says so.
 */
int plat_policy_admit(void *, uint32_t peer_ref, uint8_t transport_id) {
    set_phase(PHASE_POLICY);
    const bool admit = (g_config.admit_policy != 0u);
    log_line("policy: peer_ref=%08lX transport=%u -> %s",
             static_cast<unsigned long>(peer_ref),
             static_cast<unsigned>(transport_id),
             admit ? "admit" : "refuse");
    return admit ? 1 : 0;
}

void machine_start() {
    mcl_machine_config_t cfg;
    mcl_platform_t plat;

    /*
     * ONE NAMED DEPLOYMENT. A builder should not have to choose among
     * equivalent combinations before they have seen MCL work once, and two
     * machines that chose differently would not meet.
     */
    if (mcl_machine_config_deployment(&cfg, MCL_DEPLOYMENT_REFERENCE_1,
                                      g_source_ref,
                                      MCL_CONTACT_ROLE_INITIATOR)
        != MCL_MACHINE_OK) {
        log_line("deployment configuration refused");
        return;
    }

    /*
     * This board offers exactly the bearer the run is configured for. The
     * reference deployment mandates BLE and offers IP after it; a run that
     * quiesced Wi-Fi cannot carry IP, and offering a bearer this node has just
     * switched off would be advertising a lie.
     */
    cfg.bearer_count = 1u;
    cfg.bearer_transport_id[0] = g_config.candidate_transport;
    cfg.bearer_profile_id[0] = 1u;

    memset(&plat, 0, sizeof(plat));
    plat.clock_ms = plat_clock_ms;
    plat.random_bytes = plat_random_bytes;
    plat.transport_send = plat_send;
    plat.medium_busy = plat_medium_busy;
    plat.self_transmitting = plat_self_transmitting;
    plat.candidate_open = plat_candidate_open;
    plat.candidate_close = plat_candidate_close;
    plat.policy_admit = plat_policy_admit;
    plat.user = nullptr;

    if (mcl_machine_init(&g_machine, &cfg, &plat) != MCL_MACHINE_OK) {
        log_line("machine init refused");
        return;
    }
    if (mcl_machine_start(&g_machine) != MCL_MACHINE_OK) {
        log_line("machine start refused");
        return;
    }
    g_machine_running = true;
    log_line("machine started deployment=%s candidate_transport=%u",
             (cfg.deployment_profile_id != nullptr) ? cfg.deployment_profile_id : "?",
             static_cast<unsigned>(g_config.candidate_transport));
}

void log_heap_receipt(const char *stage, const HeapReceipt &heap) {
    log_line("BLE %s heap=%lu largest=%lu dma=%lu/%lu", stage,
             static_cast<unsigned long>(heap.free_bytes),
             static_cast<unsigned long>(heap.largest),
             static_cast<unsigned long>(heap.dma_free),
             static_cast<unsigned long>(heap.dma_largest));
}

/* Called even after MCL closes a candidate or the run ends. No worker writes
   the MCL state or log ring, and teardown never frees a live worker's client. */
void activation_service() {
    const int work = g_activation_work.load(std::memory_order_acquire);
    if (work == WORK_CONNECT) {
        ++g_connect_loop_ticks;
        const uint32_t operation_ms = (g_config.scenario == 6u) ? 5000u : kActivationOperationMs;
        if (!g_candidate_active || millis() - g_connect_started_ms >= operation_ms) {
            activation_cancel_io();
        }
        if (millis() - g_connect_heartbeat_ms >= 5000u) {
            g_connect_heartbeat_ms = millis();
            log_line("BLE pending ticks=%lu queued=%lu dropped=%lu",
                     static_cast<unsigned long>(g_connect_loop_ticks),
                     static_cast<unsigned long>(capture_queued()),
                     static_cast<unsigned long>(g_capture_dropped));
        }
        return;
    }
    if (work == WORK_DONE) {
        const ActivationReceipt receipt = g_activation_receipt;
        log_heap_receipt("before_create", receipt.before_create);
        log_heap_receipt("after_create", receipt.after_create);
        log_heap_receipt("before_connect", receipt.before_connect);
        log_heap_receipt("after_connect", receipt.after_connect);
        log_heap_receipt("cleanup", receipt.cleanup);
        log_line("BLE result connect=%u ready=%u gap=%d ms=%lu", receipt.connected,
                 receipt.ready, receipt.gap_status, static_cast<unsigned long>(receipt.elapsed_ms));
        log_line("BLE resource clients=%lu cleanup=%u stack_free=%lu",
                 static_cast<unsigned long>(g_client_creations), receipt.cleanup_ok,
                 static_cast<unsigned long>(receipt.stack_free));
        log_line("BLE cancel_rc=%d terminate_rc=%d gap=-1 means unobserved",
                 g_cancel_status, g_terminate_status);
        if (is_central_diagnostic(g_config.scenario)) {
            log_line("BLE TX endpoint conn=%u rx_handle=%u mtu=%u scenario=%u",
                     receipt.connection_handle, receipt.rx_handle,
                     receipt.negotiated_mtu, g_config.scenario);
            if (g_config.scenario >= 8u) {
                bool sent = receipt.diagnostic_fragments == 3u;
                for (uint8_t i = 0; i < receipt.diagnostic_fragments; ++i) {
                    log_line("BLE TX worker fragment=%u rc=%d", i, receipt.diagnostic_rc[i]);
                    sent = sent && receipt.diagnostic_rc[i] == 0;
                }
                if (sent) { ++g_counters.ble_frames_tx; }
                else { snprintf(g_run_failure, sizeof(g_run_failure), "worker diagnostic TX refused"); }
            }
        }
        g_activation_work.store(WORK_IDLE, std::memory_order_release);
        if (g_config.scenario == 6u && g_run_state == RUN_RUNNING) {
            ++g_retry_test_completions;
            g_retry_test_ok = g_retry_test_ok && !receipt.connected && !receipt.ready &&
                receipt.cleanup_ok && g_client_creations == 1u && g_connect_loop_ticks > 10u;
            g_retry_test_next_ms = millis() + 1000u;
        } else if (is_central_diagnostic(g_config.scenario) && g_run_state == RUN_RUNNING) {
            g_ble_connected = g_candidate_active && !g_activation_cancel.load() && receipt.ready;
            if (!g_ble_connected) {
                g_candidate_active = false;
                g_candidate_close_pending = true;
                snprintf(g_run_failure, sizeof(g_run_failure), "central diagnostic activation refused");
            }
        } else if (g_candidate_active && !g_activation_cancel.load() && receipt.ready &&
            g_machine.awaiting_candidate != 0u) {
            g_ble_connected = true;
            if (mcl_machine_candidate_ready(&g_machine) != MCL_MACHINE_OK) {
                plat_candidate_close(nullptr, MCL_CONTACT_TRANSPORT_BLE);
            }
        } else if (g_candidate_active &&
                   (!receipt.retryable || !receipt.cleanup_ok || g_activation_cancel.load())) {
            (void)mcl_machine_candidate_refused(&g_machine);
        } else if (!g_candidate_active) {
            g_candidate_close_pending = true;
        }
        /* A clean failed connect reuses this client; scanning resumes only
           through ble_scan_pump's bounded, clearing slice. */
    }
    if (g_candidate_close_pending && g_activation_work.load() == WORK_IDLE) {
        ble_stack_down();
        if (!g_ble_up) { g_candidate_close_pending = false; }
    }
    if (g_candidate_active && g_ble_role == BLE_ROLE_PERIPHERAL &&
        g_ble_connected && g_ble_subscribed && g_machine.awaiting_candidate != 0u) {
        log_line("BLE peripheral ready after notification subscription");
        if (mcl_machine_candidate_ready(&g_machine) != MCL_MACHINE_OK) {
            plat_candidate_close(nullptr, MCL_CONTACT_TRANSPORT_BLE);
        }
    }
}

void machine_pump() {
    /* A notification can arrive before the connect worker publishes WORK_DONE.
       Keep the bounded mailbox occupied until activation_service has made the
       candidate ready. Otherwise receive can invoke policy while send still
       reports no link, or consume awaiting_candidate before readiness. */
    static bool deferred_frame_logged = false;
    if (g_ble_frame_ready && g_machine.awaiting_candidate != 0u) {
        if (!deferred_frame_logged) {
            log_line("BLE RX deferred until candidate ready ms=%lu bytes=%u worker=%d",
                     static_cast<unsigned long>(millis()),
                     static_cast<unsigned>(g_ble_frame_size),
                     g_activation_work.load(std::memory_order_acquire));
            deferred_frame_logged = true;
        }
    }
    if (g_ble_frame_ready && g_ble_connected &&
        g_machine.awaiting_candidate == 0u &&
        g_activation_work.load(std::memory_order_acquire) == WORK_IDLE) {
        (void)mcl_machine_receive(&g_machine, MCL_CONTACT_TRANSPORT_BLE,
                                  g_ble_frame, g_ble_frame_size);
        g_ble_frame_ready = false;
    }
    if (!g_ble_frame_ready) { deferred_frame_logged = false; }

    ble_scan_pump();
    if (g_candidate_active && g_scan_hit.load() &&
        g_ble_role == BLE_ROLE_CENTRAL && !g_ble_connected &&
        g_activation_work.load(std::memory_order_acquire) == WORK_IDLE) {
        tag_value("peer_ble_address",
                  (static_cast<uint32_t>(g_scan_addr[2]) << 24) |
                  (static_cast<uint32_t>(g_scan_addr[3]) << 16) |
                  (static_cast<uint32_t>(g_scan_addr[4]) << 8) |
                  static_cast<uint32_t>(g_scan_addr[5]), PROV_FROM_BEARER);
        (void)ble_connect_to_hit();
    }

    mcl_machine_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (mcl_machine_poll(&g_machine, &ev) != MCL_MACHINE_OK) { return; }

    switch (ev.kind) {
        case MCL_MACHINE_EVENT_NONE:
            break;
        case MCL_MACHINE_EVENT_PEER_DETECTED:
            tag_value("peer_source_ref", ev.peer_ref, PROV_FROM_AIR);
            log_line("PEER_DETECTED peer_ref=%08lX",
                     static_cast<unsigned long>(ev.peer_ref));
            break;
        case MCL_MACHINE_EVENT_POLICY_REQUIRED:
            /* Only raised when no policy callback is installed, and one is.
               Reaching here means the platform table was built wrong. */
            log_line("POLICY_REQUIRED with a policy callback installed");
            (void)mcl_machine_refuse(&g_machine);
            break;
        case MCL_MACHINE_EVENT_CONTACT_ESTABLISHED:
            set_phase(PHASE_MIGRATED);
            /* The session was allocated by whichever peer accepted the offer.
               We accepted if we were given a token to reach; otherwise it
               reached us from the air. */
            tag_value("session_ref", ev.session_ref,
                      (g_own_token != 0u) ? PROV_FROM_AIR : PROV_LOCAL);
            log_line("CONTACT_ESTABLISHED transport=%u profile=%u ses=%08lX",
                     static_cast<unsigned>(ev.transport_id),
                     static_cast<unsigned>(ev.profile_id),
                     static_cast<unsigned long>(ev.session_ref));
            break;
        case MCL_MACHINE_EVENT_CONTACT_LOST:
            log_line("CONTACT_LOST");
            break;
        case MCL_MACHINE_EVENT_NO_COMMON_BEARER:
            log_line("NO_COMMON_BEARER: every mandated bearer was offered and refused");
            break;
        case MCL_MACHINE_EVENT_ERROR:
            log_line("ERROR status=%ld transport=%u",
                     static_cast<long>(ev.status),
                     static_cast<unsigned>(ev.transport_id));
            break;
        default:
            break;
    }
}

/* ============================================================ scenarios */

/*
 * Scenario 0: LISTEN ONLY.
 *
 * The node listens for the run's duration and reports what it heard, keeping
 * QUIET / HEARD / CONTACT distinct. It emits nothing, so it is the scenario to
 * run while somebody else is measured, and it is the one that establishes the
 * room's own noise before anything is concluded from a failure.
 */
/*
 * How much new audio to gather before searching it. See the note on
 * listen_should_poll: this is the length of the contiguous stretch the
 * receiver gets, and it must stay below the window capacity or the beginning
 * of the stretch is overwritten before it is searched.
 */
/*
 * 4 096 samples is 85 ms of audio. The ESP_I2S receive queue holds six 240
 * sample DMA descriptors, or 30 ms. With the sparse 18/9 acquisition pass a
 * cached poll over 4 096 new samples fits that backlog; a 40 000-sample poll
 * took hundreds of milliseconds and made the microphone deaf while the main
 * task searched. The reference is prepared before the timed run so its
 * deterministic construction is never charged to the first DMA interval.
 *
 * A frame does not need to fit in one batch. `mcl_ap_listener_t` retains the
 * trailing preamble and a pending acquisition across polls; the batch is only
 * how often new candidate starts are searched.
 */
constexpr size_t kPollBatchSamples = 4096;   /* 85 ms; below one DMA backlog */
uint64_t g_last_poll_pushed = 0;

/*
 * True when enough new audio has arrived to be worth a search.
 *
 * Polling every 1 024-sample read would pay the full-rate peak refinement four
 * times per batch. Waiting for four reads amortises that fixed part while
 * staying within the measured DMA/backlog budget.
 *
 * `pending` is the exception: the listener has already acquired a preamble and
 * is waiting for the body of that frame to arrive. That poll is pinned to a
 * single start position and does no correlation sweep at all, so it is cheap
 * and must not be delayed -- delaying it is how an acquired frame times out.
 */
bool listen_should_poll() {
    const uint64_t fresh = g_listener.total_pushed - g_last_poll_pushed;
    if (g_listener.pending != 0u) {
        /* Pending work becomes decidable only when more body audio arrives.
           Polling the same buffer in a tight loop produced 115k no-op calls
           in one 22 s run and stole cycles without changing a verdict. */
        return fresh != 0u;
    }
    return fresh >= kPollBatchSamples;
}

void account_listen_result(mcl_ap_listen_result_t r,
                           const mcl_ap_listen_event_t &event,
                           const uint8_t *payload) {
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

void scenario_listen_tick() {
    mcl_ap_listen_event_t event;
    uint8_t payload[kMaxBootstrapPayload];

    if (!listen_should_poll()) { return; }
    g_last_poll_pushed = g_listener.total_pushed;

    const uint32_t poll_started = millis();
    const mcl_ap_listen_result_t r =
        mcl_ap_listen_poll(&g_listener, scratch_ptr(), payload, sizeof(payload), &event);
    const uint32_t poll_ms = millis() - poll_started;
    ++g_poll_calls;
    g_poll_ms_total += poll_ms;
    if (poll_ms > g_poll_ms_max) { g_poll_ms_max = poll_ms; }
    account_listen_result(r, event, payload);
}

/*
 * Scenario 1: ANNOUNCE AND LISTEN.
 *
 * Emit a PRESENCE at a randomised cadence and listen the rest of the time.
 * This is the smallest scenario that puts two of these boards in a room and
 * lets them hear each other with no host deciding who speaks. It is NOT the
 * rendezvous: nothing here contends, offers or migrates.
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

/*
 * Scenario 2: ZERO-PRIOR RENDEZVOUS.
 *
 * The whole path, with nothing configured about the peer: AP detection,
 * PRESENCE, contention, OFFER/ACCEPT, BLE activation, PATH_CHALLENGE /
 * PATH_RESPONSE, policy, COMMIT / CONFIRM, CONTACT_MIGRATED.
 *
 * The coordinator drives it. This function only feeds it audio, hands it
 * whatever arrived on the candidate, and does the two things it cannot do
 * itself: open a bearer and decide whether to admit a stranger.
 */
void scenario_rendezvous_tick() {
    /*
     * KEEP THE BOOTSTRAP MEDIUM LIVE UNTIL THE CANDIDATE IS USABLE.
     *
     * Opening a BLE candidate is not proof that the peer heard our acoustic
     * TRANSPORT_ACCEPT.  If that frame was lost, the offerer retransmits the
     * same TRANSPORT_OFFER and the coordinator answers it idempotently with the
     * stored acceptance.  Stopping AP here as soon as scanning began made that
     * recovery rule unreachable on the real board: the microphone continued
     * filling the capture queue, but none of those samples were decoded.
     *
     * The listener arena is not owned by BLE.  Acoustic transmission already
     * invalidates and rebuilds the overlapped modem scratch in emit_payload(),
     * so it remains valid through ACTIVATE.  Once validation/policy begins the
     * candidate has actually carried traffic and AP can stand down.
     */
    if (g_phase == PHASE_RENDEZVOUS || g_phase == PHASE_ACTIVATE) {
        mcl_ap_listen_event_t event;
        uint8_t payload[kMaxBootstrapPayload];
        mcl_ap_listen_result_t r = MCL_AP_LISTEN_QUIET;
        if (listen_should_poll()) {
            g_last_poll_pushed = g_listener.total_pushed;
            r = mcl_ap_listen_poll(&g_listener, scratch_ptr(), payload,
                                   sizeof(payload), &event);
        }

        if (r == MCL_AP_LISTEN_CONTACT) {
            ++g_counters.frames_recovered;
            log_line("CONTACT %u bytes", static_cast<unsigned>(event.payload_bytes));
            trace_ap("RX", payload, event.payload_bytes);
            (void)mcl_machine_receive(&g_machine, MCL_CONTACT_TRANSPORT_AP,
                                      payload, event.payload_bytes);
        } else if (r == MCL_AP_LISTEN_HEARD) {
            ++g_counters.frames_heard;
        }
    }

    machine_pump();

    if (g_phase == PHASE_ACTIVATE && g_activation_deadline_ms != 0u &&
        static_cast<int32_t>(millis() - g_activation_deadline_ms) >= 0) {
        /*
         * BLE-ACTIVATE-1 §4: the window is derived from the controller's own
         * retransmission schedule, so a peer that has not appeared within it
         * is not going to. Logged rather than retried forever.
         */
        log_line("activation window expired without a connection");
        g_activation_deadline_ms = 0;
        if (g_machine.awaiting_candidate != 0u) {
            (void)mcl_machine_candidate_refused(&g_machine);
        }
    }
}

/* ------------------------------------------- scenarios 4 and 5: BLE-ACTIVATE
 *
 * A DIAGNOSTIC, AND IT SAYS SO IN THE RESULT.
 *
 * Scenario 4 advertises, scenario 5 scans, both for one token that is compiled
 * into this firmware. That token is not learned from the air, so these runs are
 * NOT zero-prior and `/api/result` reports `zero_prior: false` -- the constant
 * is tagged CONFIGURED for exactly that reason, and if it ever reported
 * otherwise the tagging would be broken.
 *
 * What they are for is checking, between two implementations that share no
 * code, that the 26 bytes of `BLE-ACTIVATE-1` §3 are identical on the air:
 * one machine advertises, the other scans, and each can print the raw AD it
 * saw. "Discovery succeeded" is not evidence of an encoding agreement. The
 * bytes are.
 *
 * They are also how the memory verdict is checked against the firmware that
 * actually ships, since both arrive through the armed-restart path and
 * therefore run on a boot where Wi-Fi was never initialised.
 */
constexpr uint32_t kDiagnosticToken = 0xA5C30F17u;
bool g_diag_started = false;
void scenario_end(uint8_t final_state, const char *why);

void scenario_ble_diagnostic_tick(bool advertise) {
    if (!g_diag_started) {
        g_diag_started = true;
        tag_value("diagnostic_token", kDiagnosticToken, PROV_CONFIGURED);
        set_phase(PHASE_ACTIVATE);
        const bool ok = advertise ? ble_become_peripheral(kDiagnosticToken)
                                  : ble_become_central(kDiagnosticToken);
        log_line("ble diagnostic %s %s free_heap=%lu largest=%lu",
                 advertise ? "advertise" : "scan",
                 ok ? "up" : "REFUSED",
                 static_cast<unsigned long>(ESP.getFreeHeap()),
                 static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        if (!ok) {
            scenario_end(RUN_FAILED, "BLE would not come up");
        }
        return;
    }

    /*
     * Anything that arrives on the diagnostic link is reassembled, reported and
     * echoed. The echo is the point: it exercises the notify direction and the
     * fragmenter, so a peer can check that a frame survives the round trip at
     * the minimum MTU rather than only that a write was accepted.
     */
    if (g_ble_frame_ready) {
        log_line("diag frame in %u bytes %02X%02X%02X%02X...",
                 static_cast<unsigned>(g_ble_frame_size),
                 g_ble_frame[0], g_ble_frame[1],
                 (g_ble_frame_size > 2u) ? g_ble_frame[2] : 0u,
                 (g_ble_frame_size > 3u) ? g_ble_frame[3] : 0u);
        const int32_t rc = ble_send_frame(g_ble_frame, g_ble_frame_size);
        log_line("diag echo rc=%ld", static_cast<long>(rc));
        g_ble_frame_ready = false;
    }

    ble_scan_pump();

    if (g_scan_hit && !advertise) {
        g_scan_hit = false;
        log_line("scan matched UUID and beacon, adv_len=%u",
                 static_cast<unsigned>(g_scan_raw_len));
        /* Keep scanning: a second sighting is a fact worth counting, and
           stopping here would report one advertisement as a whole run. The
           slice pump re-arms it; restarting with is_continue = true here is
           what made this scenario reboot the board. */
    }
}

/* Scenario 6 is a fault-injection instrument, never zero-prior evidence.
   It dials a fixed diagnostic address twice, cancels each attempt after five
   seconds, and services the production acoustic listener throughout. No peer
   address can be supplied by the control plane. A real connection fails the
   diagnostic; it is not silently counted as a successful cancellation. */
void scenario_retry_diagnostic_tick() {
    scenario_listen_tick();
    if (!g_diag_started) {
        g_diag_started = true;
        tag_value("diagnostic_token", kDiagnosticToken, PROV_CONFIGURED);
        tag_value("diagnostic_address", 1u, PROV_CONFIGURED);
        g_retry_test_attempts = g_retry_test_completions = 0;
        g_retry_test_next_ms = 0;
        g_retry_test_first_heap = g_retry_test_first_largest = 0;
        g_retry_test_ok = true;
        g_candidate_active = true;
        set_phase(PHASE_ACTIVATE);
        if (!ble_become_central(kDiagnosticToken)) {
            scenario_end(RUN_FAILED, "retry diagnostic BLE init refused");
            return;
        }
        BLEDevice::getScan()->stop();
    }
    if (g_activation_work.load(std::memory_order_acquire) != WORK_IDLE ||
        static_cast<int32_t>(millis() - g_retry_test_next_ms) < 0) { return; }
    if (g_retry_test_completions != 0u) {
        const HeapReceipt heap = activation_heap();
        log_heap_receipt("settled_retry", heap);
        if (g_retry_test_completions == 1u) {
            g_retry_test_first_heap = heap.free_bytes;
            g_retry_test_first_largest = heap.largest;
        } else {
            /* Frozen tolerance for allocator/controller settling, not tuned
               to a measured run. Retained raw snapshots remain the evidence. */
            g_retry_test_ok = g_retry_test_ok &&
                heap.free_bytes + 512u >= g_retry_test_first_heap &&
                heap.largest + 512u >= g_retry_test_first_largest &&
                g_capture_dropped == 0u;
            log_line("RETRY_RESOURCE_%s attempts=%lu clients=%lu dropped=%lu",
                     g_retry_test_ok ? "PASS" : "FAIL",
                     static_cast<unsigned long>(g_retry_test_attempts),
                     static_cast<unsigned long>(g_client_creations),
                     static_cast<unsigned long>(g_capture_dropped));
            scenario_end(g_retry_test_ok ? RUN_DONE : RUN_FAILED,
                         g_retry_test_ok ? nullptr : "retry resource invariant failed");
            return;
        }
    }
    /* Exercise the same bounded clearing scan-restart path, then keep its
       callback out of the injected-address mailbox. */
    g_activation_cancel.store(false);
    ble_scan_pump();
    BLEDevice::getScan()->stop();
    g_scan_hit.store(true);
    const uint8_t diagnostic_address[6] = {1, 0, 0, 0, 0, 0xC0};
    portENTER_CRITICAL(&g_scan_mux);
    memcpy(g_scan_addr, diagnostic_address, sizeof(g_scan_addr));
    g_scan_addr_type = BLE_ADDR_RANDOM;
    g_scan_raw_len = 0;
    portEXIT_CRITICAL(&g_scan_mux);
    ++g_retry_test_attempts;
    if (!ble_connect_to_hit()) { scenario_end(RUN_FAILED, "retry worker refused request"); }
}

/* Scenario 7: configured-token central carriage, separate from scenario 5's
   scan-only evidence. Uses the production worker while AP remains serviced. */
bool g_central_test_sent = false;
void scenario_central_diagnostic_tick() {
    scenario_listen_tick();
    if (!g_diag_started) {
        g_diag_started = true;
        g_central_test_sent = false;
        tag_value("diagnostic_token", kDiagnosticToken, PROV_CONFIGURED);
        g_candidate_active = true;
        set_phase(PHASE_ACTIVATE);
        if (!ble_become_central(kDiagnosticToken)) {
            scenario_end(RUN_FAILED, "central diagnostic BLE init refused");
        }
        return;
    }
    if (g_run_failure[0] != '\0') { scenario_end(RUN_FAILED, g_run_failure); return; }
    ble_scan_pump();
    if (g_scan_hit.load() && !g_ble_connected && g_activation_work.load() == WORK_IDLE) {
        (void)ble_connect_to_hit();
    }
    uint8_t expected[40];
    for (size_t i = 0; i < sizeof(expected); ++i) { expected[i] = static_cast<uint8_t>(i + 1u); }
    if (g_config.scenario == 7u && g_ble_connected && !g_central_test_sent) {
        g_central_test_sent = true;
        if (ble_send_frame(expected, sizeof(expected)) != 0) {
            scenario_end(RUN_FAILED, "central diagnostic send refused");
        }
    }
    if (g_ble_connected && g_activation_work.load() == WORK_IDLE && g_ble_frame_ready) {
        const bool exact = g_ble_frame_size == sizeof(expected) &&
                           memcmp(g_ble_frame, expected, sizeof(expected)) == 0;
        g_ble_frame_ready = false;
        log_line("CENTRAL_ROUNDTRIP_%s bytes=%u fragments=3", exact ? "PASS" : "FAIL",
                 static_cast<unsigned>(g_ble_frame_size));
        scenario_end(exact ? RUN_DONE : RUN_FAILED, exact ? nullptr : "central echo mismatch");
    }
}

/* ------------------------------------------------------- scenario 3: IP */

/*
 * Scenario 3: MCL-IP CARRIAGE, as a responder.
 *
 * This proves Stable IP-DATAGRAM carriage on this autonomous board. It is NOT
 * stranger discovery over IP and does not pretend to be: MCL defines no global
 * discovery port and this rig does not invent one.
 *
 * What keeps it zero-prior is that the node is told NOTHING about the peer. It
 * listens on a port of its own and learns the peer's address from the datagram
 * that arrives, which is provenance FROM_BEARER. A rig that was handed a peer
 * address would have to tag it CONFIGURED, and the run would report itself as
 * not zero-prior -- which is the point of the tags.
 */
constexpr uint16_t kIpResponderPort = 47100;
WiFiUDP g_udp;
bool    g_udp_open = false;
uint8_t g_udp_buf[MCL_LINK_FRAME_MAX_SIZE];

void scenario_ip_tick() {
    if (!g_udp_open) {
        if (!g_wifi_up) { wifi_up(); http_begin(); }
        g_udp.begin(kIpResponderPort);
        g_udp_open = true;
        log_line("udp responder on %s:%u",
                 WiFi.softAPIP().toString().c_str(),
                 static_cast<unsigned>(kIpResponderPort));
    }

    const int packet = g_udp.parsePacket();
    if (packet <= 0) { return; }

    const int got = g_udp.read(g_udp_buf, sizeof(g_udp_buf));
    if (got <= 0) { return; }

    tag_value("peer_ip", static_cast<uint32_t>(g_udp.remoteIP()), PROV_FROM_BEARER);

    mcl_link_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    size_t consumed = 0;
    const mcl_link_status_t rc =
        mcl_link_frame_decode(g_udp_buf, static_cast<size_t>(got), &frame, &consumed);
    if (rc != MCL_LINK_OK) {
        log_line("udp %d bytes refused rc=%ld", got, static_cast<long>(rc));
        return;
    }
    ++g_counters.frames_recovered;
    log_line("udp frame class=%u major=%u payload=%u from %s",
             static_cast<unsigned>(frame.frame_class),
             static_cast<unsigned>(frame.link_major),
             static_cast<unsigned>(frame.payload_len),
             g_udp.remoteIP().toString().c_str());

    /* Echo the frame back to where it came from. The address is the sender's
       own, read off the datagram; nothing here was configured. */
    g_udp.beginPacket(g_udp.remoteIP(), g_udp.remotePort());
    g_udp.write(g_udp_buf, static_cast<size_t>(got));
    (void)g_udp.endPacket();
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
    j += "\"firmware\":\"v2\",";
    j += "\"wire_major\":" + String(static_cast<unsigned>(MCL_WIRE_STABLE_MAJOR)) + ",";
    j += "\"link_major\":" + String(static_cast<unsigned>(MCL_LINK_STABLE_MAJOR)) + ",";
    j += "\"run_state\":\"" + String(run_state_name(g_run_state)) + "\",";
    j += "\"phase\":\"" + String(phase_name(g_phase)) + "\",";
    j += "\"scenario\":" + String(static_cast<unsigned>(g_config.scenario)) + ",";
    j += "\"uptime_ms\":" + String(millis()) + ",";
    j += "\"free_heap\":" + String(static_cast<unsigned long>(ESP.getFreeHeap())) + ",";
    j += "\"largest_block\":" + String(static_cast<unsigned long>(ESP.getMaxAllocHeap())) + ",";
    j += "\"microphone\":" + String(g_microphone_ready ? "true" : "false") + ",";
    j += "\"speaker\":" + String(g_speaker_ready ? "true" : "false") + ",";
    j += "\"ble_up\":" + String(g_ble_up ? "true" : "false") + ",";
    j += "\"ble_role\":\"" + String(ble_role_name(g_ble_role)) + "\",";
    j += "\"ble_connected\":" + String(g_ble_connected ? "true" : "false") + ",";
    j += "\"arena_bytes\":" + String(static_cast<unsigned>(kArenaBytes)) + ",";
    j += "\"window_samples\":" + String(static_cast<unsigned>(kListenSamples)) + ",";
    j += "\"log_held\":" + String(static_cast<unsigned>(g_log_count)) + ",";
    j += "\"log_dropped\":" + String(g_log_dropped) + ",";
    j += "\"reset_reason\":" + String(g_reset_reason) + ",";
    j += "\"previous\":{";
    j += "\"known\":" + String(g_prev_valid ? "true" : "false") + ",";
    j += "\"incomplete\":" + String(g_prev_incomplete ? "true" : "false") + ",";
    j += "\"scenario\":" + String(static_cast<unsigned>(g_prev_scenario)) + ",";
    j += "\"phase\":\"" + String(phase_name(g_prev_phase)) + "\",";
    j += "\"run_state\":\"" + String(run_state_name(g_prev_run_state)) + "\",";
    j += "\"ble_role\":\"" + String(ble_role_name(g_prev_ble_role)) + "\",";
    j += "\"uptime_ms\":" + String(g_prev_uptime_ms) + ",";
    j += "\"free_heap\":" + String(g_prev_free_heap);
    j += "}";
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
    if (g_http.hasArg("candidate_transport")) {
        const long v = g_http.arg("candidate_transport").toInt();
        if (v != MCL_CONTACT_TRANSPORT_BLE && v != MCL_CONTACT_TRANSPORT_IP) {
            send_json(400, "{\"error\":\"candidate_transport must be 2 (IP) or 3 (BLE)\"}");
            return;
        }
        g_config.candidate_transport = static_cast<uint8_t>(v);
    }
    if (g_http.hasArg("admit_policy")) {
        g_config.admit_policy = (g_http.arg("admit_policy") == "1" ||
                                 g_http.arg("admit_policy") == "true") ? 1u : 0u;
    }

    listener_reset();
    log_line("config scenario=%u dur=%lu band=%u/%u gain=%u quiesce=%u cand=%u admit=%u",
             static_cast<unsigned>(g_config.scenario),
             static_cast<unsigned long>(g_config.duration_ms),
             static_cast<unsigned>(g_config.band_low_hz),
             static_cast<unsigned>(g_config.band_high_hz),
             static_cast<unsigned>(g_config.emit_gain_pct),
             static_cast<unsigned>(g_config.quiesce_wifi ? 1 : 0),
             static_cast<unsigned>(g_config.candidate_transport),
             static_cast<unsigned>(g_config.admit_policy));
    send_json(200, "{\"ok\":true}");
}

/*
 * Arm, and restart into the run. See the note at kArmedMagic: this is what
 * makes every run start from a boot where Wi-Fi has never been up, so a BLE
 * phase has the ~21 KB the network stack would otherwise be holding.
 *
 * The response goes out first. A node that restarted before answering would
 * look to the operator exactly like one that had crashed.
 */
void arm_and_restart() {
    static_assert(sizeof(ScenarioConfig) <= sizeof(g_armed_blob),
                  "armed configuration does not fit in RTC memory");
    memcpy(g_armed_blob, &g_config, sizeof(g_config));
    g_armed_magic = kArmedMagic;
    /*
     * This restart is intended, so it must not be reported as a crash. The
     * post-mortem is invalidated here and nowhere else: every other path to
     * esp_restart() is one this node did not choose, and those are exactly
     * the ones worth seeing.
     */
    g_post_magic = 0u;
    log_line("armed scenario=%u, restarting into the run",
             static_cast<unsigned>(g_config.scenario));
    Serial.flush();
    delay(300);
    esp_restart();
}

void handle_run() {
    if (g_run_state == RUN_RUNNING) {
        send_json(409, "{\"error\":\"run in progress\"}");
        return;
    }
    send_json(200, "{\"ok\":true,\"state\":\"ARMED\",\"note\":\"node restarts into the run\"}");
    delay(100);
    arm_and_restart();
}

void handle_stop() {
    if (g_run_state == RUN_RUNNING || g_run_state == RUN_ARMED) {
        scenario_end(RUN_STOPPED, "stopped by control plane");
    }
    send_json(200, "{\"ok\":true}");
}

void handle_result() {
    const bool live = (g_run_state == RUN_RUNNING || g_run_state == RUN_ARMED);
    const uint32_t cap_blocks = live ? g_cap_blocks : g_run_cap_blocks;
    const uint32_t cap_samples = live ? g_cap_samples : g_run_cap_samples;
    const uint16_t cap_peak = live ? g_cap_peak : g_run_cap_peak;
    const uint32_t cap_mean = live
        ? (g_cap_samples
           ? static_cast<uint32_t>(g_cap_abs_sum / g_cap_samples) : 0u)
        : g_run_cap_mean;
    String j = "{";
    j += "\"run_state\":\"" + String(run_state_name(g_run_state)) + "\",";
    j += "\"phase\":\"" + String(phase_name(g_phase)) + "\",";
    j += "\"scenario\":" + String(static_cast<unsigned>(g_config.scenario)) + ",";
    j += "\"started_ms\":" + String(g_run_started_ms) + ",";
    j += "\"ended_ms\":" + String(g_run_ended_ms) + ",";
    j += "\"zero_prior\":" + String(run_is_zero_prior() ? "true" : "false") + ",";
    j += "\"log_dropped\":" + String(g_log_dropped) + ",";
    j += "\"ble_frames_lost\":" + String(g_ble_frames_lost) + ",";
    j += "\"ble_role\":\"" + String(ble_role_name(g_ble_role)) + "\",";
    j += "\"machine_state\":\"" +
         String(g_machine_running ? mcl_machine_state_name(&g_machine) : "NOT_STARTED") + "\",";
    j += "\"failure\":\"" + json_escape(g_run_failure) + "\",";
    j += "\"counters\":{";
    j += "\"frames_heard\":" + String(g_counters.frames_heard) + ",";
    j += "\"frames_recovered\":" + String(g_counters.frames_recovered) + ",";
    j += "\"frames_emitted\":" + String(g_counters.frames_emitted) + ",";
    j += "\"objects_decoded\":" + String(g_counters.objects_decoded) + ",";
    j += "\"samples_unscanned\":" + String(g_counters.samples_unscanned) + ",";
    j += "\"ble_frames_tx\":" + String(g_counters.ble_frames_tx) + ",";
    j += "\"ble_frames_rx\":" + String(g_counters.ble_frames_rx) + ",";
    j += "\"ble_frag_rejected\":" + String(g_counters.ble_frag_rejected) + ",";
    j += "\"scan_matches\":" + String(g_counters.scan_matches) + ",";
    j += "\"scan_uuid_only\":" + String(g_counters.scan_uuid_only) + ",";
    j += "\"cap_blocks\":" + String(cap_blocks) + ",";
    j += "\"cap_samples\":" + String(cap_samples) + ",";
    j += "\"cap_peak\":" + String(cap_peak) + ",";
    j += "\"cap_mean\":" + String(cap_mean) + ",";
    j += "\"scan_slices\":" + String(g_counters.scan_slices) + ",";
    j += "\"scan_seen\":" + String(g_counters.scan_seen);
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

/*
 * The raw advertising payload the scanner matched, byte for byte.
 *
 * This is what makes "the two implementations encode the same AD structure" a
 * checkable statement rather than "discovery succeeded". A scanner that found
 * something is not evidence; the 26 bytes are.
 */
void handle_scan_record() {
    String j = "{\"len\":" + String(static_cast<unsigned>(g_scan_raw_len)) + ",\"raw\":\"";
    for (size_t i = 0; i < g_scan_raw_len; ++i) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", g_scan_raw[i]);
        j += hex;
    }
    j += "\",\"wanted_token\":" + String(g_wanted_token);
    j += ",\"matches\":" + String(g_counters.scan_matches);
    j += ",\"uuid_only\":" + String(g_counters.scan_uuid_only) + "}";
    send_json(200, j);
}

/* NDJSON: one object per line, so a long log streams without being assembled
   in RAM first. */
void handle_log() {
    g_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_http.send(200, "application/x-ndjson", "");

    const size_t start = (g_log_count < kLogEntries) ? 0 : g_log_head;
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
        "<button onclick=\"go('/api/scan-record')\">scan record</button>"
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
    g_http.on("/api/scan-record", HTTP_GET, handle_scan_record);
    g_http.on("/api/log", HTTP_GET, handle_log);
    g_http.begin();
}

/* ------------------------------------------------------------ run control */

void scenario_start() {
    g_capture_active = false;
    delay(30);  /* let an in-flight I2S read finish before zeroing the receipt */
    g_run_ended_ms = 0;
    g_run_state = RUN_RUNNING;
    g_ble_role = BLE_ROLE_NONE;
    g_machine_running = false;

    /* This node's own reference is generated here, not configured. */
    tag_value("own_source_ref", g_source_ref, PROV_LOCAL);

    listener_reset();
    if (!listener_prepare()) {
        snprintf(g_run_failure, sizeof(g_run_failure),
                 "modem reference preparation failed");
        g_run_state = RUN_FAILED;
        return;
    }
    /* The run interval starts only once the receiver can actually listen.
       Deterministic reference construction is initialization, not deaf time. */
    g_run_started_ms = millis();
    capture_level_reset();
    g_run_cap_blocks = 0;
    g_run_cap_samples = 0;
    g_run_cap_peak = 0;
    g_run_cap_mean = 0;
    g_last_poll_pushed = 0;
    schedule_next_announce();
    g_capture_active = (g_config.scenario < 3u || g_config.scenario == 6u || is_central_diagnostic(g_config.scenario));

    g_diag_started = false;
    const bool needs_ble = (g_config.scenario == 4u || g_config.scenario == 5u || g_config.scenario == 6u || is_central_diagnostic(g_config.scenario) ||
                            (g_config.scenario == 2u &&
                             g_config.candidate_transport == MCL_CONTACT_TRANSPORT_BLE));
    if (g_config.quiesce_wifi || needs_ble) {
        /*
         * Taking Wi-Fi down is not tidiness, and since the memory spike it is
         * not optional either. It removes the control plane from the air for
         * the duration of the exchange, so a result cannot depend on it, and
         * it is the ONLY way a BLE stack comes up beside these audio buffers:
         * BLEDevice::init() is refused with the SoftAP running.
         */
        wifi_down();
        set_phase(PHASE_QUIESCE);
    }

    if (g_config.scenario == 2u) {
        set_phase(PHASE_RENDEZVOUS);
        machine_start();
        if (!g_machine_running) {
            snprintf(g_run_failure, sizeof(g_run_failure),
                     "machine refused to start");
            g_run_state = RUN_FAILED;
            return;
        }
    }

    log_line("run start scenario=%u duration=%lu quiesce=%u free_heap=%lu",
             static_cast<unsigned>(g_config.scenario),
             static_cast<unsigned long>(g_config.duration_ms),
             static_cast<unsigned>((g_config.quiesce_wifi || needs_ble) ? 1 : 0),
             static_cast<unsigned long>(ESP.getFreeHeap()));
}

void scenario_end(uint8_t final_state, const char *why) {
    g_candidate_active = false;
    activation_cancel_io();
    if (g_activation_work.load(std::memory_order_acquire) != WORK_IDLE) {
        g_finish_pending = true;
        g_finish_state = final_state;
        if (why != nullptr && why != g_run_failure) {
            snprintf(g_run_failure, sizeof(g_run_failure), "%s", why);
        }
        return; /* seal only after the worker has relinquished its resources */
    }
    g_finish_pending = false;
    g_capture_active = false;
    delay(30);  /* seal after the producer's current DMA read completes */

    /* The producer owns the time boundary. Drain every sample it accepted
       before sealing; otherwise a frame received at the end of the run could
       be reported as silence merely because it was still queued. */
    while (capture_queued() != 0u) { capture_pump(); }
    if (g_config.scenario == 0u || g_config.scenario == 1u) {
        mcl_ap_listen_result_t r;
        mcl_ap_listen_event_t event;
        uint8_t payload[kMaxBootstrapPayload];
        do {
            const uint32_t poll_started = millis();
            r = mcl_ap_listen_flush(&g_listener, scratch_ptr(), payload,
                                    sizeof(payload), &event);
            const uint32_t poll_ms = millis() - poll_started;
            ++g_poll_calls;
            g_poll_ms_total += poll_ms;
            if (poll_ms > g_poll_ms_max) { g_poll_ms_max = poll_ms; }
            account_listen_result(r, event, payload);
        } while (r == MCL_AP_LISTEN_CONTACT || r == MCL_AP_LISTEN_HEARD);
    }
    g_run_ended_ms = millis();
    g_run_state = final_state;
    if (why != nullptr && why != g_run_failure) {
        snprintf(g_run_failure, sizeof(g_run_failure), "%s", why);
    }
    g_counters.samples_unscanned =
        static_cast<uint32_t>(g_listener.samples_unscanned)
        + g_capture_dropped;
    g_run_cap_blocks = g_cap_blocks;
    g_run_cap_samples = g_cap_samples;
    g_run_cap_peak = g_cap_peak;
    g_run_cap_mean = g_cap_samples
        ? static_cast<uint32_t>(g_cap_abs_sum / g_cap_samples) : 0u;
    /*
     * THE RUN PRINTS ITS OWN RESULT, ACROSS THREE LINES.
     *
     * Reading it back over a second serial connection is not reliable here:
     * opening the port asserts DTR, which on this part's USB-serial-JTAG
     * resets the board, and a reset destroys the counters that were the point
     * of the run. So everything that decides whether a run counts goes out on
     * the wire while the run is still the thing that is running.
     *
     * Three lines rather than one because kLogTextMax is 88 bytes and the ring
     * is 128 entries deep. Widening it to fit one long line would add about
     * 9 KB of static DRAM, and a BLE phase on this board has roughly 15 KB of
     * heap in total -- the log would be bought with the radio.
     */
    log_line("run end state=%s heard=%lu recovered=%lu emitted=%lu unscanned=%lu",
             run_state_name(final_state),
             static_cast<unsigned long>(g_counters.frames_heard),
             static_cast<unsigned long>(g_counters.frames_recovered),
             static_cast<unsigned long>(g_counters.frames_emitted),
             static_cast<unsigned long>(g_counters.samples_unscanned));
    log_line("run end cap_blocks=%lu cap_samples=%lu peak=%lu mean=%lu",
             static_cast<unsigned long>(g_cap_blocks),
             static_cast<unsigned long>(g_cap_samples),
             static_cast<unsigned long>(g_cap_peak),
             static_cast<unsigned long>(g_cap_samples ?
                 (g_cap_abs_sum / g_cap_samples) : 0u));
    log_line("run end pushed=%lu searched=%lu unscanned=%lu queue=%lu/%lu",
             static_cast<unsigned long>(g_listener.total_pushed),
             static_cast<unsigned long>(g_listener.samples_searched),
             static_cast<unsigned long>(g_counters.samples_unscanned),
             static_cast<unsigned long>(capture_queued()),
             static_cast<unsigned long>(g_capture_dropped));
    log_line("run end read_ms=%lu/%lu poll_ms=%lu/%lu polls=%lu",
             static_cast<unsigned long>(g_read_ms_total),
             static_cast<unsigned long>(g_read_ms_max),
             static_cast<unsigned long>(g_poll_ms_total),
             static_cast<unsigned long>(g_poll_ms_max),
             static_cast<unsigned long>(g_poll_calls));
    log_line("run end objects=%lu ble_tx=%lu ble_rx=%lu ble_frag_rej=%lu",
             static_cast<unsigned long>(g_counters.objects_decoded),
             static_cast<unsigned long>(g_counters.ble_frames_tx),
             static_cast<unsigned long>(g_counters.ble_frames_rx),
             static_cast<unsigned long>(g_counters.ble_frag_rejected));
    log_line("run end scan_seen=%lu match=%lu uuid=%lu slices=%lu heap=%lu/%lu",
             static_cast<unsigned long>(g_counters.scan_seen),
             static_cast<unsigned long>(g_counters.scan_matches),
             static_cast<unsigned long>(g_counters.scan_uuid_only),
             static_cast<unsigned long>(g_counters.scan_slices),
             static_cast<unsigned long>(ESP.getFreeHeap()),
             static_cast<unsigned long>(ESP.getMaxAllocHeap()));

    if (g_ble_up) {
        set_phase(PHASE_TEARDOWN);
        ble_stack_down();
    }
    if (!g_wifi_up && !g_ble_up) {
        wifi_up();
        http_begin();
        log_line("wifi restored; result available");
    }
    capture_queue_reset();
    g_capture_active = true;
    set_phase(PHASE_REPORT);
}

void scenario_tick() {
    if (static_cast<int32_t>(millis() - (g_run_started_ms + g_config.duration_ms)) >= 0) {
        if (g_config.scenario == 6u || is_central_diagnostic(g_config.scenario)) {
            scenario_end(RUN_FAILED, "diagnostic deadline expired");
        } else { scenario_end(RUN_DONE, nullptr); }
        return;
    }

    /* Scenario 3 is on Wi-Fi and scenarios 4-5 are BLE diagnostics; neither
       has any use for the microphone, and polling it would only cost time. */
    if (g_config.scenario < 3u || g_config.scenario == 6u || is_central_diagnostic(g_config.scenario)) { capture_pump(); }

    switch (g_config.scenario) {
        case 0: scenario_listen_tick(); break;
        case 1: scenario_announce_tick(); break;
        case 2: scenario_rendezvous_tick(); break;
        case 3: scenario_ip_tick(); break;
        case 4: scenario_ble_diagnostic_tick(true); break;
        case 5: scenario_ble_diagnostic_tick(false); break;
        case 6: scenario_retry_diagnostic_tick(); break;
        case 7: case 8: case 9: case 10: scenario_central_diagnostic_tick(); break;
        default:
            scenario_end(RUN_FAILED, "unknown scenario");
            break;
    }
}

/* ---------------------------------------------------------- serial control */

/*
 * A second control plane, for a board on a cable. Same standing as HTTP and
 * with the same fence: there is no command here that carries a peer address, a
 * token, a reference, a secret or a pairing state, and adding one would make
 * every zero-prior claim this node prints false.
 *
 * It exists because a run armed over HTTP costs a reboot to get its memory
 * back, and because a board being watched on a serial line should not need a
 * second machine to join its SoftAP.
 */
String g_serial_line;

void serial_report_result() {
    Serial.printf("MCLAUTO RESULT state=%s phase=%s zero_prior=%s "
                  "heard=%lu recovered=%lu emitted=%lu objects=%lu "
                  "ble_tx=%lu ble_rx=%lu scan_match=%lu scan_uuid_only=%lu "
                  "log_dropped=%lu ble_lost=%lu machine=%s\n",
                  run_state_name(g_run_state), phase_name(g_phase),
                  run_is_zero_prior() ? "true" : "false",
                  static_cast<unsigned long>(g_counters.frames_heard),
                  static_cast<unsigned long>(g_counters.frames_recovered),
                  static_cast<unsigned long>(g_counters.frames_emitted),
                  static_cast<unsigned long>(g_counters.objects_decoded),
                  static_cast<unsigned long>(g_counters.ble_frames_tx),
                  static_cast<unsigned long>(g_counters.ble_frames_rx),
                  static_cast<unsigned long>(g_counters.scan_matches),
                  static_cast<unsigned long>(g_counters.scan_uuid_only),
                  static_cast<unsigned long>(g_log_dropped),
                  static_cast<unsigned long>(g_ble_frames_lost),
                  g_machine_running ? mcl_machine_state_name(&g_machine) : "NOT_STARTED");
    for (size_t i = 0; i < g_tagged_count; ++i) {
        Serial.printf("MCLAUTO VALUE %s=%lu provenance=%s\n",
                      g_tagged[i].name,
                      static_cast<unsigned long>(g_tagged[i].value),
                      provenance_name(g_tagged[i].provenance));
    }
    Serial.printf("MCLAUTO SCANRAW len=%u hex=", static_cast<unsigned>(g_scan_raw_len));
    for (size_t i = 0; i < g_scan_raw_len; ++i) { Serial.printf("%02X", g_scan_raw[i]); }
    Serial.println();
}

void handle_serial_line(const String &line) {
    /* CONFIG <scenario> <duration_ms> <band_low> <band_high> <gain> <candidate> <admit> */
    if (line.startsWith("CONFIG ")) {
        int values[7] = {0, 0, 0, 0, 0, 0, 0};
        int count = 0;
        int from = 7;
        while (count < 7 && from < static_cast<int>(line.length())) {
            const int space = line.indexOf(' ', from);
            const String token = (space < 0) ? line.substring(from) : line.substring(from, space);
            values[count++] = token.toInt();
            if (space < 0) { break; }
            from = space + 1;
        }
        if (count < 7) { Serial.println("MCLAUTO ERR CONFIG needs 7 values"); return; }
        if (values[4] < 1 || values[4] > 100) {
            Serial.println("MCLAUTO ERR gain out of range 1..100"); return;
        }
        if (values[5] != MCL_CONTACT_TRANSPORT_BLE && values[5] != MCL_CONTACT_TRANSPORT_IP) {
            Serial.println("MCLAUTO ERR candidate_transport must be 2 or 3"); return;
        }
        g_config.scenario = static_cast<uint8_t>(values[0]);
        g_config.duration_ms = static_cast<uint32_t>(values[1]);
        g_config.band_low_hz = static_cast<uint16_t>(values[2]);
        g_config.band_high_hz = static_cast<uint16_t>(values[3]);
        g_config.emit_gain_pct = static_cast<uint8_t>(values[4]);
        g_config.candidate_transport = static_cast<uint8_t>(values[5]);
        g_config.admit_policy = static_cast<uint8_t>(values[6] ? 1 : 0);
        g_config.quiesce_wifi = true;
        listener_reset();
        Serial.printf("MCLAUTO CONFIG ok scenario=%u dur=%lu band=%u/%u gain=%u cand=%u admit=%u\n",
                      static_cast<unsigned>(g_config.scenario),
                      static_cast<unsigned long>(g_config.duration_ms),
                      static_cast<unsigned>(g_config.band_low_hz),
                      static_cast<unsigned>(g_config.band_high_hz),
                      static_cast<unsigned>(g_config.emit_gain_pct),
                      static_cast<unsigned>(g_config.candidate_transport),
                      static_cast<unsigned>(g_config.admit_policy));
        return;
    }
    if (line == "ARM") {
        if (g_run_state == RUN_RUNNING) { Serial.println("MCLAUTO ERR run in progress"); return; }
        Serial.println("MCLAUTO ARMED restarting into the run");
        arm_and_restart();
        return;
    }
    if (line == "STOP") {
        if (g_run_state == RUN_RUNNING || g_run_state == RUN_ARMED) {
            scenario_end(RUN_STOPPED, "stopped by control plane");
        }
        Serial.println(g_finish_pending ? "MCLAUTO STOPPING" : "MCLAUTO STOPPED");
        return;
    }
    if (line == "STATUS") {
        Serial.printf("MCLAUTO STATUS state=%s phase=%s scenario=%u free_heap=%lu "
                      "largest=%lu psram=%lu/%lu queue=%u/%lu wifi=%u ble=%u "
                      "role=%s source_ref=%lu\n",
                      run_state_name(g_run_state), phase_name(g_phase),
                      static_cast<unsigned>(g_config.scenario),
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                      static_cast<unsigned long>(ESP.getFreePsram()),
                      static_cast<unsigned long>(ESP.getPsramSize()),
                      static_cast<unsigned>(capture_queued()),
                      static_cast<unsigned long>(g_capture_dropped),
                      static_cast<unsigned>(g_wifi_up ? 1 : 0),
                      static_cast<unsigned>(g_ble_up ? 1 : 0),
                      ble_role_name(g_ble_role),
                      static_cast<unsigned long>(g_source_ref));
        return;
    }
    if (line == "RESULT") { serial_report_result(); return; }
    if (line == "LOG") {
        const size_t start = (g_log_count < kLogEntries) ? 0 : g_log_head;
        for (size_t n = 0; n < g_log_count; ++n) {
            const size_t i = (start + n) % kLogEntries;
            Serial.printf("MCLAUTO LOG %lu %s\n",
                          static_cast<unsigned long>(g_log[i].at_ms), g_log[i].text);
        }
        Serial.printf("MCLAUTO LOG END held=%u dropped=%lu\n",
                      static_cast<unsigned>(g_log_count),
                      static_cast<unsigned long>(g_log_dropped));
        return;
    }
    /*
     * BLETEST <role>: bring the radio up on THIS image, in one role, and print
     * the heap. 1 = peripheral/advertiser, 2 = central/scanner.
     *
     * A diagnostic, not a run: it exchanges nothing and touches no rendezvous
     * state. It exists because the memory verdict in spike-ble-memory/ was
     * measured on a smaller image than this one, and a claim that BLE fits
     * should be checkable against the firmware that actually ships.
     */
    if (line.startsWith("BLETEST")) {
        if (g_run_state == RUN_RUNNING) { Serial.println("MCLAUTO ERR run in progress"); return; }
        const int role = (line.length() > 8) ? line.substring(8).toInt() : 1;
        wifi_down();
        Serial.printf("MCLAUTO BLETEST before free=%lu largest=%lu\n",
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        const bool ok = (role == 2) ? ble_become_central(0xDEADBEEFu)
                                    : ble_become_peripheral(0xDEADBEEFu);
        Serial.printf("MCLAUTO BLETEST role=%s result=%s free=%lu largest=%lu\n",
                      ble_role_name(g_ble_role), ok ? "UP" : "REFUSED",
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        return;
    }
    if (line == "BLEDOWN") {
        ble_stack_down();
        g_ble_role = BLE_ROLE_NONE;
        Serial.printf("MCLAUTO BLEDOWN free=%lu largest=%lu\n",
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()));
        return;
    }
    if (line == "WIFI ON") {
        wifi_up();
        http_begin();
        Serial.println("MCLAUTO WIFI up");
        return;
    }
    if (line == "WIFI OFF") { wifi_down(); Serial.println("MCLAUTO WIFI down"); return; }
    if (line.length() > 0) { Serial.println("MCLAUTO ERR unknown command"); }
}

void serial_pump() {
    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (g_serial_line.length() > 0) {
                String line = g_serial_line;
                g_serial_line = "";
                line.trim();
                handle_serial_line(line);
            }
        } else if (g_serial_line.length() < 96) {
            g_serial_line += c;
        }
    }
}

}  /* namespace */

/* ------------------------------------------------------------------ boot */

void allocation_failure_receipt(size_t requested, uint32_t caps, const char *function_name) {
    /* ROM output does not allocate; allocation-failure callbacks must not
       recurse through the heap or the application log ring. */
    esp_rom_printf("MCL_ALLOC_FAIL bytes=%u caps=%08x function=%s\n",
                   static_cast<unsigned>(requested), static_cast<unsigned>(caps), function_name);
}

void setup() {
    heap_caps_register_failed_alloc_callback(allocation_failure_receipt);
    pinMode(kActivityLedPin, OUTPUT);
    digitalWrite(kActivityLedPin, LOW);

    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(500);
    Serial.begin(921600);
    const uint32_t waited = millis();
    while (!Serial && millis() - waited < 3000) { delay(10); }

    g_log = static_cast<LogEntry *>(heap_caps_calloc(kLogEntries, sizeof(LogEntry),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_log == nullptr) {
        Serial.println("MCLAUTO FATAL PSRAM_LOG_UNAVAILABLE");
        for (;;) { delay(1000); }
    }
    if (xTaskCreatePinnedToCore(activation_worker, "mcl-ble-activate", 4096,
                               nullptr, 1, &g_activation_task, 1) != pdPASS) {
        Serial.println("MCLAUTO FATAL BLE_WORKER_UNAVAILABLE");
        for (;;) { delay(1000); }
    }

    bool address_ok = true;
    for (uint8_t type : {static_cast<uint8_t>(BLE_ADDR_PUBLIC), static_cast<uint8_t>(BLE_ADDR_RANDOM)}) {
        BLEAddress original(String("c1:23:45:67:89:ab"), type);
        BLEAddress broken(original.getNative(), type); /* negative control */
        BLEAddress restored = address_from_native(original.getNative(), type);
        const bool pass = broken != original && restored == original &&
            memcmp(restored.getNative(), original.getNative(), 6) == 0;
        address_ok = address_ok && pass;
        log_line("ADDRESS_SELFTEST %s type=%u original=%s old=%s", pass ? "PASS" : "FAIL",
                 type, original.toString().c_str(), broken.toString().c_str());
    }
    if (!address_ok) {
        Serial.println("MCLAUTO FATAL NATIVE_ADDRESS_ROUNDTRIP_FAILED");
        for (;;) { delay(1000); }
    }
    uint8_t ad_test[27] = {25, MCL_BLE_AD_TYPE_SERVICE_DATA_128};
    uint8_t beacon_test[MCL_RENDEZVOUS_BEACON_SIZE];
    bool uuid_seen_test = false;
    beacon_from_token(kDiagnosticToken, beacon_test);
    memcpy(ad_test + 2, kServiceUuidLe, sizeof(kServiceUuidLe));
    memcpy(ad_test + 18, beacon_test, sizeof(beacon_test));
    bool ad_ok = advertisement_matches(ad_test, 26, beacon_test, &uuid_seen_test);
    ad_ok = ad_ok && !advertisement_matches(ad_test, 25, beacon_test, &uuid_seen_test);
    ad_test[0] = 26;
    ad_ok = ad_ok && !advertisement_matches(ad_test, 27, beacon_test, &uuid_seen_test);
    ad_test[0] = 25;
    ad_test[18] ^= 1u;
    ad_ok = ad_ok && !advertisement_matches(ad_test, 26, beacon_test, &uuid_seen_test);
    log_line("BEACON_SELFTEST %s exact/truncated/overlong/wrong-token", ad_ok ? "PASS" : "FAIL");
    if (!ad_ok) {
        Serial.println("MCLAUTO FATAL BEACON_CONTRACT_FAILED");
        for (;;) { delay(1000); }
    }

    /* Randomised at boot: see the note at g_source_ref. */
    g_source_ref = esp_random();

    microphone.setPinsPdmRx(kPdmClockPin, kPdmDataPin);
    g_microphone_ready = microphone.begin(I2S_MODE_PDM_RX, kSampleRateHz,
                                          I2S_DATA_BIT_WIDTH_16BIT,
                                          I2S_SLOT_MODE_MONO);
    if (!g_microphone_ready) { Serial.println("MCLAUTO WARN PDM_INIT_FAILED"); }

    if (ESP.getPsramSize() != 0u) {
        g_capture_ring = static_cast<int16_t *>(
            ps_malloc(kCaptureRingSamples * sizeof(int16_t)));
        g_capture_consumer_block = static_cast<int16_t *>(
            ps_malloc(kCaptureConsumerSamples * sizeof(int16_t)));
    }
    if (g_capture_consumer_block == nullptr) {
        Serial.println("MCLAUTO FATAL PSRAM_CAPTURE_STAGING_UNAVAILABLE");
        for (;;) { delay(1000); }
    }
    if (g_capture_ring == nullptr) {
        Serial.println("MCLAUTO FATAL PSRAM_CAPTURE_QUEUE_UNAVAILABLE");
    } else {
        const BaseType_t task_ok =
            xTaskCreatePinnedToCore(capture_task_main, "mcl-audio-capture",
                                    4096, nullptr, 3, &g_capture_task, 0);
        if (task_ok != pdPASS) {
            g_capture_ring = nullptr;
            Serial.println("MCLAUTO FATAL CAPTURE_TASK_CREATE_FAILED");
        } else {
            g_capture_active = true;
        }
    }

    speaker.setPins(kAmpBclkPin, kAmpLrclkPin, kAmpDataPin);
    g_speaker_ready = speaker.begin(I2S_MODE_STD, kSampleRateHz,
                                    I2S_DATA_BIT_WIDTH_16BIT,
                                    I2S_SLOT_MODE_MONO);
    if (!g_speaker_ready) { Serial.println("MCLAUTO WARN AMP_INIT_FAILED"); }

    listener_reset();

    /*
     * The window size is CHECKED, not asserted. 47 360 samples is the minimum
     * for a 17-byte payload today; if a profile constant ever moves, a node
     * that quietly listened through a window too short for its own frames
     * would report a dead room rather than a configuration error.
     */
    const size_t need = mcl_ap_listen_min_window_samples(&g_listen_config);
    if (need == 0u || need > kListenSamples) {
        Serial.printf("MCLAUTO FATAL window %u samples, profile needs %u\n",
                      static_cast<unsigned>(kListenSamples),
                      static_cast<unsigned>(need));
    }

    /*
     * A run armed before this boot restarts INTO the run, and Wi-Fi is not
     * brought up for it: see the note at kArmedMagic. The magic is cleared
     * first, so a crash during a run cannot make the node loop forever
     * restarting into the thing that crashed it.
     */
    bool armed_on_boot = false;
    /*
     * Read the previous boot's footprint before this boot overwrites it.
     * "Incomplete" is the honest word: RUN_RUNNING or RUN_ARMED at the moment
     * the lights went out means the run did not reach an ending, whatever the
     * reason code says. A brownout, a panic and a watchdog are all the same
     * fact to the evidence -- the run did not finish, so it does not count.
     */
    g_reset_reason = static_cast<int>(esp_reset_reason());
    if (g_post_magic == kPostMagic) {
        g_prev_valid      = true;
        g_prev_scenario   = g_post_scenario;
        g_prev_phase      = g_post_phase;
        g_prev_run_state  = g_post_run_state;
        g_prev_ble_role   = g_post_ble_role;
        g_prev_uptime_ms  = g_post_uptime_ms;
        g_prev_free_heap  = g_post_free_heap;
        g_prev_incomplete = (g_prev_run_state == RUN_RUNNING ||
                             g_prev_run_state == RUN_ARMED);
    }
    g_post_magic = 0u;

    if (g_armed_magic == kArmedMagic) {
        g_armed_magic = 0u;
        memcpy(&g_config, g_armed_blob, sizeof(g_config));
        armed_on_boot = true;
    }

    if (!armed_on_boot) {
        wifi_up();
        http_begin();
    }

    Serial.printf("MCLAUTO READY wire_major=%u link_major=%u arena=%u window=%u "
                  "scratch=%u min_window=%u free_heap=%lu psram=%lu/%lu source_ref=%lu\n",
                  static_cast<unsigned>(MCL_WIRE_STABLE_MAJOR),
                  static_cast<unsigned>(MCL_LINK_STABLE_MAJOR),
                  static_cast<unsigned>(kArenaBytes),
                  static_cast<unsigned>(kListenSamples),
                  static_cast<unsigned>(sizeof(mcl_ap_modem_scratch_t)),
                  static_cast<unsigned>(need),
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getFreePsram()),
                  static_cast<unsigned long>(ESP.getPsramSize()),
                  static_cast<unsigned long>(g_source_ref));

    if (armed_on_boot) {
        g_log_count = 0;
        g_log_head = 0;
        g_log_dropped = 0;
        g_tagged_count = 0;
        g_counters = RunCounters{0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        g_ble_frames_lost = 0;
        g_own_token = 0;
        g_run_failure[0] = '\0';
        g_run_state = RUN_ARMED;
        log_line("armed run resumed after restart, wifi never started");
    }
}

/*
 * Mirror the live footprint into RTC memory. Called every loop: these are six
 * word writes to memory that is already mapped, and the alternative -- update
 * it only at phase changes -- loses precisely the run that dies mid-phase.
 */
void post_mortem_mark() {
    g_post_magic     = kPostMagic;
    g_post_scenario  = g_config.scenario;
    g_post_phase     = g_phase;
    g_post_run_state = g_run_state;
    g_post_ble_role  = g_ble_role;
    g_post_uptime_ms = millis();
    g_post_free_heap = static_cast<uint32_t>(ESP.getFreeHeap());
}

void loop() {
    post_mortem_mark();
    activation_service();
    if (g_phase == PHASE_REPORT && !g_wifi_up && !g_ble_up && !g_candidate_close_pending) {
        wifi_up();
        http_begin();
        log_line("wifi restored after activation cleanup");
    }
    serial_pump();
    if (g_wifi_up) { g_http.handleClient(); }
    if (g_finish_pending) {
        if (g_activation_work.load(std::memory_order_acquire) == WORK_IDLE) {
            scenario_end(g_finish_state, g_run_failure);
        } else {
            capture_pump();
            scenario_listen_tick();
            delay(1);
        }
        return;
    }

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

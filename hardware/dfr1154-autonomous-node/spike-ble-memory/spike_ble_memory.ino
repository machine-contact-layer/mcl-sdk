/*
 * DFR1154 BLE memory feasibility spike.
 *
 * LAB INSTRUMENT. This is not an MCL node, it exchanges nothing, and no
 * evidence about the protocol comes out of it. It answers one question that
 * had to be answered before another thousand lines were written:
 *
 *   Can a BLE stack coexist, on this board, with the static allocations the
 *   autonomous node already makes?
 *
 * WHY A SPIKE AND NOT "JUST ADD BLE AND SEE"
 *
 * Two different failures both look like "BLE does not work here", and only one
 * of them can be worked around at runtime:
 *
 *   STATIC   the image does not link -- `.dram0.bss will not fit` -- because
 *            the arena, the modem scratch and the radio stacks' own static
 *            data exceed internal DRAM. Shutting Wi-Fi down at runtime cannot
 *            repair this: the memory was never available to begin with.
 *
 *   RUNTIME  the image links and BLEDevice::init() fails, or leaves too little
 *            heap to open a connection, because Wi-Fi is holding heap that BLE
 *            wants. This one IS repairable, by quiescing Wi-Fi first -- which
 *            the node already does for evidence reasons.
 *
 * So this sketch allocates EXACTLY what the node allocates, links the real BLE
 * library, and then measures the heap at every stage in order, including one
 * deliberate attempt to bring BLE up while Wi-Fi is still running. A spike that
 * measured a smaller program than the node will be would be worse than no
 * measurement, because it would be believed.
 *
 * WHAT IT PRINTS
 *
 *   SPIKE STATIC ...                 sizes of the node's static allocations
 *   SPIKE STAGE <name> free=... ...  heap at each stage, in order
 *   SPIKE RESULT ...                 fits / does not fit, and by which path
 *
 * Nothing is written to flash, no radio carries an MCL object, and the sketch
 * ends parked in a loop reprinting the final line.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_heap_caps.h"
#include "esp_wifi.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLE2902.h>

#include "mcl/ap_listen.h"
#include "mcl/ap_modem.h"
#include "mcl/wire.h"
#include "mcl/link.h"
#include "mcl/sdk.h"
#include "mcl/rendezvous.h"
#include "mcl/ble_binding.h"

namespace {

/* --------------------------------------------------------- the node's RAM */

/*
 * These four declarations are the point of the spike. They are copied from the
 * node deliberately -- same sizes, same types, same order -- because what is
 * being measured is whether a BLE stack fits BESIDE them.
 */
/*
 * Both sizes are overridable from the build so the DEFICIT can be measured
 * rather than guessed. A link failure says "does not fit"; it does not say by
 * how much, and the remedy depends entirely on the number.
 */
#ifndef SPIKE_ARENA_SAMPLES
#define SPIKE_ARENA_SAMPLES 66560            /* 130 KB: the 17-byte waveform */
#endif
#ifndef SPIKE_WITH_SCRATCH
#define SPIKE_WITH_SCRATCH 1                 /* 75 KB: the modem's working set */
#endif

/*
 * SPIKE_HEAP_ARENA: hold the audio working set on the heap instead of in .bss,
 * and release it before BLE comes up.
 *
 * The node's phases are strictly sequential -- acoustic rendezvous, THEN the
 * candidate bearer -- and the listener is deaf while the radio work happens
 * regardless. A static arena is therefore memory the BLE stack can never have,
 * even though nothing is using it. Measured against the static layout so the
 * choice rests on numbers.
 */
#ifndef SPIKE_HEAP_ARENA
#define SPIKE_HEAP_ARENA 0
#endif

/* The union layout, both variants: the listener's window and the modem
   scratch coexist during reception; the transmit waveform overlaps both, and
   the listener is reset either side of a transmission. */
constexpr size_t kListenSamples    = 47360;   /* minimum window, 17-byte payload */
constexpr size_t kTxWaveformBytes  = 133120;  /* a 17-byte object, modulated */
constexpr size_t kUnionArenaBytes  =
    (kListenSamples * sizeof(int16_t) + sizeof(mcl_ap_modem_scratch_t) > kTxWaveformBytes)
        ? (kListenSamples * sizeof(int16_t) + sizeof(mcl_ap_modem_scratch_t))
        : kTxWaveformBytes;

constexpr size_t kArenaSamples = SPIKE_ARENA_SAMPLES;
int16_t g_arena[kArenaSamples];

#if SPIKE_HEAP_ARENA
uint8_t *g_heap_arena = nullptr;
#endif

#if SPIKE_WITH_SCRATCH
mcl_ap_modem_scratch_t g_scratch;
#endif
mcl_ap_listener_t      g_listener;
mcl_ap_listen_config_t g_listen_config;

/* What the finished node adds on top: one contact, one coordinator, one
   reassembler. Small next to the audio, and included so the number is real. */
mcl_node_t          g_node;
mcl_rdv_t           g_rdv;
mcl_ble_reassembler_t g_reasm;

WebServer g_http(80);

const char *kApSsid = "mcl-auto-node";
const char *kApPass = "mcl-lab-node";

/* ------------------------------------------------------------ BLE objects */

/* BLE-GATT-1 §2. The same service and characteristics the node will carry, so
   the GATT table this measures is the GATT table it will build. */
#define MCL_SERVICE_UUID "6d636c00-0001-4d43-4c00-6d636c626c65"
#define MCL_RX_CHAR_UUID "6d636c00-0002-4d43-4c00-6d636c626c65"
#define MCL_TX_CHAR_UUID "6d636c00-0003-4d43-4c00-6d636c626c65"

BLEServer         *g_server  = nullptr;
BLECharacteristic *g_tx_char = nullptr;
BLEScan           *g_scan    = nullptr;
BLEClient         *g_client  = nullptr;

class SpikeScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice device) override {
        /* Counted, not acted on. A spike that connected to strangers would be
           a different experiment. */
        ++g_seen;
    }
public:
    uint32_t g_seen = 0;
};
SpikeScanCallbacks g_scan_callbacks;

/* ---------------------------------------------------------- measurement */

struct Stage {
    const char *name;
    uint32_t free_heap;
    uint32_t largest_block;
    uint32_t min_free_ever;
    uint32_t internal_free;
};

constexpr size_t kMaxStages = 16;
Stage  g_stages[kMaxStages];
size_t g_stage_count = 0;

void record(const char *name) {
    if (g_stage_count >= kMaxStages) { return; }
    Stage &s = g_stages[g_stage_count++];
    s.name = name;
    s.free_heap = static_cast<uint32_t>(ESP.getFreeHeap());
    s.largest_block = static_cast<uint32_t>(ESP.getMaxAllocHeap());
    s.min_free_ever = static_cast<uint32_t>(ESP.getMinFreeHeap());
    s.internal_free = static_cast<uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    Serial.printf("SPIKE STAGE %-18s free=%lu largest=%lu min_free_ever=%lu internal=%lu\n",
                  s.name,
                  static_cast<unsigned long>(s.free_heap),
                  static_cast<unsigned long>(s.largest_block),
                  static_cast<unsigned long>(s.min_free_ever),
                  static_cast<unsigned long>(s.internal_free));
    Serial.flush();
}

/* ---------------------------------------------------------------- stages */

void wifi_and_http_up() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
    g_http.on("/", []() { g_http.send(200, "text/plain", "spike"); });
    g_http.begin();
}

void wifi_down() {
    g_http.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    /* The event loop and the driver release their heap asynchronously; without
       this the "after Wi-Fi shutdown" reading is taken before the shutdown has
       finished and reports less recovery than actually happens. */
    delay(500);
}

/*
 * WI-FI OFF IS NOT WI-FI GONE.
 *
 * WiFi.mode(WIFI_OFF) stops the radio and returns most of the SoftAP's heap,
 * and it measurably does NOT return all of it. Whether the remainder can be
 * recovered decides how much margin a BLE connection has, so it is measured
 * rather than assumed. Returns the driver's own status so a refusal is
 * reported instead of being read as a saving.
 */
esp_err_t wifi_deinit_fully() {
    const esp_err_t rc = esp_wifi_deinit();
    delay(300);
    return rc;
}

bool ble_bring_up_stack() {
    /* Just the controller and host. If this fails, nothing above it matters. */
    return BLEDevice::init("MCL-SPIKE");
}

void ble_build_gatt() {
    g_server = BLEDevice::createServer();
    BLEService *service = g_server->createService(MCL_SERVICE_UUID);

    service->createCharacteristic(MCL_RX_CHAR_UUID,
                                  BLECharacteristic::PROPERTY_WRITE |
                                  BLECharacteristic::PROPERTY_WRITE_NR);
    g_tx_char = service->createCharacteristic(MCL_TX_CHAR_UUID,
                                              BLECharacteristic::PROPERTY_NOTIFY);
    g_tx_char->addDescriptor(new BLE2902());
    service->start();
}

void ble_advertise() {
    /*
     * BLE-ACTIVATE-1 §3 / BLE-GATT-1 §7: one Service Data - 128-bit AD
     * structure, the MCL UUID, and the 8-byte beacon carrying a zero-extended
     * big-endian endpoint_token. The token here is a placeholder: the spike is
     * measuring the advertiser's cost, not running an activation.
     */
    const uint32_t token = 0x12345678u;
    uint8_t beacon[MCL_RENDEZVOUS_BEACON_SIZE] = {0};
    beacon[4] = static_cast<uint8_t>((token >> 24) & 0xFFu);
    beacon[5] = static_cast<uint8_t>((token >> 16) & 0xFFu);
    beacon[6] = static_cast<uint8_t>((token >> 8) & 0xFFu);
    beacon[7] = static_cast<uint8_t>(token & 0xFFu);

    String payload;
    for (size_t i = 0; i < sizeof(beacon); ++i) {
        payload += static_cast<char>(beacon[i]);
    }

    BLEAdvertisementData adv_data;
    adv_data.setServiceData(BLEUUID(MCL_SERVICE_UUID), payload);

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->setAdvertisementData(adv_data);
    advertising->setScanResponse(false);
    BLEDevice::startAdvertising();
}

void ble_scan_start() {
    g_scan = BLEDevice::getScan();
    g_scan->setAdvertisedDeviceCallbacks(&g_scan_callbacks, false, true);
    g_scan->setActiveScan(false);   /* passive: the beacon is in the adv itself */
    g_scan->setInterval(100);
    g_scan->setWindow(80);
    /* Duration 0 would block forever in the blocking form; the non-blocking
       form with a duration is what the node will use. */
    g_scan->start(5, nullptr, false);
}

void ble_client_alloc() {
    /* A central allocates its client object before it connects. Measured
       because the node needs BOTH orientations in one image. */
    g_client = BLEDevice::createClient();
}

}  /* namespace */

void setup() {
    Serial.setTxBufferSize(4096);
    Serial.setTxTimeoutMs(500);
    Serial.begin(921600);
    const uint32_t waited = millis();
    while (!Serial && millis() - waited < 3000) { delay(10); }

    Serial.println();
    Serial.println("SPIKE BEGIN dfr1154 ble memory feasibility");
#if SPIKE_WITH_SCRATCH
    const unsigned scratch_bytes = static_cast<unsigned>(sizeof(g_scratch));
#else
    const unsigned scratch_bytes = 0u;
#endif
    Serial.printf("SPIKE STATIC arena=%u scratch=%u listener=%u node=%u rdv=%u reasm=%u total=%u\n",
                  static_cast<unsigned>(sizeof(g_arena)),
                  scratch_bytes,
                  static_cast<unsigned>(sizeof(g_listener)),
                  static_cast<unsigned>(sizeof(g_node)),
                  static_cast<unsigned>(sizeof(g_rdv)),
                  static_cast<unsigned>(sizeof(g_reasm)),
                  static_cast<unsigned>(sizeof(g_arena) + scratch_bytes +
                                        sizeof(g_listener) + sizeof(g_node) +
                                        sizeof(g_rdv) + sizeof(g_reasm)));

    /*
     * TOUCH EVERY STATIC, THROUGH A VOLATILE POINTER.
     *
     * The first version of this spike declared the modem scratch and never
     * used it, and the linker removed it: 181 892 bytes of "global variables"
     * against the node's own 271 680, which would have been reported as BLE
     * fitting comfortably in memory the image had simply not allocated. A
     * feasibility measurement that quietly measures a smaller program than the
     * one being planned is worse than no measurement.
     */
    volatile uint8_t *p;
    p = reinterpret_cast<volatile uint8_t *>(g_arena);        p[0] = 1; p[sizeof(g_arena) - 1] = 1;
#if SPIKE_WITH_SCRATCH
    p = reinterpret_cast<volatile uint8_t *>(&g_scratch);     p[0] = 1; p[sizeof(g_scratch) - 1] = 1;
#endif
    p = reinterpret_cast<volatile uint8_t *>(&g_node);        p[0] = 1; p[sizeof(g_node) - 1] = 1;
    p = reinterpret_cast<volatile uint8_t *>(&g_rdv);         p[0] = 1; p[sizeof(g_rdv) - 1] = 1;
    p = reinterpret_cast<volatile uint8_t *>(&g_reasm);       p[0] = 1; p[sizeof(g_reasm) - 1] = 1;

    mcl_ap_listen_default_config(&g_listen_config);
    g_listen_config.max_payload_bytes = 17u;
    (void)mcl_ap_listen_init(&g_listener, &g_listen_config, g_arena, kArenaSamples);

    record("boot");

#if SPIKE_HEAP_ARENA
    /*
     * MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, explicitly. The correlation inner
     * loop reads this block tens of millions of times per acquisition and must
     * not land in PSRAM by default on a board that has 8 MB of it.
     */
    g_heap_arena = static_cast<uint8_t *>(
        heap_caps_malloc(kUnionArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    Serial.printf("SPIKE NOTE heap arena %u bytes: %s\n",
                  static_cast<unsigned>(kUnionArenaBytes),
                  (g_heap_arena != nullptr) ? "allocated" : "REFUSED");
    if (g_heap_arena != nullptr) {
        g_heap_arena[0] = 1;
        g_heap_arena[kUnionArenaBytes - 1] = 1;
    }
    record(g_heap_arena ? "arena_on_heap" : "arena_on_heap_REFUSED");
#endif

    wifi_and_http_up();
    record("wifi_softap_http");

#if SPIKE_HEAP_ARENA
    /* The acoustic phase is over before the candidate bearer is opened, so the
       window and the scratch go back before the radio asks for memory. */
    heap_caps_free(g_heap_arena);
    g_heap_arena = nullptr;
    record("arena_released");
#endif

    /*
     * THE ATTEMPT THAT DECIDES THE NODE'S PHASE MACHINE.
     *
     * If BLE comes up here, the node may keep its control plane on the air
     * during an activation and the quiesce step is an evidence decision only.
     * If it does not, quiescing Wi-Fi is a hard requirement and the node must
     * sequence around it.
     */
    const bool ble_with_wifi = ble_bring_up_stack();
    record(ble_with_wifi ? "ble_init_wifi_up" : "ble_init_wifi_up_FAILED");
    Serial.printf("SPIKE NOTE ble_init_with_wifi_up=%s\n", ble_with_wifi ? "ok" : "refused");

    bool ble_ok = ble_with_wifi;
    if (!ble_ok) {
        wifi_down();
        record("wifi_down");

        const esp_err_t deinit_rc = wifi_deinit_fully();
        Serial.printf("SPIKE NOTE esp_wifi_deinit rc=%d (%s)\n", (int)deinit_rc,
                      deinit_rc == ESP_OK ? "ok" : "refused");
        record(deinit_rc == ESP_OK ? "wifi_deinit" : "wifi_deinit_REFUSED");

        ble_ok = ble_bring_up_stack();
        record(ble_ok ? "ble_init_wifi_down" : "ble_init_wifi_down_FAILED");
    }

    if (!ble_ok) {
        Serial.println("SPIKE RESULT ble=NO_INIT verdict=DOES_NOT_FIT");
        return;
    }

    ble_build_gatt();
    record("ble_gatt_server");

    ble_advertise();
    record("ble_advertising");

    ble_scan_start();
    record("ble_scanning");

    ble_client_alloc();
    record("ble_client_alloc");

    if (ble_with_wifi) {
        /* Wi-Fi was never taken down above; measure the recovery anyway, since
           the node quiesces for evidence reasons regardless. */
        wifi_down();
        record("wifi_down_after_ble");
    }

#if SPIKE_HEAP_ARENA
    /*
     * THE RETURN TRIP IS THE PART THAT CAN FAIL.
     *
     * Taking 168 KB back after a radio stack has been up and down is a
     * fragmentation question, not an arithmetic one, and a node that could not
     * listen again after its first BLE activation would be useless for a
     * multi-round campaign. Measured here rather than discovered in a run.
     */
    uint8_t *again = static_cast<uint8_t *>(
        heap_caps_malloc(kUnionArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    Serial.printf("SPIKE NOTE arena re-acquire after BLE: %s\n",
                  (again != nullptr) ? "ok" : "REFUSED");
    record(again ? "arena_reacquired" : "arena_reacquire_REFUSED");
    if (again != nullptr) { heap_caps_free(again); }

    /*
     * TWO BLOCKS INSTEAD OF ONE.
     *
     * The window and the modem scratch are separate objects and never have to
     * be contiguous -- only the transmit waveform needs one run of 130 KB. So
     * if a single 168 KB block is refused after BLE has fragmented the heap,
     * the receive path may still be satisfiable in two pieces. Worth knowing
     * before the node is written around either answer.
     */
    uint8_t *win = static_cast<uint8_t *>(
        heap_caps_malloc(kListenSamples * sizeof(int16_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint8_t *scr = static_cast<uint8_t *>(
        heap_caps_malloc(sizeof(mcl_ap_modem_scratch_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    Serial.printf("SPIKE NOTE split re-acquire window=%s scratch=%s\n",
                  win ? "ok" : "REFUSED", scr ? "ok" : "REFUSED");
    record((win && scr) ? "arena_split_reacquired" : "arena_split_REFUSED");
    if (win) { heap_caps_free(win); }
    if (scr) { heap_caps_free(scr); }

    /*
     * AND THE ONE THE NODE ACTUALLY DOES: tear the BLE stack down at the end
     * of the bearer phase, then take the arena back for the next round.
     */
    BLEDevice::deinit(true);
    delay(300);
    record("ble_deinit");

    uint8_t *after_deinit = static_cast<uint8_t *>(
        heap_caps_malloc(kUnionArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    Serial.printf("SPIKE NOTE arena re-acquire after BLE deinit: %s\n",
                  (after_deinit != nullptr) ? "ok" : "REFUSED");
    record(after_deinit ? "arena_after_ble_deinit" : "arena_after_ble_deinit_REFUSED");
    if (after_deinit != nullptr) { heap_caps_free(after_deinit); }
#endif

    const Stage &last = g_stages[g_stage_count - 1];
    Serial.printf("SPIKE RESULT ble=UP wifi_coexist=%s final_free=%lu final_largest=%lu verdict=%s\n",
                  ble_with_wifi ? "yes" : "no",
                  static_cast<unsigned long>(last.free_heap),
                  static_cast<unsigned long>(last.largest_block),
                  (last.free_heap > 20000u) ? "FITS" : "TIGHT");
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last > 5000u) {
        last = millis();
        Serial.printf("SPIKE IDLE free=%lu largest=%lu scanned=%lu\n",
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                      static_cast<unsigned long>(g_scan_callbacks.g_seen));
    }
    delay(50);
}

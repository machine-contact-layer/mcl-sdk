/*
 * MCL dual-transport peer for the DFR1154 (ESP32-S3).
 *
 * ONE MCL contact, TWO live radios at the same time, and a real migration
 * between them.
 *
 * Every previous over-air run in this project exercised one binding against a
 * host over one medium. That establishes carriage. It cannot establish
 * migration, because migration is the property that a contact SURVIVES a change
 * of medium, and a peer with one radio has no medium to change to.
 *
 * This sketch brings up a 2.4 GHz SoftAP with a UDP socket AND a BLE GATT
 * server, simultaneously, and holds a single mcl_node_t across both. The
 * transport-aware SDK boundary is what makes that expressible: the tx callback
 * is told which bearer each frame must leave on, and every receive is told
 * which bearer it arrived on. Before that existed this sketch could not have
 * been written correctly -- there was no way for the node to say "this
 * PATH_RESPONSE must go out over IP, not the BLE link it was negotiated on".
 *
 * WHAT THIS MEASURES
 *
 *   - that the four handoff controls cross real radios and are decoded by
 *     the peer's own codec, not by a test harness;
 *   - that a control arriving on the WRONG transport is refused, which is the
 *     one check path validation depends on and cannot be tested in a
 *     single-transport rig at all;
 *   - that the contact continues -- same session_ref, same peer_ref -- after
 *     the medium underneath it changes;
 *   - that a dropped CONFIRM is repaired by retransmission rather than
 *     splitting the peers permanently.
 *
 * WHAT IT DOES NOT MEASURE
 *
 *   - independent interoperability. Both ends compile the same mcl-sdk,
 *     mcl-link, mcl-wire, mcl-ip and mcl-ble sources, so a shared misreading of
 *     the specification passes on both sides and is invisible here. C4 and E6
 *     need a second implementation written by someone else from the
 *     specification, and nothing in this directory substitutes for that.
 *   - security. There is none. The BLE link uses Just Works pairing, the
 *     SoftAP a shared password, and every MCL reference in the exchange crosses
 *     both media in the clear. A listener that heard the first contact can
 *     complete the whole migration sequence and be accepted exactly as an
 *     honest peer would. That is the honest limit of correlation and
 *     reachability without cryptography.
 *
 * The board is the RESPONDER. The host drives the sequence, because somebody
 * has to and the deployment owns that choice (charter 2.10.1).
 *
 * Serial control:
 *   PING     -> MCLPONG
 *   STATUS   -> one line of contact, transport and counter state
 *   RESET    -> return to a fresh contact on BLE
 *   DROPNEXT -> silently discard the next frame this board would transmit,
 *               so the host can test a lost CONFIRM without unplugging a radio
 *
 * Every event produces one MCLDUAL line on serial, which is the evidence
 * record for a run.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

extern "C" {
#include "mcl/sdk.h"
#include "mcl/ble_binding.h"
#include "mcl/ip_binding.h"
}

/* Integrator-scoped identifiers, matching the single-transport BLE rig so a
 * host that can talk to one can talk to this. */
#define MCL_SERVICE_UUID   "6d636c00-0001-4d43-4c00-6d636c626c65"
#define MCL_RX_CHAR_UUID   "6d636c00-0002-4d43-4c00-6d636c626c65"  /* host -> board */
#define MCL_TX_CHAR_UUID   "6d636c00-0003-4d43-4c00-6d636c626c65"  /* board -> host */

static const char *kApSsid     = "MCL-DUAL-TEST";
static const char *kApPassword = "mcl-contact-0";
static const int   kApChannel  = 6;
static const uint16_t kUdpPort = 5555;

/* Contact reference this board puts in the frames it sends. NOT an identity:
 * it crosses both media in the clear and anyone in range can quote it. */
static const uint32_t kBoardSourceRef = 0x0D0A1154u;

/* ---------------------------------------------------------------- state */

static mcl_node_t g_node;

static BLEServer         *g_server  = nullptr;
static BLECharacteristic *g_tx_char = nullptr;
static bool g_ble_connected = false;

static WiFiUDP g_udp;
static IPAddress g_host_ip;
static uint16_t  g_host_port = 0;
static bool      g_host_known = false;

static mcl_ble_reassembler_t g_reasm;

static uint8_t g_rx_buf[1600];
static uint8_t g_scratch[MCL_LINK_FRAME_MAX_SIZE];
static uint8_t g_wire_buf[MCL_WIRE_TIER0_MAX_SIZE];

static uint32_t g_rx_ble = 0, g_rx_ip = 0;
static uint32_t g_tx_ble = 0, g_tx_ip = 0;
static uint32_t g_rejected = 0;
static uint32_t g_wrong_transport = 0;
static uint32_t g_log_dropped = 0;
static uint32_t g_dropped_tx = 0;
static bool     g_drop_next_tx = false;

/*
 * Log only when the USB CDC transmit buffer has room. Serial.printf blocks once
 * that buffer fills, which it does immediately if no host is reading the port,
 * and while it blocks the radios back up. An instrument that perturbs what it
 * measures is worse than no instrument, so a line is dropped and counted
 * instead, and STATUS reports the count so a quiet run is never mistaken for a
 * clean one.
 */
#define MCL_LOG_HEADROOM 192

static bool log_ready(void)
{
    if (Serial.availableForWrite() >= MCL_LOG_HEADROOM) {
        return true;
    }
    g_log_dropped++;
    return false;
}

/*
 * Emit an answer to a serial command, in chunks, with a bounded wait.
 *
 * Command answers cannot use log_ready(): a dropped STATUS is not a lost log
 * line, it is the run's second record going missing. But they must not block
 * either. Serial.printf blocks indefinitely when the USB CDC transmit buffer is
 * full, and this board's loop() also drains the UDP socket, so a blocked write
 * stops servicing a radio.
 *
 * This was not hypothetical. A host harness closed the port while the board was
 * writing, the board's loop stopped there, and the board went on answering ICMP
 * from the network stack while its own console and its MCL loop were dead --
 * alive on the radio, deaf on the protocol, with nothing in the log to say so.
 * It took a hard reset to recover, and it looked exactly like a firmware crash.
 *
 * The deadline resets on progress, so a slow reader is tolerated and an absent
 * one is not.
 */
static bool serial_emit(const char *text)
{
    const char *p = text;
    size_t remaining = strlen(text);
    uint32_t last_progress = millis();

    while (remaining > 0u) {
        int room = Serial.availableForWrite();
        if (room <= 0) {
            if (millis() - last_progress > 500u) {
                g_log_dropped++;
                return false;
            }
            delay(1);
            continue;
        }
        size_t chunk = ((size_t)room < remaining) ? (size_t)room : remaining;
        Serial.write((const uint8_t *)p, chunk);
        p += chunk;
        remaining -= chunk;
        last_progress = millis();
    }
    return true;
}

static const char *transport_name(uint8_t t)
{
    switch (t) {
    case MCL_CONTACT_TRANSPORT_AP:  return "AP";
    case MCL_CONTACT_TRANSPORT_IP:  return "IP";
    case MCL_CONTACT_TRANSPORT_BLE: return "BLE";
    case MCL_CONTACT_TRANSPORT_UWB: return "UWB";
    default:                        return "?";
    }
}

static const char *state_name(uint8_t s)
{
    switch (s) {
    case MCL_CONTACT_STATE_ACTIVE:     return "ACTIVE";
    case MCL_CONTACT_STATE_OFFERED:    return "OFFERED";
    case MCL_CONTACT_STATE_AGREED:     return "AGREED";
    case MCL_CONTACT_STATE_VALIDATING: return "VALIDATING";
    case MCL_CONTACT_STATE_VALIDATED:  return "VALIDATED";
    case MCL_CONTACT_STATE_COMMITTING: return "COMMITTING";
    case MCL_CONTACT_STATE_CLOSED:     return "CLOSED";
    default:                           return "NONE";
    }
}

/* ---------------------------------------------------------------- egress */

static void send_ble_fragmented(const uint8_t *frame, size_t frame_size,
                                int32_t *result)
{
    /*
     * Fragmented at the SMALLEST MTU BLE permits, not at whatever this
     * connection negotiated. Windows negotiates a large one, which would carry
     * every frame in a single PDU and leave the fragmentation path completely
     * untested. The minimum is the case the scheme has to survive.
     */
    const uint16_t mtu = MCL_BLE_ATT_DEFAULT_MTU;
    uint8_t pdu[MCL_BLE_ATT_DEFAULT_MTU];
    size_t count, i, written = 0;

    if (g_tx_char == nullptr || !g_ble_connected) {
        *result = -1;   /* definitely not transmitted: there is no link */
        return;
    }

    count = mcl_ble_fragment_count(frame_size, mtu);
    if (count == 0u) {
        *result = -1;
        return;
    }

    for (i = 0u; i < count; ++i) {
        if (mcl_ble_fragment(frame, frame_size, mtu, i, pdu, sizeof(pdu),
                             &written) != MCL_BLE_OK) {
            /*
             * Partway through. Some fragments are already out, so the peer may
             * hold part of a frame it can never complete -- it will time the
             * reassembly out and discard. Reported as UNCERTAIN rather than
             * "not sent", because bytes did leave.
             */
            *result = (i == 0u) ? -1 : 1;
            return;
        }
        g_tx_char->setValue(pdu, written);
        g_tx_char->notify();
        delay(8);   /* let the stack drain; a burst is dropped, not queued */
    }
    g_tx_ble++;
    *result = 0;
}

static void send_udp(const uint8_t *frame, size_t frame_size, int32_t *result)
{
    if (!g_host_known) {
        *result = -1;   /* nowhere to send: nothing left this machine */
        return;
    }
    g_udp.beginPacket(g_host_ip, g_host_port);
    g_udp.write(frame, frame_size);
    if (g_udp.endPacket() == 1) {
        g_tx_ip++;
        *result = 0;
    } else {
        /*
         * The stack refused the packet. On this API that means it was not
         * handed to the driver, so nothing was transmitted.
         */
        *result = -1;
    }
}

/*
 * The transport-aware transmit callback.
 *
 * Returns 0 for accepted, negative for DEFINITELY not transmitted, positive for
 * an outcome this board cannot vouch for. The three-way answer is what lets the
 * SDK decide whether sending a COMMIT was irrevocable: a BLE notify with no
 * link is a definite refusal, a fragment burst that failed halfway is not.
 */
static int32_t board_tx(void *user, uint8_t transport_id,
                        const uint8_t *data, size_t data_size)
{
    int32_t result = -1;
    (void)user;

    if (g_drop_next_tx) {
        /*
         * DROPNEXT. The frame is reported ACCEPTED and then discarded, which is
         * the case that matters: a transmit failure the sender can see is easy
         * to handle, and a frame that vanishes after a successful send is what
         * actually happens on a radio. This is how the host tests a lost
         * CONFIRM without unplugging anything.
         */
        g_drop_next_tx = false;
        g_dropped_tx++;
        if (log_ready()) {
            Serial.printf("MCLDUAL TX transport=%s n=%u DROPPED_ON_PURPOSE\n",
                          transport_name(transport_id), (unsigned)data_size);
        }
        return 0;
    }

    switch (transport_id) {
    case MCL_CONTACT_TRANSPORT_BLE:
        send_ble_fragmented(data, data_size, &result);
        break;
    case MCL_CONTACT_TRANSPORT_IP:
        send_udp(data, data_size, &result);
        break;
    default:
        /* A transport this board does not have. Definitely not sent. */
        result = -1;
        break;
    }

    if (log_ready()) {
        Serial.printf("MCLDUAL TX transport=%s n=%u r=%ld\n",
                      transport_name(transport_id), (unsigned)data_size,
                      (long)result);
    }
    return result;
}

/* ---------------------------------------------------------------- helpers */

static void fill_presence(mcl_wire_tier0_t *obj)
{
    memset(obj, 0, sizeof(*obj));
    obj->kind = MCL_WIRE_KIND_PRESENCE;
    obj->priority = 2u;
    obj->source_ref = kBoardSourceRef;
    obj->body.presence.machine_class = 3u;
    obj->body.presence.capability_tag = 0x17C0DEu;
    obj->body.presence.ttl = 30u;
}

/*
 * Walk the Link lifecycle on real events rather than jumping to ESTABLISHED at
 * boot.
 *
 * A migration may only be driven from ESTABLISHED or HANDOFF -- the SDK
 * enforces that -- and it would be easy to satisfy by transitioning through the
 * whole lifecycle in setup(). That would be a lie: nothing has been discovered,
 * no capabilities exchanged, nothing negotiated. So the board advances on what
 * actually happens: a peer's first frame is a discovery, and the
 * offer/acceptance exchange IS the negotiation.
 */
static void advance_link(mcl_link_state_t target)
{
    const mcl_link_state_t path[] = {
        MCL_LINK_STATE_DISCOVERED,
        MCL_LINK_STATE_CAPABILITIES,
        MCL_LINK_STATE_NEGOTIATING,
        MCL_LINK_STATE_ESTABLISHED
    };
    size_t i;
    for (i = 0u; i < sizeof(path) / sizeof(path[0]); ++i) {
        if (mcl_node_get_link(&g_node)->state >= target) {
            break;
        }
        if (mcl_node_get_link(&g_node)->state < path[i]) {
            (void)mcl_node_link_transition(&g_node, path[i]);
        }
        if (path[i] == target) {
            break;
        }
    }
}

/*
 * `addressed` sets MCL_LINK_FLAG_DESTINATION. The destination itself is not
 * passed: the SDK fills it from this contact's peer reference, because a node
 * holds one contact and the only machine it can address is that contact's peer.
 * A reply that named no destination would be for whoever heard it, which on a
 * bearer two machines share is not what a reply is.
 */
static void send_semantic(mcl_link_frame_class_t cls,
                          const mcl_wire_tier0_t *obj,
                          uint8_t addressed)
{
    uint8_t flags = MCL_LINK_FLAG_SEQUENCE | MCL_LINK_FLAG_FRAME_CHECK;
    mcl_sdk_status_t st;

    if (addressed != 0u) {
        flags |= MCL_LINK_FLAG_DESTINATION;
    }
    if (mcl_node_get_contact(&g_node)->session_valid != 0u) {
        flags |= MCL_LINK_FLAG_SESSION;
    }

    /*
     * The SDK picks the bearer and refuses while the contact is quiesced --
     * between a COMMIT and its CONFIRM the peer may already have left the
     * transport this board still considers active.
     */
    st = mcl_node_send_framed_tier0(&g_node, obj, cls, flags,
                                    g_scratch, sizeof(g_scratch), NULL);
    if (st != MCL_SDK_OK && log_ready()) {
        Serial.printf("MCLDUAL TXERR semantic cls=%u st=%ld\n",
                      (unsigned)cls, (long)st);
    }
}

static void send_handoff(const mcl_handoff_control_t *control)
{
    mcl_sdk_status_t st = mcl_node_send_handoff(
        &g_node, control,
        MCL_LINK_FLAG_SESSION | MCL_LINK_FLAG_SEQUENCE | MCL_LINK_FLAG_FRAME_CHECK,
        g_scratch, sizeof(g_scratch), NULL);

    if (log_ready()) {
        Serial.printf("MCLDUAL HANDOFF_TX op=%u st=%ld state=%s\n",
                      (unsigned)control->operation, (long)st,
                      state_name(mcl_node_get_contact(&g_node)->state));
    }
}

/* ---------------------------------------------------------------- ingress */

/* A TRANSPORT_OFFER arrived. Record it, agree, and answer with an ACCEPT. */
static void handle_offer(const mcl_wire_tier0_t *obj, uint32_t peer_ref)
{
    mcl_contact_t *c = mcl_node_get_contact(&g_node);
    const uint32_t migration_ref = obj->body.transport_offer.migration_ref;
    const uint8_t  transport     = obj->body.transport_offer.transport_id;
    const uint8_t  profile       = obj->body.transport_offer.profile_id;
    mcl_link_status_t lst;
    uint32_t session_ref;
    mcl_wire_tier0_t reply;

    (void)mcl_contact_set_peer_ref(c, peer_ref);
    advance_link(MCL_LINK_STATE_NEGOTIATING);

    lst = mcl_contact_record_offer(c, migration_ref, transport, profile,
                                   obj->body.transport_offer.endpoint_token,
                                   obj->body.transport_offer.validity);
    if (lst != MCL_LINK_OK) {
        if (log_ready()) {
            Serial.printf("MCLDUAL OFFER mig=%08lX to=%s REFUSED lst=%ld\n",
                          (unsigned long)migration_ref,
                          transport_name(transport), (long)lst);
        }
        return;
    }

    /*
     * The session reference is chosen once and reused for the life of the
     * contact. A second migration MUST name the same one -- it identifies the
     * continuing contact, and an identifier that rotated per hop could not.
     */
    session_ref = (c->session_valid != 0u) ? c->session_ref
                                          : (kBoardSourceRef ^ 0x5E5510C7u);

    lst = mcl_contact_agree(c, migration_ref, transport, profile, session_ref);
    if (lst != MCL_LINK_OK) {
        if (log_ready()) {
            Serial.printf("MCLDUAL AGREE mig=%08lX REFUSED lst=%ld\n",
                          (unsigned long)migration_ref, (long)lst);
        }
        return;
    }
    advance_link(MCL_LINK_STATE_ESTABLISHED);

    memset(&reply, 0, sizeof(reply));
    reply.kind = MCL_WIRE_KIND_TRANSPORT_ACCEPT;
    reply.priority = 2u;
    reply.source_ref = kBoardSourceRef;
    reply.body.transport_accept.migration_ref = migration_ref;
    reply.body.transport_accept.transport_id = transport;
    reply.body.transport_accept.profile_id = profile;
    reply.body.transport_accept.session_ref = session_ref;

    if (log_ready()) {
        Serial.printf("MCLDUAL OFFER mig=%08lX to=%s ACCEPT sess=%08lX\n",
                      (unsigned long)migration_ref, transport_name(transport),
                      (unsigned long)session_ref);
    }
    send_semantic(MCL_LINK_CLASS_CONTACT, &reply, 1u);
}

/* A HANDOFF frame arrived on `arrival`. */
static void handle_handoff(uint8_t arrival, const uint8_t *data, size_t len)
{
    mcl_link_frame_t frame;
    mcl_handoff_control_t control;
    mcl_handoff_action_t action = MCL_HANDOFF_ACTION_NONE;
    size_t consumed = 0;
    mcl_sdk_status_t st;

    st = mcl_node_receive_handoff(&g_node, arrival, data, len, &frame,
                                  &control, &consumed);
    if (st != MCL_SDK_OK) {
        if (st == MCL_SDK_ERR_WRONG_TRANSPORT) {
            /*
             * THE CHECK A SINGLE-TRANSPORT RIG CANNOT TEST.
             *
             * A control that is correct in every reference, delivered over the
             * wrong medium. Accepting it would declare a candidate path
             * reachable on the strength of bytes that never crossed it.
             */
            g_wrong_transport++;
        }
        g_rejected++;
        if (log_ready()) {
            Serial.printf("MCLDUAL HANDOFF_RX on=%s n=%u REJECT st=%ld\n",
                          transport_name(arrival), (unsigned)len, (long)st);
        }
        return;
    }

    st = mcl_node_apply_handoff(&g_node, &control, &action);
    if (log_ready()) {
        const mcl_contact_t *c = mcl_node_get_contact_const(&g_node);
        Serial.printf("MCLDUAL HANDOFF_RX on=%s op=%u mig=%08lX sess=%08lX "
                      "apply=%ld state=%s active=%s act=%u\n",
                      transport_name(arrival), (unsigned)control.operation,
                      (unsigned long)control.migration_ref,
                      (unsigned long)control.session_ref, (long)st,
                      state_name(c->state),
                      transport_name(c->active_transport), (unsigned)action);
    }
    if (st != MCL_SDK_OK) {
        g_rejected++;
        return;
    }

    if (action == MCL_HANDOFF_ACTION_SEND_PATH_RESPONSE) {
        mcl_handoff_control_t reply;
        if (mcl_handoff_make_path_response(&reply, control.migration_ref,
                                           control.session_ref,
                                           control.challenge) == MCL_LINK_OK) {
            send_handoff(&reply);
        }
    } else if (action == MCL_HANDOFF_ACTION_SEND_CONFIRM) {
        mcl_handoff_control_t reply;
        if (mcl_handoff_make_confirm(&reply, control.migration_ref,
                                     control.session_ref) == MCL_LINK_OK) {
            send_handoff(&reply);
        }
    }
}

/* Any complete Link frame, from either radio. */
static void handle_frame(uint8_t arrival, const uint8_t *data, size_t len)
{
    mcl_link_frame_t frame;
    mcl_wire_tier0_t obj;
    uint8_t has_object = 0u;
    size_t consumed = 0;
    mcl_sdk_status_t st;

    if (arrival == MCL_CONTACT_TRANSPORT_BLE) { g_rx_ble++; } else { g_rx_ip++; }

    /*
     * Peek the class before choosing a decoder. A handoff control and a
     * semantic object are different payload contracts, and the class is what
     * says which registry the payload's first bytes belong to.
     */
    if (len >= 1u && (data[0] & 0x0Fu) == (uint8_t)MCL_LINK_CLASS_HANDOFF) {
        handle_handoff(arrival, data, len);
        return;
    }

    st = mcl_node_receive_framed(&g_node, arrival, data, len, &frame, &obj,
                                 &has_object, &consumed);
    if (st == MCL_SDK_NOT_ADDRESSED) {
        if (log_ready()) {
            Serial.printf("MCLDUAL RX on=%s n=%u NOT_ADDRESSED dst=%08lX\n",
                          transport_name(arrival), (unsigned)len,
                          (unsigned long)frame.destination_ref);
        }
        return;
    }
    if (st != MCL_SDK_OK) {
        g_rejected++;
        if (st == MCL_SDK_ERR_WRONG_TRANSPORT) { g_wrong_transport++; }
        if (log_ready()) {
            Serial.printf("MCLDUAL RX on=%s n=%u REJECT st=%ld\n",
                          transport_name(arrival), (unsigned)len, (long)st);
        }
        return;
    }

    advance_link(MCL_LINK_STATE_DISCOVERED);

    /*
     * Learn the peer's contact reference from the first frame it is heard in.
     * Addressing a reply requires it: MCL_LINK_FLAG_DESTINATION is filled from
     * the contact's peer_ref, and a node that has not learned one is refused
     * rather than allowed to address a frame to nobody. First-write-wins, so a
     * later frame claiming a different reference is refused by the library and
     * cannot redirect this contact.
     */
    (void)mcl_contact_set_peer_ref(mcl_node_get_contact(&g_node),
                                   frame.source_ref);

    if (has_object == 0u) {
        if (log_ready()) {
            Serial.printf("MCLDUAL RX on=%s cls=%u src=%08lX seq=%u nosem\n",
                          transport_name(arrival), (unsigned)frame.frame_class,
                          (unsigned long)frame.source_ref,
                          (unsigned)frame.sequence);
        }
        return;
    }

    if (log_ready()) {
        Serial.printf("MCLDUAL RX on=%s cls=%u src=%08lX seq=%u kind=%u\n",
                      transport_name(arrival), (unsigned)frame.frame_class,
                      (unsigned long)frame.source_ref,
                      (unsigned)frame.sequence, (unsigned)obj.kind);
    }

    if (obj.kind == MCL_WIRE_KIND_TRANSPORT_OFFER) {
        handle_offer(&obj, frame.source_ref);
        return;
    }

    /* Anything else is answered with this board's presence, as the
     * single-transport rigs do. Receiving is not acting. */
    {
        mcl_wire_tier0_t reply;
        fill_presence(&reply);
        send_semantic(MCL_LINK_CLASS_DATA, &reply, 1u);
    }
}

/* ---------------------------------------------------------------- BLE glue */

class ServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        g_ble_connected = true;
        mcl_ble_reassembler_reset(&g_reasm);
        if (log_ready()) { Serial.println("MCLDUAL BLE CONNECTED"); }
    }
    void onDisconnect(BLEServer *s) override {
        g_ble_connected = false;
        if (log_ready()) { Serial.println("MCLDUAL BLE DISCONNECTED"); }
        s->startAdvertising();
    }
};

class RxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        String v = c->getValue();
        const uint8_t *data = (const uint8_t *)v.c_str();
        size_t len = v.length();
        size_t frame_size = 0;
        mcl_ble_status_t st;

        if (len == 0) { return; }

        st = mcl_ble_reassemble(&g_reasm, data, len, &frame_size);
        if (st == MCL_BLE_OK) {
            handle_frame(MCL_CONTACT_TRANSPORT_BLE, g_reasm.buffer, frame_size);
        } else if (st == MCL_BLE_ERR_INCOMPLETE) {
            /* Normal: more fragments are expected. */
        } else {
            g_rejected++;
            if (log_ready()) {
                Serial.printf("MCLDUAL FRAG n=%u REJECT reasm=%ld\n",
                              (unsigned)len, (long)st);
            }
        }
    }
};

/* ---------------------------------------------------------------- serial */

static void start_contact(void)
{
    mcl_node_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = board_tx;
    cfg.user_ctx = NULL;
    cfg.source_ref = kBoardSourceRef;
    cfg.transport_id = MCL_CONTACT_TRANSPORT_BLE;   /* contact begins on BLE */
    cfg.role = MCL_CONTACT_ROLE_RESPONDER;
    (void)mcl_node_init(&g_node, &cfg);
}

static void handle_serial_line(const String &line)
{
    /* Command answers go through serial_emit, which waits with a deadline
     * rather than blocking the loop that also drains the UDP socket. */
    char buf[512];

    if (line == "PING") {
        (void)serial_emit("MCLPONG\n");
    } else if (line == "STATUS") {
        const mcl_contact_t *c = mcl_node_get_contact_const(&g_node);
        uint8_t ctrl_transport = 0u, data_transport = 0u, quiesced = 0u;
        (void)mcl_contact_control_transport(c, &ctrl_transport);
        (void)mcl_contact_data_transport(c, &data_transport, &quiesced);
        (void)snprintf(buf, sizeof(buf),
                       "MCLDUAL STATUS state=%s active=%s ctrl=%s quiesced=%u "
                       "sess=%08lX mig=%08lX done=%08lX hops=%u link=%u "
                       "ble=%u ip=%s:%u rx_ble=%lu rx_ip=%lu tx_ble=%lu "
                       "tx_ip=%lu rej=%lu wrongxp=%lu drop=%lu logdrop=%lu\n",
                       state_name(c->state),
                       transport_name(c->active_transport),
                       transport_name(ctrl_transport), (unsigned)quiesced,
                       (unsigned long)c->session_ref,
                       (unsigned long)c->pending_migration_ref,
                       (unsigned long)c->completed_migration_ref,
                       (unsigned)c->migration_count,
                       (unsigned)mcl_node_get_link(&g_node)->state,
                       (unsigned)g_ble_connected,
                       g_host_known ? g_host_ip.toString().c_str() : "none",
                       (unsigned)g_host_port,
                       (unsigned long)g_rx_ble, (unsigned long)g_rx_ip,
                       (unsigned long)g_tx_ble, (unsigned long)g_tx_ip,
                       (unsigned long)g_rejected,
                       (unsigned long)g_wrong_transport,
                       (unsigned long)g_dropped_tx,
                       (unsigned long)g_log_dropped);
        (void)serial_emit(buf);
    } else if (line == "RESET") {
        start_contact();
        g_rx_ble = g_rx_ip = g_tx_ble = g_tx_ip = 0;
        g_rejected = g_wrong_transport = g_log_dropped = g_dropped_tx = 0;
        g_drop_next_tx = false;
        mcl_ble_reassembler_reset(&g_reasm);
        (void)serial_emit("MCLDUAL RESET OK\n");
    } else if (line == "DROPNEXT") {
        g_drop_next_tx = true;
        (void)serial_emit("MCLDUAL DROPNEXT ARMED\n");
    }
}

/* ---------------------------------------------------------------- setup */

void setup()
{
    Serial.begin(115200);
    delay(300);

    start_contact();
    mcl_ble_reassembler_reset(&g_reasm);

    /*
     * BOTH radios, at once. WIFI_AP and BLE share the 2.4 GHz front end on this
     * part and coexist by time-slicing; that is a real constraint of the
     * hardware and it is part of what this experiment measures.
     */
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPassword, kApChannel);
    delay(200);
    g_udp.begin(kUdpPort);

    BLEDevice::init("MCL-DUAL-TEST");
    g_server = BLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());

    BLEService *service = g_server->createService(MCL_SERVICE_UUID);

    BLECharacteristic *rx_char = service->createCharacteristic(
        MCL_RX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR);
    rx_char->setCallbacks(new RxCallbacks());

    g_tx_char = service->createCharacteristic(
        MCL_TX_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    g_tx_char->addDescriptor(new BLE2902());

    service->start();

    {
        BLEAdvertising *advertising = BLEDevice::getAdvertising();
        advertising->addServiceUUID(MCL_SERVICE_UUID);
        advertising->setScanResponse(true);
        BLEDevice::startAdvertising();
    }

    /*
     * Through serial_emit like every other answer. A board that boots with no
     * host attached to the port must not spend its first half second blocked in
     * a banner, and after the wedge described above no unbounded Serial write
     * is left anywhere in this sketch.
     */
    {
        char banner[256];
        (void)snprintf(banner, sizeof(banner),
                       "MCLDUAL READY ssid=%s ip=%s port=%u ch=%d ble=%s "
                       "src=%08lX mtu=%u frame_max=%u\n",
                       kApSsid, WiFi.softAPIP().toString().c_str(), kUdpPort,
                       kApChannel, MCL_SERVICE_UUID,
                       (unsigned long)kBoardSourceRef,
                       (unsigned)MCL_BLE_ATT_DEFAULT_MTU,
                       (unsigned)MCL_LINK_FRAME_MAX_SIZE);
        (void)serial_emit(banner);
    }
}

void loop()
{
    int len;

    /*
     * Drain every queued datagram before touching anything else. Handling one
     * per iteration lets a burst back up behind the rest of the loop, and that
     * overflow would look like packet loss on the radio.
     */
    while ((len = g_udp.parsePacket()) > 0) {
        IPAddress from = g_udp.remoteIP();
        uint16_t port = g_udp.remotePort();
        int n = g_udp.read(g_rx_buf, sizeof(g_rx_buf));

        /*
         * Learn the host's UDP endpoint from the first datagram. The board
         * cannot know it in advance, and the endpoint_token in the offer is a
         * rendezvous reference rather than an address: resolving it is the
         * candidate transport's own business, and on UDP the resolution is
         * "reply where the probe came from".
         */
        g_host_ip = from;
        g_host_port = port;
        g_host_known = true;

        if (n > 0) {
            handle_frame(MCL_CONTACT_TRANSPORT_IP, g_rx_buf, (size_t)n);
        }
    }

    while (Serial.available() > 0) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            handle_serial_line(line);
        }
    }
}

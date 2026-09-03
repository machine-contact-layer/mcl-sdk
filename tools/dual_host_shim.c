/*
 * Flat C entry points over one mcl_node_t, for a host harness that holds two
 * live radios at once.
 *
 * WHY THIS EXISTS
 *
 * The dual-transport experiment needs a host peer that is on Bluetooth LE and
 * on Wi-Fi simultaneously and migrates one contact between them. Windows
 * exposes BLE only through WinRT, so that harness has to be written in a
 * language with WinRT projections. Writing the protocol there would mean the
 * migration being measured was the harness's idea of migration rather than the
 * shipped one -- and migration is precisely the property this project has never
 * been able to test over real radios.
 *
 * So the harness supplies sockets, GATT and timing, and nothing else. Every
 * frame is built, decoded, validated and applied here, by mcl-sdk, mcl-link,
 * mcl-wire, mcl-ble and mcl-ip. This file adds no protocol logic: it holds no
 * state of its own, makes no decisions, and each function forwards to the
 * library and translates the result into something a marshaller can carry.
 *
 * THE TRANSMIT CALLBACK IS THE WHOLE POINT
 *
 * mcl_sdk_tx_fn is told which bearer each frame must leave on, and the SDK
 * derives that from the contact state. The harness registers one callback that
 * dispatches on transport_id to a GATT characteristic or a UDP socket. That is
 * how "this PATH_CHALLENGE must go out over IP, not over the BLE link it was
 * negotiated on" becomes something the host obeys rather than something the
 * host has to remember.
 *
 * STATUS VALUES ARE RETURNED UNCHANGED
 *
 * A function returning mcl_sdk_status_t returns exactly that value, and one
 * returning mcl_link_status_t returns exactly that. A harness asserting that a
 * peer refused for the reason the specification requires needs the reason, not
 * a generic failure. Shim-level argument faults return MCLX_ERR_ARGUMENT, which
 * is deliberately outside both ranges.
 *
 * This is host tooling, not runtime code. It is compiled into a shared library
 * and is not part of the freestanding stack, so it may use a wider ABI than the
 * protocol libraries do. It still keeps all node state caller-owned, because a
 * hidden global would make a two-node test in one process quietly wrong.
 */

#include "mcl/sdk.h"
#include "mcl/ble_binding.h"
#include "mcl/ip_binding.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#  define MCLX_API __declspec(dllexport)
#else
#  define MCLX_API __attribute__((visibility("default")))
#endif

/* Outside every library's status range, so a shim fault is never mistaken for
 * a protocol refusal. */
#define MCLX_ERR_ARGUMENT (-1000)

/* ---------------------------------------------------------------- sizes */

MCLX_API int32_t mclx_node_size(void)          { return (int32_t)sizeof(mcl_node_t); }
MCLX_API int32_t mclx_reassembler_size(void)   { return (int32_t)sizeof(mcl_ble_reassembler_t); }
MCLX_API int32_t mclx_frame_max_size(void)     { return (int32_t)MCL_LINK_FRAME_MAX_SIZE; }
MCLX_API int32_t mclx_ble_default_mtu(void)    { return (int32_t)MCL_BLE_ATT_DEFAULT_MTU; }
MCLX_API int32_t mclx_challenge_size(void)     { return (int32_t)MCL_CONTACT_CHALLENGE_SIZE; }

/*
 * Transport identifiers, read from the registry headers rather than restated in
 * C#. These drifted between a binding header and the registry once already; a
 * harness holding a third copy is a third place for it to happen.
 */
MCLX_API int32_t mclx_transport_ip(void)  { return (int32_t)MCL_CONTACT_TRANSPORT_IP; }
MCLX_API int32_t mclx_transport_ble(void) { return (int32_t)MCL_CONTACT_TRANSPORT_BLE; }
MCLX_API int32_t mclx_transport_ap(void)  { return (int32_t)MCL_CONTACT_TRANSPORT_AP; }
MCLX_API int32_t mclx_transport_uwb(void) { return (int32_t)MCL_CONTACT_TRANSPORT_UWB; }

/* ---------------------------------------------------------------- BLE carriage */

MCLX_API int32_t mclx_ble_fragment_count(int32_t frame_size, int32_t att_mtu)
{
    if (frame_size < 0 || att_mtu < 0 || att_mtu > 0xFFFF) {
        return 0;
    }
    return (int32_t)mcl_ble_fragment_count((size_t)frame_size, (uint16_t)att_mtu);
}

MCLX_API int32_t mclx_ble_fragment(const uint8_t *frame, int32_t frame_size,
                                   int32_t att_mtu, int32_t index,
                                   uint8_t *out, int32_t out_capacity)
{
    size_t written = 0u;
    mcl_ble_status_t st;

    if (frame == NULL || out == NULL || frame_size < 0 || index < 0 ||
        att_mtu < 0 || att_mtu > 0xFFFF || out_capacity < 0) {
        return MCLX_ERR_ARGUMENT;
    }

    st = mcl_ble_fragment(frame, (size_t)frame_size, (uint16_t)att_mtu,
                          (size_t)index, out, (size_t)out_capacity, &written);
    if (st != MCL_BLE_OK) {
        return -(int32_t)st;
    }
    return (int32_t)written;
}

MCLX_API void mclx_ble_reassembler_reset(void *state)
{
    mcl_ble_reassembler_reset((mcl_ble_reassembler_t *)state);
}

/*
 * Feed one fragment. Returns the frame length when a frame completes, 0 when
 * more fragments are expected -- the normal path, deliberately not an error --
 * or a negative status on a refusal.
 */
MCLX_API int32_t mclx_ble_reassemble(void *state,
                                     const uint8_t *fragment, int32_t fragment_size,
                                     uint8_t *out, int32_t out_capacity)
{
    mcl_ble_reassembler_t *r = (mcl_ble_reassembler_t *)state;
    size_t frame_size = 0u;
    mcl_ble_status_t st;

    if (r == NULL || fragment == NULL || out == NULL ||
        fragment_size < 0 || out_capacity < 0) {
        return MCLX_ERR_ARGUMENT;
    }

    st = mcl_ble_reassemble(r, fragment, (size_t)fragment_size, &frame_size);
    if (st == MCL_BLE_ERR_INCOMPLETE) {
        return 0;
    }
    if (st != MCL_BLE_OK) {
        return -(int32_t)st;
    }
    if (frame_size > (size_t)out_capacity) {
        return -(int32_t)MCL_BLE_ERR_RANGE;
    }
    memcpy(out, r->buffer, frame_size);
    return (int32_t)frame_size;
}

/* ---------------------------------------------------------------- IP carriage */

/*
 * A datagram carries exactly one Link frame. The check belongs to the binding,
 * so the harness calls it rather than assuming that whatever recvfrom returned
 * is a frame.
 */
MCLX_API int32_t mclx_ip_datagram_validate(const uint8_t *datagram, int32_t size)
{
    if (datagram == NULL || size < 0) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_ip_datagram_validate(datagram, (size_t)size);
}

/* ---------------------------------------------------------------- node */

MCLX_API int32_t mclx_node_init(void *node, uint32_t source_ref,
                                int32_t transport_id, int32_t role,
                                mcl_sdk_tx_fn tx_fn, void *user)
{
    mcl_node_config_t cfg;

    if (node == NULL || transport_id < 0 || transport_id > 0xFF ||
        role < 0 || role > 0xFF) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.supported_wire_majors_mask = mcl_link_wire_major_mask(0u);
    cfg.tx_fn = tx_fn;
    cfg.user_ctx = user;
    cfg.source_ref = source_ref;
    cfg.transport_id = (uint8_t)transport_id;
    cfg.role = (mcl_contact_role_t)role;

    return (int32_t)mcl_node_init((mcl_node_t *)node, &cfg);
}

MCLX_API int32_t mclx_link_transition(void *node, int32_t state)
{
    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_node_link_transition((mcl_node_t *)node,
                                             (mcl_link_state_t)state);
}

MCLX_API int32_t mclx_link_state(const void *node)
{
    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_node_get_link_const((const mcl_node_t *)node)->state;
}

/* ---------------------------------------------------------------- send */

/*
 * Send a PRESENCE object in a Link frame. The values are fixed here rather than
 * passed in: this harness uses presence as ordinary traffic to prove a contact
 * still carries data after a migration, and what is in it does not matter.
 */
MCLX_API int32_t mclx_send_presence(void *node, int32_t frame_class,
                                    int32_t flags, int32_t addressed)
{
    uint8_t scratch[MCL_LINK_FRAME_MAX_SIZE];
    mcl_wire_tier0_t obj;

    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 2u;
    obj.source_ref = ((const mcl_node_t *)node)->source_ref;
    obj.body.presence.machine_class = 7u;
    obj.body.presence.capability_tag = 0x112233u;
    obj.body.presence.ttl = 60u;

    if (addressed != 0) {
        flags |= (int32_t)MCL_LINK_FLAG_DESTINATION;
    }

    return (int32_t)mcl_node_send_framed_tier0(
        (mcl_node_t *)node, &obj, (mcl_link_frame_class_t)frame_class,
        (uint8_t)flags, scratch, sizeof(scratch), NULL);
}

/*
 * Send a TRANSPORT_OFFER. Every field is the caller's, because the failure
 * matrix needs offers that are wrong in one field at a time -- a stale
 * migration_ref, an unsupported profile, a transport nobody has.
 *
 * `addressed` sets MCL_LINK_FLAG_DESTINATION, which the SDK fills with this
 * contact's peer reference, so a machine on a shared bearer can tell an offer
 * meant for it from one it merely overheard. The destination is not a parameter
 * because a node holds one contact and can address only that contact's peer.
 */
MCLX_API int32_t mclx_send_transport_offer(void *node, int32_t flags,
                                           int32_t addressed,
                                           uint32_t migration_ref,
                                           int32_t transport_id,
                                           int32_t profile_id,
                                           uint32_t endpoint_token,
                                           int32_t validity)
{
    uint8_t scratch[MCL_LINK_FRAME_MAX_SIZE];
    mcl_wire_tier0_t obj;

    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_TRANSPORT_OFFER;
    obj.priority = 2u;
    obj.source_ref = ((const mcl_node_t *)node)->source_ref;
    obj.body.transport_offer.migration_ref = migration_ref;
    obj.body.transport_offer.transport_id = (uint8_t)transport_id;
    obj.body.transport_offer.profile_id = (uint8_t)profile_id;
    obj.body.transport_offer.endpoint_token = endpoint_token;
    obj.body.transport_offer.validity = (uint8_t)validity;

    if (addressed != 0) {
        flags |= (int32_t)MCL_LINK_FLAG_DESTINATION;
    }

    return (int32_t)mcl_node_send_framed_tier0(
        (mcl_node_t *)node, &obj, MCL_LINK_CLASS_CONTACT, (uint8_t)flags,
        scratch, sizeof(scratch), NULL);
}

/* ---------------------------------------------------------------- receive */

/*
 * Everything a harness needs from one received frame, in one struct.
 *
 * The transport-change fields are filled only for the two object kinds that
 * carry them, and left zero otherwise. `kind` is -1 when the frame carried no
 * semantic object at all, which is the ordinary case for ACK and KEEPALIVE.
 */
typedef struct {
    int32_t  frame_class;
    uint32_t source_ref;
    uint32_t destination_ref;
    int32_t  sequence;
    int32_t  kind;
    uint32_t migration_ref;
    int32_t  transport_id;
    int32_t  profile_id;
    uint32_t endpoint_token;
    int32_t  validity;
    uint32_t session_ref;
} mclx_rx_t;

MCLX_API int32_t mclx_receive_framed(void *node, int32_t arrival_transport,
                                     const uint8_t *data, int32_t size,
                                     mclx_rx_t *out)
{
    mcl_link_frame_t frame;
    mcl_wire_tier0_t obj;
    uint8_t has_object = 0u;
    size_t consumed = 0u;
    mcl_sdk_status_t st;

    if (node == NULL || data == NULL || size < 0 || out == NULL ||
        arrival_transport < 0 || arrival_transport > 0xFF) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    out->kind = -1;

    st = mcl_node_receive_framed((mcl_node_t *)node, (uint8_t)arrival_transport,
                                 data, (size_t)size, &frame, &obj, &has_object,
                                 &consumed);

    /*
     * NOT_ADDRESSED still decodes the frame, and a harness needs to see whose
     * it was. Filling the frame fields for that case is what lets the multi-peer
     * isolation check assert which node the frame named rather than only that
     * this node declined it.
     */
    if (st == MCL_SDK_OK || st == MCL_SDK_NOT_ADDRESSED) {
        out->frame_class = (int32_t)frame.frame_class;
        out->source_ref = frame.source_ref;
        out->destination_ref = frame.destination_ref;
        out->sequence = (int32_t)frame.sequence;
    }
    if (st != MCL_SDK_OK || has_object == 0u) {
        return (int32_t)st;
    }

    out->kind = (int32_t)obj.kind;
    if (obj.kind == MCL_WIRE_KIND_TRANSPORT_OFFER) {
        out->migration_ref = obj.body.transport_offer.migration_ref;
        out->transport_id = (int32_t)obj.body.transport_offer.transport_id;
        out->profile_id = (int32_t)obj.body.transport_offer.profile_id;
        out->endpoint_token = obj.body.transport_offer.endpoint_token;
        out->validity = (int32_t)obj.body.transport_offer.validity;
    } else if (obj.kind == MCL_WIRE_KIND_TRANSPORT_ACCEPT) {
        out->migration_ref = obj.body.transport_accept.migration_ref;
        out->transport_id = (int32_t)obj.body.transport_accept.transport_id;
        out->profile_id = (int32_t)obj.body.transport_accept.profile_id;
        out->session_ref = obj.body.transport_accept.session_ref;
    }
    return (int32_t)st;
}

/* ---------------------------------------------------------------- handoff */

/*
 * Send one handoff control.
 *
 * `challenge` is read only for PATH_CHALLENGE and PATH_RESPONSE and may be NULL
 * otherwise. The SDK chooses the bearer from the contact; the caller cannot
 * override it, which is the property that makes a validated path mean
 * something.
 */
MCLX_API int32_t mclx_send_handoff(void *node, int32_t operation,
                                   uint32_t migration_ref, uint32_t session_ref,
                                   const uint8_t *challenge, int32_t flags)
{
    uint8_t scratch[MCL_LINK_FRAME_MAX_SIZE];
    mcl_handoff_control_t control;

    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&control, 0, sizeof(control));
    control.operation = (mcl_handoff_op_t)operation;
    control.migration_ref = migration_ref;
    control.session_ref = session_ref;
    if (challenge != NULL) {
        memcpy(control.challenge, challenge, MCL_CONTACT_CHALLENGE_SIZE);
        control.challenge_present = 1u;
    }

    return (int32_t)mcl_node_send_handoff((mcl_node_t *)node, &control,
                                          (uint8_t)flags, scratch,
                                          sizeof(scratch), NULL);
}

/*
 * Send a handoff control with an arbitrary encoded payload, bypassing the
 * control encoder.
 *
 * This exists for one case in the failure matrix: a HANDOFF frame whose control
 * payload is malformed. A peer's refusal of a payload its own encoder produced
 * proves nothing, so the harness has to be able to emit bytes the encoder would
 * never write. It is the only function here that does not go through the
 * library, and it is deliberately not usable for anything else -- it builds the
 * frame with mcl_link_frame_encode and lets the payload be whatever the caller
 * gave.
 */
MCLX_API int32_t mclx_send_handoff_raw(void *node, uint32_t session_ref,
                                       const uint8_t *payload, int32_t payload_size,
                                       int32_t transport_id)
{
    uint8_t scratch[MCL_LINK_FRAME_MAX_SIZE];
    mcl_node_t *n = (mcl_node_t *)node;
    mcl_link_frame_t frame;
    size_t written = 0u;
    mcl_link_status_t lst;
    int32_t tx_res;

    if (node == NULL || payload == NULL || payload_size < 0 ||
        transport_id < 0 || transport_id > 0xFF) {
        return MCLX_ERR_ARGUMENT;
    }
    if (n->tx_fn == NULL) {
        return (int32_t)MCL_SDK_ERR_TX_UNAVAILABLE;
    }

    memset(&frame, 0, sizeof(frame));
    frame.frame_class = MCL_LINK_CLASS_HANDOFF;
    frame.flags = MCL_LINK_FLAG_SESSION | MCL_LINK_FLAG_FRAME_CHECK;
    frame.source_ref = n->source_ref;
    frame.session_ref = session_ref;
    frame.payload = payload;
    frame.payload_len = (uint16_t)payload_size;

    lst = mcl_link_frame_encode(&frame, scratch, sizeof(scratch), &written);
    if (lst != MCL_LINK_OK) {
        return (int32_t)MCL_SDK_ERR_FRAME_FAILURE;
    }

    tx_res = n->tx_fn(n->user_ctx, (uint8_t)transport_id, scratch, written);
    return (tx_res < 0) ? (int32_t)MCL_SDK_ERR_TX_NOT_SENT : (int32_t)MCL_SDK_OK;
}

/*
 * Encode a handoff control into bytes without sending it.
 *
 * Two cases in the failure matrix need this, and neither can be reached through
 * mclx_send_handoff, because that function is correct:
 *
 *   - a VALID control delivered on the WRONG transport. This is the check a
 *     single-transport rig cannot perform at all, and it is the reason this
 *     experiment exists. mclx_send_handoff always picks the bearer from the
 *     contact, so the harness has to build the control and hand the bytes to a
 *     radio of its choosing.
 *   - a control the encoder would never produce: a truncated payload, an
 *     unassigned operation, a challenge on an operation that carries none.
 *
 * The encoding itself is still the library's. Only the delivery is the
 * harness's.
 */
MCLX_API int32_t mclx_handoff_control_encode(int32_t operation,
                                             uint32_t migration_ref,
                                             uint32_t session_ref,
                                             const uint8_t *challenge,
                                             uint8_t *out, int32_t out_capacity)
{
    mcl_handoff_control_t control;
    size_t written = 0u;
    mcl_link_status_t lst;

    if (out == NULL || out_capacity < 0) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&control, 0, sizeof(control));
    control.operation = (mcl_handoff_op_t)operation;
    control.migration_ref = migration_ref;
    control.session_ref = session_ref;
    if (challenge != NULL) {
        memcpy(control.challenge, challenge, MCL_CONTACT_CHALLENGE_SIZE);
        control.challenge_present = 1u;
    }

    lst = mcl_handoff_control_encode(&control, out, (size_t)out_capacity,
                                     &written);
    if (lst != MCL_LINK_OK) {
        return -(int32_t)lst;
    }
    return (int32_t)written;
}

MCLX_API int32_t mclx_receive_handoff(void *node, int32_t arrival_transport,
                                      const uint8_t *data, int32_t size,
                                      int32_t *out_operation,
                                      uint32_t *out_migration_ref,
                                      uint32_t *out_session_ref,
                                      uint8_t *out_challenge,
                                      int32_t *out_challenge_valid)
{
    mcl_link_frame_t frame;
    mcl_handoff_control_t control;
    size_t consumed = 0u;
    mcl_sdk_status_t st;

    if (node == NULL || data == NULL || size < 0 || out_operation == NULL ||
        out_migration_ref == NULL || out_session_ref == NULL ||
        out_challenge == NULL || out_challenge_valid == NULL ||
        arrival_transport < 0 || arrival_transport > 0xFF) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&control, 0, sizeof(control));
    st = mcl_node_receive_handoff((mcl_node_t *)node, (uint8_t)arrival_transport,
                                  data, (size_t)size, &frame, &control,
                                  &consumed);

    *out_operation = (int32_t)control.operation;
    *out_migration_ref = control.migration_ref;
    *out_session_ref = control.session_ref;
    *out_challenge_valid = (int32_t)control.challenge_present;
    memcpy(out_challenge, control.challenge, MCL_CONTACT_CHALLENGE_SIZE);

    return (int32_t)st;
}

MCLX_API int32_t mclx_apply_handoff(void *node, int32_t operation,
                                    uint32_t migration_ref, uint32_t session_ref,
                                    const uint8_t *challenge,
                                    int32_t challenge_valid,
                                    int32_t *out_action)
{
    mcl_handoff_control_t control;
    mcl_handoff_action_t action = MCL_HANDOFF_ACTION_NONE;
    mcl_sdk_status_t st;

    if (node == NULL || out_action == NULL) {
        return MCLX_ERR_ARGUMENT;
    }

    memset(&control, 0, sizeof(control));
    control.operation = (mcl_handoff_op_t)operation;
    control.migration_ref = migration_ref;
    control.session_ref = session_ref;
    if (challenge != NULL && challenge_valid != 0) {
        memcpy(control.challenge, challenge, MCL_CONTACT_CHALLENGE_SIZE);
        control.challenge_present = 1u;
    }

    st = mcl_node_apply_handoff((mcl_node_t *)node, &control, &action);
    *out_action = (int32_t)action;
    return (int32_t)st;
}

/* ---------------------------------------------------------------- contact */

/*
 * The whole contact, in the terms the specification uses. A harness that
 * derived any of this for itself would be asserting its own bookkeeping against
 * the board rather than the library's.
 */
typedef struct {
    int32_t  state;
    int32_t  role;
    int32_t  active_transport;
    int32_t  control_transport;
    int32_t  data_transport;
    int32_t  quiesced;
    uint32_t local_ref;
    uint32_t peer_ref;
    int32_t  peer_ref_valid;
    uint32_t session_ref;
    int32_t  session_valid;
    uint32_t pending_migration_ref;
    int32_t  pending_transport;
    int32_t  pending_profile;
    uint32_t pending_endpoint_token;
    uint32_t completed_migration_ref;
    int32_t  migration_count;
    int32_t  link_state;
} mclx_contact_snapshot_t;

MCLX_API int32_t mclx_contact_snapshot(const void *node,
                                       mclx_contact_snapshot_t *out)
{
    const mcl_node_t *n = (const mcl_node_t *)node;
    const mcl_contact_t *c;
    uint8_t control_transport = 0u, data_transport = 0u, quiesced = 0u;

    if (node == NULL || out == NULL) {
        return MCLX_ERR_ARGUMENT;
    }

    c = mcl_node_get_contact_const(n);
    memset(out, 0, sizeof(*out));

    out->state = (int32_t)c->state;
    out->role = (int32_t)c->role;
    out->active_transport = (int32_t)c->active_transport;
    out->local_ref = c->local_ref;
    out->peer_ref = c->peer_ref;
    out->peer_ref_valid = (int32_t)c->peer_ref_valid;
    out->session_ref = c->session_ref;
    out->session_valid = (int32_t)c->session_valid;
    out->pending_migration_ref = c->pending_migration_ref;
    out->pending_transport = (int32_t)c->pending_transport;
    out->pending_profile = (int32_t)c->pending_profile;
    out->pending_endpoint_token = c->pending_endpoint_token;
    out->completed_migration_ref = c->completed_migration_ref;
    out->migration_count = (int32_t)c->migration_count;
    out->link_state = (int32_t)mcl_node_get_link_const(n)->state;

    /*
     * Both derived transports come from the library. A refusal is reported as
     * transport 0, which is the reserved identifier: there are contact states
     * with no admissible bearer, and inventing one here would hide them.
     */
    if (mcl_contact_control_transport(c, &control_transport) == MCL_LINK_OK) {
        out->control_transport = (int32_t)control_transport;
    }
    if (mcl_contact_data_transport(c, &data_transport, &quiesced) == MCL_LINK_OK) {
        out->data_transport = (int32_t)data_transport;
        out->quiesced = (int32_t)quiesced;
    }
    return 0;
}

MCLX_API int32_t mclx_contact_set_peer_ref(void *node, uint32_t peer_ref)
{
    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_contact_set_peer_ref(
        mcl_node_get_contact((mcl_node_t *)node), peer_ref);
}

MCLX_API int32_t mclx_contact_record_offer(void *node, uint32_t migration_ref,
                                           int32_t transport_id, int32_t profile_id,
                                           uint32_t endpoint_token, int32_t validity)
{
    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_contact_record_offer(
        mcl_node_get_contact((mcl_node_t *)node), migration_ref,
        (uint8_t)transport_id, (uint8_t)profile_id, endpoint_token,
        (uint8_t)validity);
}

MCLX_API int32_t mclx_contact_agree(void *node, uint32_t migration_ref,
                                    int32_t transport_id, int32_t profile_id,
                                    uint32_t session_ref)
{
    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_contact_agree(
        mcl_node_get_contact((mcl_node_t *)node), migration_ref,
        (uint8_t)transport_id, (uint8_t)profile_id, session_ref);
}

MCLX_API int32_t mclx_contact_validation_begin(void *node, const uint8_t *challenge)
{
    if (node == NULL || challenge == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_contact_validation_begin(
        mcl_node_get_contact((mcl_node_t *)node), challenge);
}

MCLX_API int32_t mclx_contact_abandon_migration(void *node)
{
    if (node == NULL) {
        return MCLX_ERR_ARGUMENT;
    }
    return (int32_t)mcl_contact_abandon_migration(
        mcl_node_get_contact((mcl_node_t *)node));
}

MCLX_API int32_t mclx_contact_resolve_offer_collision(
    void *node, uint32_t peer_source_ref, uint32_t peer_migration_ref,
    int32_t peer_transport_id, int32_t peer_profile_id,
    uint32_t peer_endpoint_token, int32_t peer_validity, int32_t *out_outcome)
{
    mcl_contact_collision_t outcome = MCL_CONTACT_COLLISION_LOCAL_WINS;
    mcl_link_status_t lst;

    if (node == NULL || out_outcome == NULL) {
        return MCLX_ERR_ARGUMENT;
    }

    lst = mcl_contact_resolve_offer_collision(
        mcl_node_get_contact((mcl_node_t *)node), peer_source_ref,
        peer_migration_ref, (uint8_t)peer_transport_id, (uint8_t)peer_profile_id,
        peer_endpoint_token, (uint8_t)peer_validity, &outcome);
    *out_outcome = (int32_t)outcome;
    return (int32_t)lst;
}

#include "mcl/sdk.h"

/* ---------- transport-aware egress and ingress ----------
 *
 * Every send resolves WHICH transport before it encodes anything, and every
 * receive is told where the bytes arrived. See the tx callback contract in
 * sdk.h for why a single untagged transmit path cannot express a migration.
 */

/* Map a transmit callback's three-way return onto SDK status. */
static mcl_sdk_status_t mcl_node_tx_status(int32_t tx_res)
{
    if (tx_res == 0) {
        return MCL_SDK_OK;
    }
    if (tx_res < 0) {
        /* The transport is certain nothing left this machine. */
        return MCL_SDK_ERR_TX_NOT_SENT;
    }
    /*
     * The transport cannot say. Treated as possibly transmitted everywhere it
     * matters: the peer may hold these bytes, and assuming otherwise is the
     * inference that produces a split contact.
     */
    return MCL_SDK_ERR_TX_UNCERTAIN;
}

/*
 * Did the frame possibly leave? True for accepted and for uncertain.
 *
 * Used for anything that must not be repeated with the same value -- the
 * sequence number -- and for the irrevocable COMMIT transition.
 */
static int mcl_node_tx_possibly_sent(int32_t tx_res)
{
    return tx_res >= 0;
}

/* The link states in which a migration may be driven. See sdk.h. */
static int mcl_node_link_permits_migration(const mcl_node_t *node)
{
    return node->link.state == MCL_LINK_STATE_ESTABLISHED ||
           node->link.state == MCL_LINK_STATE_HANDOFF;
}

/*
 * Is this contact reachable on the transport these bytes arrived on?
 *
 * During a migration both media are legitimate: ordinary traffic may still be
 * on the old one while the controls are on the candidate. Anything else is a
 * bearer this contact does not live on.
 */
static int mcl_node_transport_belongs(
    const mcl_node_t *node,
    uint8_t arrival_transport)
{
    if (arrival_transport == MCL_CONTACT_TRANSPORT_RESERVED) {
        return 0;
    }
    if (arrival_transport == node->contact.active_transport) {
        return 1;
    }
    return node->contact.pending_migration_ref != MCL_CONTACT_MIGRATION_NONE &&
           arrival_transport == node->contact.pending_transport;
}

/*
 * Is the frame addressed elsewhere?
 *
 * A frame with no destination is for whoever hears it, which is how broadcast
 * first contact works. One that names a destination names it for a reason.
 */
static int mcl_node_frame_addressed_elsewhere(
    const mcl_node_t *node,
    const mcl_link_frame_t *frame)
{
    return (frame->flags & MCL_LINK_FLAG_DESTINATION) != 0u &&
           frame->destination_ref != node->source_ref;
}

/*
 * Fill the destination reference for a frame whose caller asked for one.
 *
 * WHY THE DESTINATION IS NOT A PARAMETER
 *
 * A node holds one contact, so the only machine it is in a position to address
 * is that contact's peer, and the reference for it was recorded during first
 * contact. Taking a destination as an argument would let a caller address a
 * frame to a machine this node has no contact with, and the correct value is
 * already known.
 *
 * WHAT THIS FIXES
 *
 * Every send path used to set destination_ref to zero unconditionally while
 * still honouring MCL_LINK_FLAG_DESTINATION from the caller's flags. Setting
 * the flag therefore produced a frame addressed to reference zero -- addressed
 * to nobody -- which a correct receiver refuses as NOT_ADDRESSED. The sending
 * half of the addressing rule did not exist: on a shared bearer a node could
 * not direct a frame at its peer at all, and MCL_SDK_NOT_ADDRESSED was
 * reachable only for frames this SDK had not produced. Found by the
 * dual-transport hardware harness, where two machines share a bearer and it
 * matters.
 *
 * Refusing when the peer is unknown mirrors the session_ref rule in
 * mcl_node_send_framed_tier0: emitting a reference this node has not learned
 * would invite a peer to correlate against something that does not exist.
 */
static mcl_sdk_status_t mcl_node_fill_destination(
    const mcl_node_t *node,
    mcl_link_frame_t *frame)
{
    if ((frame->flags & MCL_LINK_FLAG_DESTINATION) == 0u) {
        frame->destination_ref = 0u;
        return MCL_SDK_OK;
    }
    if (node->contact.peer_ref_valid == 0u) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    frame->destination_ref = node->contact.peer_ref;
    return MCL_SDK_OK;
}


mcl_sdk_status_t mcl_node_init(
    mcl_node_t *node,
    const mcl_node_config_t *config)
{
    mcl_link_status_t lst;

    if (node == NULL || config == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_init(&node->link, config->supported_wire_majors_mask);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    node->tx_fn = config->tx_fn;
    node->user_ctx = config->user_ctx;
    node->supported_wire_majors_mask = config->supported_wire_majors_mask;
    node->source_ref = config->source_ref;
    node->tx_sequence = 0u;

    /*
     * The contact begins on the configured transport. Its source_ref doubles as
     * the contact's local reference so the two cannot disagree about who this
     * node is within the contact.
     */
    lst = mcl_contact_begin(&node->contact, config->role,
                            config->source_ref, config->transport_id);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_reset(mcl_node_t *node)
{
    mcl_link_status_t lst;

    if (node == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_reset(&node->link);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }
    node->tx_sequence = 0u;

    /*
     * Reset the contact onto the transport it is currently using rather than
     * the one it started on. A reset clears session and migration state; it is
     * not a claim that the machine teleported back to the first medium.
     */
    lst = mcl_contact_begin(&node->contact, node->contact.role,
                            node->source_ref, node->contact.active_transport);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_send_tier0(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    mcl_wire_status_t wst;
    mcl_link_status_t lst;
    size_t written = 0u;
    int32_t tx_res;
    uint8_t transport = 0u;
    uint8_t quiesced = 0u;

    if (node == NULL || object == NULL || scratch == NULL || scratch_capacity == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    if (node->tx_fn == NULL) {
        return MCL_SDK_ERR_TX_UNAVAILABLE;
    }

    wst = mcl_wire_tier0_encode(object, scratch, scratch_capacity, &written);
    if (wst == MCL_WIRE_ERR_BUFFER_TOO_SMALL) {
        return MCL_SDK_ERR_BUFFER_TOO_SMALL;
    }
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    /*
     * The raw path carries no Link frame and therefore no contact routing, but
     * it still has to leave on a specific bearer. It uses the contact's data
     * transport, and is quiesced during a cutover for the same reason the
     * framed path is.
     */
    lst = mcl_contact_data_transport(&node->contact, &transport, &quiesced);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    if (quiesced != 0u) {
        return MCL_SDK_ERR_QUIESCED;
    }

    tx_res = node->tx_fn(node->user_ctx, transport, scratch, written);
    if (tx_res != 0) {
        return mcl_node_tx_status(tx_res);
    }

    if (bytes_sent != NULL) {
        *bytes_sent = written;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_receive_tier0(
    mcl_node_t *node,
    const uint8_t *data,
    size_t data_size,
    mcl_wire_tier0_t *object,
    size_t *consumed)
{
    mcl_wire_status_t wst;
    size_t bytes_consumed = 0u;

    if (node == NULL || data == NULL || object == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    /*
     * Parse Tier-0 object using Wire codec.
     * Note: Receiving an AUTHORITY_CLAIM, REQUEST, or other semantic does NOT
     * perform local policy execution, grant local privileges, or mutate Link context.
     * Product policy remains strictly outside the Machine Contact Layer.
     */
    wst = mcl_wire_tier0_decode(data, data_size, object, &bytes_consumed);
    if (wst == MCL_WIRE_ERR_BUFFER_TOO_SMALL || wst == MCL_WIRE_ERR_TRUNCATED) {
        return MCL_SDK_ERR_BUFFER_TOO_SMALL;
    }
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    if (consumed != NULL) {
        *consumed = bytes_consumed;
    }

    return MCL_SDK_OK;
}

mcl_contact_t *mcl_node_get_contact(mcl_node_t *node)
{
    return (node != NULL) ? &node->contact : NULL;
}

const mcl_contact_t *mcl_node_get_contact_const(const mcl_node_t *node)
{
    return (node != NULL) ? &node->contact : NULL;
}

mcl_link_t *mcl_node_get_link(mcl_node_t *node)
{
    return (node != NULL) ? &node->link : NULL;
}

const mcl_link_t *mcl_node_get_link_const(const mcl_node_t *node)
{
    return (node != NULL) ? &node->link : NULL;
}

mcl_sdk_status_t mcl_node_link_transition(
    mcl_node_t *node,
    mcl_link_state_t new_state)
{
    mcl_link_status_t lst;

    if (node == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    if (mcl_node_link_permits_migration(node) != 0 &&
        new_state != MCL_LINK_STATE_ESTABLISHED &&
        new_state != MCL_LINK_STATE_HANDOFF &&
        node->contact.pending_migration_ref != MCL_CONTACT_MIGRATION_NONE) {
        /*
         * The other half of the crossing invariant. Leaving the states where a
         * migration may be driven, while one is outstanding, would leave the
         * contact mid-transaction with no lifecycle able to carry it -- and
         * from COMMITTING it would abandon a commit the peer may have acted on
         * by a route that does not go through mcl_contact_abandon_migration at
         * all. Close the contact first, or finish the migration.
         */
        return MCL_SDK_ERR_INVALID_STATE;
    }

    lst = mcl_link_transition(&node->link, new_state);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_link_install_context(
    mcl_node_t *node,
    const mcl_link_context_key_t *key)
{
    mcl_link_status_t lst;

    if (node == NULL || key == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_install_context(&node->link, key);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_link_authorize_context(
    const mcl_node_t *node,
    const mcl_link_context_key_t *key)
{
    mcl_link_status_t lst;

    if (node == NULL || key == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    lst = mcl_link_authorize_context(&node->link, key);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    return MCL_SDK_OK;
}

/* ---------- Framed contact path ---------- */

/*
 * The one implementation behind both framed-send entry points.
 *
 * The majors are parameters rather than constants because Wire and Link
 * version INDEPENDENTLY: a Base 1 deployment needs Wire 1 inside Link 1, and
 * before this existed the node API could only emit Wire 0 inside Link 0, which
 * left the Stable pair reachable from the rendezvous and binding code but not
 * from the general node surface an integrator uses.
 */
static mcl_sdk_status_t node_send_framed_at_major(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    uint8_t wire_major,
    uint8_t link_major,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    uint8_t wire_buf[MCL_WIRE_TIER0_MAX_SIZE];
    mcl_link_frame_t frame;
    mcl_wire_status_t wst;
    mcl_link_status_t lst;
    mcl_sdk_status_t sst;
    size_t wire_written = 0u;
    size_t frame_written = 0u;
    int32_t tx_res;
    uint8_t transport = 0u;
    uint8_t quiesced = 0u;

    if (node == NULL || object == NULL || scratch == NULL || scratch_capacity == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
    if (node->tx_fn == NULL) {
        return MCL_SDK_ERR_TX_UNAVAILABLE;
    }

    /*
     * Resolve the bearer before encoding anything. During a cutover this
     * refuses rather than transmitting onto a medium the peer may have left;
     * see mcl_contact_data_transport.
     */
    lst = mcl_contact_data_transport(&node->contact, &transport, &quiesced);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    if (quiesced != 0u) {
        return MCL_SDK_ERR_QUIESCED;
    }

    wst = mcl_wire_tier0_encode_at_major(wire_major, object, wire_buf,
                                         sizeof(wire_buf), &wire_written);
    if (wst != MCL_WIRE_OK) {
        /*
         * Includes the Candidate-object-at-the-Stable-major refusal. A caller
         * asking for major 1 with a HAZARD is refused here rather than having
         * the object quietly demoted to a major that does carry it.
         */
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    frame.frame_class = frame_class;
    frame.flags = flags;
    frame.source_ref = node->source_ref;
    frame.session_ref = 0u;
    frame.sequence = 0u;

    sst = mcl_node_fill_destination(node, &frame);
    if (sst != MCL_SDK_OK) {
        return sst;
    }
    frame.freshness_ms = 0u;
    frame.payload = wire_buf;
    frame.payload_len = (uint16_t)wire_written;

    if ((flags & MCL_LINK_FLAG_SESSION) != 0u) {
        /*
         * The session reference comes from the CONTACT, never from the Wire
         * context.
         *
         * An earlier revision emitted node->link.active_context.context_id
         * here, which conflated two unrelated things:
         *
         *   session_ref   correlates a continuing contact across a transport
         *                 change; it is chosen in TRANSPORT_ACCEPT and lives
         *                 for as long as the contact does
         *
         *   context_id    names an installed semantic compression ruleset; it
         *                 has its own generation and can change, or never
         *                 exist, while the contact is perfectly healthy
         *
         * A machine can have a session with no context, the same session across
         * several context generations, or the same contact with an entirely new
         * context. Deriving one from the other made all three unrepresentable
         * and forced a session reference to depend on a compression decision.
         *
         * Emitting a session reference before a migration has been agreed would
         * invite a peer to correlate against a session that does not exist, so
         * that is refused rather than filled with a placeholder.
         */
        if (node->contact.session_valid == 0u) {
            return MCL_SDK_ERR_INVALID_STATE;
        }
        frame.session_ref = node->contact.session_ref;
    }
    if ((flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        frame.sequence = node->tx_sequence;
    }

    lst = mcl_link_frame_encode_at_major(link_major, &frame, scratch,
                                         scratch_capacity, &frame_written);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    tx_res = node->tx_fn(node->user_ctx, transport, scratch, frame_written);

    /*
     * Advance the sequence for anything that POSSIBLY left, not only for what
     * was acknowledged. A frame whose fate the transport could not report may
     * be in the peer's hands, and reusing its ordinal would give two different
     * frames one name.
     */
    if (mcl_node_tx_possibly_sent(tx_res) != 0 &&
        (flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        node->tx_sequence = (uint16_t)(node->tx_sequence + 1u);
    }
    if (tx_res != 0) {
        return mcl_node_tx_status(tx_res);
    }

    if (bytes_sent != NULL) {
        *bytes_sent = frame_written;
    }

    return MCL_SDK_OK;
}

/*
 * The historical entry point. Its majors are FROZEN at the experimental pair.
 *
 * v1.0 promises source compatibility (V1_SCOPE 4.5), and this function's bytes
 * are what every pre-major-1 caller and every retained receipt already contain
 * -- including the 104-migration continuity campaign. Retargeting it at the
 * Stable pair would silently change what an existing integration puts on the
 * wire, which is the one way to break compatibility that nobody can see.
 * test_sdk_framed.c pins this.
 */
mcl_sdk_status_t mcl_node_send_framed_tier0(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    return node_send_framed_at_major(node, object,
                                     MCL_WIRE_EXPERIMENTAL_MAJOR,
                                     MCL_LINK_FRAME_MAJOR,
                                     frame_class, flags, scratch,
                                     scratch_capacity, bytes_sent);
}

mcl_sdk_status_t mcl_node_send_framed_tier0_at_major(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    uint8_t wire_major,
    uint8_t link_major,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    return node_send_framed_at_major(node, object, wire_major, link_major,
                                     frame_class, flags, scratch,
                                     scratch_capacity, bytes_sent);
}

mcl_sdk_status_t mcl_node_receive_framed(
    mcl_node_t *node,
    uint8_t arrival_transport,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_wire_tier0_t *object,
    uint8_t *has_object,
    size_t *consumed)
{
    mcl_link_status_t lst;
    mcl_wire_status_t wst;
    size_t wire_consumed = 0u;

    if (node == NULL || data == NULL || frame == NULL ||
        has_object == NULL || consumed == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    *has_object = 0u;

    /*
     * Checked before decoding. A bearer this contact does not live on has no
     * business delivering frames into it, and refusing early keeps the decoder
     * off bytes that were never addressed to this contact at all.
     */
    if (mcl_node_transport_belongs(node, arrival_transport) == 0) {
        return MCL_SDK_ERR_WRONG_TRANSPORT;
    }

    /*
     * Decode the frame on its own terms first. A malformed frame must never
     * reach the semantic decoder, and an unknown frame class is rejected
     * rather than guessed at.
     */
    lst = mcl_link_frame_decode(data, data_size, frame, consumed);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    if (mcl_node_frame_addressed_elsewhere(node, frame) != 0) {
        /* Decoded, and for someone else. Reported rather than silently
         * dropped, so a caller can see it heard the frame. */
        return MCL_SDK_NOT_ADDRESSED;
    }

    /*
     * Only classes that carry semantics are handed to Wire. A KEEPALIVE or an
     * ACK has no payload to interpret, and attempting to decode one as a
     * semantic object would manufacture meaning that was never sent.
     *
     * CAPABILITY and NEGOTIATION were in this list until they acquired their
     * own contracts in mcl-link/spec/link-negotiation-v1.md. They carry Link
     * CONTROL payloads now, like ACK and HANDOFF do, and the caller decodes
     * them with mcl_link_capability_decode / mcl_link_negotiation_decode.
     * Passing one to the Tier-0 decoder would interpret a 9-byte capability
     * advertisement as whatever semantic object those bytes happen to spell.
     */
    if (frame->frame_class != MCL_LINK_CLASS_CONTACT &&
        frame->frame_class != MCL_LINK_CLASS_DATA) {
        return MCL_SDK_OK;
    }
    if (object == NULL || frame->payload == NULL || frame->payload_len == 0u) {
        return MCL_SDK_OK;
    }

    wst = mcl_wire_tier0_decode(frame->payload, (size_t)frame->payload_len,
                                object, &wire_consumed);
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }
    if (wire_consumed != (size_t)frame->payload_len) {
        /*
         * payload_len is an exact declared boundary, so a semantic object that
         * ends before it means the payload carries bytes nobody declared.
         * Accepting the object and ignoring the remainder is how a framing
         * discrepancy becomes a semantic one: two implementations would
         * disagree about what was sent while both believed they had decoded it
         * successfully.
         */
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    *has_object = 1u;
    return MCL_SDK_OK;
}

/* ---------- Framed path with Wire extensions ---------- */

mcl_sdk_status_t mcl_node_send_framed_tier0_ext(
    mcl_node_t *node,
    const mcl_wire_tier0_t *object,
    const mcl_wire_extension_t *extensions,
    size_t extension_count,
    mcl_link_frame_class_t frame_class,
    uint8_t flags,
    uint8_t *wire_scratch,
    size_t wire_scratch_capacity,
    uint8_t *frame_scratch,
    size_t frame_scratch_capacity,
    size_t *bytes_sent)
{
    mcl_link_frame_t frame;
    mcl_wire_status_t wst;
    mcl_link_status_t lst;
    mcl_sdk_status_t sst;
    size_t wire_written = 0u;
    size_t frame_written = 0u;
    int32_t tx_res;
    uint8_t transport = 0u;
    uint8_t quiesced = 0u;

    if (node == NULL || object == NULL || wire_scratch == NULL ||
        frame_scratch == NULL || wire_scratch_capacity == 0u ||
        frame_scratch_capacity == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
    if (node->tx_fn == NULL) {
        return MCL_SDK_ERR_TX_UNAVAILABLE;
    }

    lst = mcl_contact_data_transport(&node->contact, &transport, &quiesced);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    if (quiesced != 0u) {
        return MCL_SDK_ERR_QUIESCED;
    }

    wst = mcl_wire_tier0_encode_ext(object, extensions, extension_count,
                                    wire_scratch, wire_scratch_capacity,
                                    &wire_written);
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }
    if (wire_written > (size_t)MCL_LINK_FRAME_MAX_PAYLOAD) {
        /* The object encoded, but no legal Link frame can carry it. Reported
         * here rather than as a frame-encode failure, because the caller's
         * remedy is fewer or smaller extensions. */
        return MCL_SDK_ERR_BUFFER_TOO_SMALL;
    }

    frame.frame_class = frame_class;
    frame.flags = flags;
    frame.source_ref = node->source_ref;
    frame.session_ref = 0u;
    frame.sequence = 0u;

    sst = mcl_node_fill_destination(node, &frame);
    if (sst != MCL_SDK_OK) {
        return sst;
    }
    frame.freshness_ms = 0u;
    frame.payload = wire_scratch;
    frame.payload_len = (uint16_t)wire_written;

    if ((flags & MCL_LINK_FLAG_SESSION) != 0u) {
        /* The session reference comes from the contact, never from the Wire
         * context. See mcl_node_send_framed_tier0. */
        if (node->contact.session_valid == 0u) {
            return MCL_SDK_ERR_INVALID_STATE;
        }
        frame.session_ref = node->contact.session_ref;
    }
    if ((flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        frame.sequence = node->tx_sequence;
    }

    lst = mcl_link_frame_encode(&frame, frame_scratch, frame_scratch_capacity,
                                &frame_written);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    tx_res = node->tx_fn(node->user_ctx, transport, frame_scratch, frame_written);

    /* Advance for anything that possibly left; see mcl_node_send_framed_tier0. */
    if (mcl_node_tx_possibly_sent(tx_res) != 0 &&
        (flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        node->tx_sequence = (uint16_t)(node->tx_sequence + 1u);
    }
    if (tx_res != 0) {
        return mcl_node_tx_status(tx_res);
    }

    if (bytes_sent != NULL) {
        *bytes_sent = frame_written;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_receive_framed_ext(
    mcl_node_t *node,
    uint8_t arrival_transport,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_wire_tier0_t *object,
    mcl_wire_extension_reader_t *reader,
    mcl_wire_extension_accept_fn accept,
    void *accept_user,
    uint8_t *has_object,
    size_t *consumed)
{
    mcl_link_status_t lst;
    mcl_wire_status_t wst;
    size_t wire_consumed = 0u;

    if (node == NULL || data == NULL || frame == NULL || reader == NULL ||
        has_object == NULL || consumed == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    *has_object = 0u;

    if (mcl_node_transport_belongs(node, arrival_transport) == 0) {
        return MCL_SDK_ERR_WRONG_TRANSPORT;
    }

    lst = mcl_link_frame_decode(data, data_size, frame, consumed);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    if (mcl_node_frame_addressed_elsewhere(node, frame) != 0) {
        return MCL_SDK_NOT_ADDRESSED;
    }

    if (frame->frame_class != MCL_LINK_CLASS_CONTACT &&
        frame->frame_class != MCL_LINK_CLASS_DATA) {
        /* A class that carries no semantics yields no object rather than
         * having meaning manufactured for it. CAPABILITY and NEGOTIATION now
         * carry Link control payloads, not semantic objects; see the note on
         * the other receive path. */
        return MCL_SDK_OK;
    }
    if (object == NULL || frame->payload == NULL || frame->payload_len == 0u) {
        return MCL_SDK_OK;
    }

    wst = mcl_wire_tier0_decode_ext_accept(frame->payload,
                                           (size_t)frame->payload_len,
                                           object, reader, accept, accept_user,
                                           &wire_consumed);
    if (wst != MCL_WIRE_OK) {
        return MCL_SDK_ERR_WIRE_FAILURE;
    }
    if (wire_consumed != (size_t)frame->payload_len) {
        /* payload_len is an exact declared boundary. An object that ends before
         * it means the payload carries bytes nobody declared. */
        return MCL_SDK_ERR_WIRE_FAILURE;
    }

    *has_object = 1u;
    return MCL_SDK_OK;
}

/* ---------- Handoff control path ---------- */

mcl_sdk_status_t mcl_node_send_handoff(
    mcl_node_t *node,
    const mcl_handoff_control_t *control,
    uint8_t flags,
    uint8_t *scratch,
    size_t scratch_capacity,
    size_t *bytes_sent)
{
    uint8_t control_buf[MCL_HANDOFF_CONTROL_MAX_SIZE];
    mcl_link_frame_t frame;
    mcl_link_status_t lst;
    mcl_sdk_status_t sst;
    size_t control_written = 0u;
    size_t frame_written = 0u;
    int32_t tx_res;
    uint8_t transport = 0u;

    if (node == NULL || control == NULL || scratch == NULL ||
        scratch_capacity == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
    if (node->tx_fn == NULL) {
        return MCL_SDK_ERR_TX_UNAVAILABLE;
    }
    if (mcl_node_link_permits_migration(node) == 0) {
        /* The one place the two state machines cross, enforced here rather
         * than assumed. See the ownership note in sdk.h. */
        return MCL_SDK_ERR_INVALID_STATE;
    }

    /*
     * MANDATORY, not recommended. A post-acceptance control arrives on the
     * candidate transport, which the contact has not been using, so a receiver
     * holding several contacts must route it before it can parse a
     * class-specific payload -- and the only routing field it can use is the
     * outer session reference.
     */
    if ((flags & MCL_LINK_FLAG_SESSION) == 0u) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    /*
     * The bearer is chosen from the contact, never passed in. A handoff control
     * sent on the wrong medium proves nothing about the medium it claims to be
     * establishing.
     */
    lst = mcl_contact_control_transport(&node->contact, &transport);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_INVALID_STATE;
    }

    if (control->operation == MCL_HANDOFF_OP_COMMIT &&
        node->contact.state != MCL_CONTACT_STATE_VALIDATED &&
        node->contact.state != MCL_CONTACT_STATE_COMMITTING) {
        /* Committing an unvalidated path is the defect the contact state
         * machine exists to prevent; refused before anything is encoded. */
        return MCL_SDK_ERR_INVALID_STATE;
    }

    lst = mcl_handoff_control_encode(control, control_buf, sizeof(control_buf),
                                     &control_written);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    frame.frame_class = MCL_LINK_CLASS_HANDOFF;
    frame.flags = flags;
    frame.source_ref = node->source_ref;
    frame.session_ref = 0u;
    frame.sequence = 0u;

    sst = mcl_node_fill_destination(node, &frame);
    if (sst != MCL_SDK_OK) {
        return sst;
    }
    frame.freshness_ms = 0u;
    frame.payload = control_buf;
    frame.payload_len = (uint16_t)control_written;

    /*
     * The frame's session reference must be the control's own. A frame that
     * contradicted its payload would leave a receiver with two answers to
     * "which contact is this", and the redundancy check in
     * link-handoff-control-v0.1.md section 3 would fail against frames this
     * implementation itself produced.
     */
    if (node->contact.session_valid == 0u) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    if (node->contact.session_ref != control->session_ref) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    frame.session_ref = control->session_ref;

    if ((flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
        frame.sequence = node->tx_sequence;
    }

    lst = mcl_link_frame_encode(&frame, scratch, scratch_capacity,
                               &frame_written);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    tx_res = node->tx_fn(node->user_ctx, transport, scratch, frame_written);

    if (mcl_node_tx_possibly_sent(tx_res) != 0) {
        if ((flags & MCL_LINK_FLAG_SEQUENCE) != 0u) {
            node->tx_sequence = (uint16_t)(node->tx_sequence + 1u);
        }
        /*
         * THE COMMIT BOUNDARY.
         *
         * Sending COMMIT is irrevocable, so this transition belongs to the
         * transmission and not to a separate call the caller makes beforehand.
         * It happens for a frame that was accepted AND for one whose fate the
         * transport could not report, because in the second case the peer may
         * hold it -- and once it might, rolling back is a claim this machine
         * cannot make.
         *
         * It does NOT happen when the transport is certain nothing left, which
         * is the case that would otherwise strand a contact irrevocably in
         * COMMITTING over a frame nobody ever saw.
         *
         * Retransmission is a no-op here: the contact is already COMMITTING,
         * and re-sending is exactly what a peer in that state is supposed to do.
         */
        if (control->operation == MCL_HANDOFF_OP_COMMIT &&
            node->contact.state == MCL_CONTACT_STATE_VALIDATED) {
            lst = mcl_contact_commit_begin(&node->contact);
            if (lst != MCL_LINK_OK) {
                return MCL_SDK_ERR_LINK_FAILURE;
            }
        }
    }

    if (tx_res != 0) {
        return mcl_node_tx_status(tx_res);
    }
    if (bytes_sent != NULL) {
        *bytes_sent = frame_written;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_receive_handoff(
    mcl_node_t *node,
    uint8_t arrival_transport,
    const uint8_t *data,
    size_t data_size,
    mcl_link_frame_t *frame,
    mcl_handoff_control_t *control,
    size_t *consumed)
{
    mcl_link_status_t lst;
    uint8_t expected_transport = 0u;

    if (node == NULL || data == NULL || frame == NULL || control == NULL ||
        consumed == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    /*
     * THE CHECK PATH VALIDATION DEPENDS ON.
     *
     * A handoff control is admissible only on the transport the current
     * transaction is being conducted over -- the candidate while a migration is
     * in progress, the active transport otherwise. Without it a PATH_RESPONSE
     * delivered from the OLD path would validate a candidate that had never
     * carried a byte, which is the only thing the challenge/response exchange
     * exists to establish. Nothing else in the sequence can detect that: every
     * reference in the control would be correct.
     */
    lst = mcl_contact_control_transport(&node->contact, &expected_transport);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_INVALID_STATE;
    }
    if (arrival_transport != expected_transport) {
        return MCL_SDK_ERR_WRONG_TRANSPORT;
    }

    lst = mcl_link_frame_decode(data, data_size, frame, consumed);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    if (frame->frame_class != MCL_LINK_CLASS_HANDOFF) {
        /*
         * Refused rather than ignored. The frame class selects which registry
         * the payload's leading bytes belong to, so decoding a handoff control
         * out of another class would give one payload two meanings.
         */
        return MCL_SDK_ERR_FRAME_FAILURE;
    }
    if (frame->payload == NULL) {
        return MCL_SDK_ERR_FRAME_FAILURE;
    }
    if (mcl_node_frame_addressed_elsewhere(node, frame) != 0) {
        return MCL_SDK_NOT_ADDRESSED;
    }
    if ((frame->flags & MCL_LINK_FLAG_SESSION) == 0u) {
        /*
         * Required, not merely checked when present. A post-acceptance handoff
         * frame without a session reference cannot be routed to a contact by a
         * machine holding more than one, and accepting it here would mean the
         * routing field a multi-contact receiver depends on could simply be
         * absent from a frame this implementation accepts.
         */
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    /*
     * payload_len is an exact boundary. The control decoder enforces it, which
     * is why the length is passed through unmodified rather than capped.
     */
    lst = mcl_handoff_control_decode(frame->payload,
                                     (size_t)frame->payload_len, control);
    if (lst != MCL_LINK_OK) {
        return MCL_SDK_ERR_LINK_FAILURE;
    }

    if (frame->session_ref != control->session_ref) {
        /*
         * link-handoff-control-v0.1.md section 3. A frame that disagrees with
         * its own payload is rejected: there is no correct way to choose which
         * of the two is meant.
         */
        return MCL_SDK_ERR_FRAME_FAILURE;
    }

    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_node_apply_handoff(
    mcl_node_t *node,
    const mcl_handoff_control_t *control,
    mcl_handoff_action_t *action)
{
    mcl_link_status_t lst;
    uint8_t reconfirm = 0u;
    uint8_t reecho = 0u;

    if (node == NULL || control == NULL || action == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
    if (mcl_node_link_permits_migration(node) == 0) {
        return MCL_SDK_ERR_INVALID_STATE;
    }

    *action = MCL_HANDOFF_ACTION_NONE;

    switch (control->operation) {
    case MCL_HANDOFF_OP_PATH_CHALLENGE:
        if (node->contact.state == MCL_CONTACT_STATE_VALIDATED) {
            /*
             * A retransmitted challenge, because this peer's PATH_RESPONSE was
             * lost. It must be answered again: the peer is doing the only thing
             * available to it, and refusing would end a migration over one
             * dropped frame with both sides behaving correctly.
             *
             * Only an IDENTICAL challenge is re-echoed. A different one under
             * the same transaction is answered with silence -- an honest
             * retransmission repeats itself.
             */
            lst = mcl_contact_challenge_repeat(&node->contact,
                                               control->migration_ref,
                                               control->session_ref,
                                               control->challenge, &reecho);
            if (lst != MCL_LINK_OK) {
                return MCL_SDK_ERR_LINK_FAILURE;
            }
            if (reecho == 0u) {
                return MCL_SDK_ERR_INVALID_STATE;
            }
            *action = MCL_HANDOFF_ACTION_SEND_PATH_RESPONSE;
            return MCL_SDK_OK;
        }
        if (node->contact.state != MCL_CONTACT_STATE_AGREED) {
            return MCL_SDK_ERR_INVALID_STATE;
        }
        /*
         * The transaction is checked BEFORE any state moves. Recording the
         * challenge first and validating afterwards would let a control from an
         * abandoned transaction push this contact into VALIDATING, which is a
         * state change caused by a frame that was then refused.
         */
        if (node->contact.session_valid == 0u ||
            control->migration_ref != node->contact.pending_migration_ref ||
            control->session_ref != node->contact.session_ref) {
            return MCL_SDK_ERR_INVALID_STATE;
        }
        lst = mcl_contact_validation_begin(&node->contact, control->challenge);
        if (lst != MCL_LINK_OK) {
            return MCL_SDK_ERR_LINK_FAILURE;
        }
        lst = mcl_contact_validation_response(&node->contact,
                                              control->migration_ref,
                                              control->session_ref,
                                              control->challenge);
        if (lst != MCL_LINK_OK) {
            return MCL_SDK_ERR_LINK_FAILURE;
        }
        *action = MCL_HANDOFF_ACTION_SEND_PATH_RESPONSE;
        return MCL_SDK_OK;

    case MCL_HANDOFF_OP_PATH_RESPONSE:
        /*
         * From VALIDATING this completes validation; from VALIDATED it is a
         * duplicate and changes nothing. The path is already proven, and
         * proving it again is not a failure of anything.
         */
        lst = mcl_contact_validation_response(&node->contact,
                                              control->migration_ref,
                                              control->session_ref,
                                              control->challenge);
        if (lst != MCL_LINK_OK) {
            /*
             * A wrong echo is one wrong frame on a shared medium, not proof
             * that the path is bad. mcl_contact_validation_response leaves the
             * contact in VALIDATING deliberately, so the caller may retry.
             */
            return MCL_SDK_ERR_LINK_FAILURE;
        }
        return MCL_SDK_OK;

    case MCL_HANDOFF_OP_COMMIT:
        if (node->contact.state == MCL_CONTACT_STATE_ACTIVE) {
            /*
             * Retransmission after a lost CONFIRM. Answered again, and nothing
             * changes. See link-handoff-control-v0.1.md section 8.
             */
            lst = mcl_contact_commit_repeat(&node->contact,
                                            control->migration_ref,
                                            control->session_ref, &reconfirm);
            if (lst != MCL_LINK_OK) {
                return MCL_SDK_ERR_LINK_FAILURE;
            }
            if (reconfirm == 0u) {
                return MCL_SDK_ERR_INVALID_STATE;
            }
            *action = MCL_HANDOFF_ACTION_SEND_CONFIRM;
            return MCL_SDK_OK;
        }
        /*
         * One call, so a refused commit leaves the contact in VALIDATED rather
         * than stranded in COMMITTING. The receiving peer must never enter
         * COMMITTING: that state means "I sent COMMIT and do not know whether
         * it arrived", and rollback is forbidden there. A peer that has not
         * sent CONFIRM is in no such difficulty.
         */
        lst = mcl_contact_commit_accept(&node->contact,
                                        control->migration_ref,
                                        control->session_ref);
        if (lst != MCL_LINK_OK) {
            /* COMMIT before VALIDATED, or naming another transaction. */
            return MCL_SDK_ERR_LINK_FAILURE;
        }
        *action = MCL_HANDOFF_ACTION_SEND_CONFIRM;
        return MCL_SDK_OK;

    case MCL_HANDOFF_OP_CONFIRM:
        /*
         * From COMMITTING this completes the move; from ACTIVE it is a
         * duplicate of the confirmation that already completed it, and changes
         * nothing. mcl_contact_commit_confirm handles both and refuses a
         * CONFIRM from ACTIVE that names any other transaction.
         */
        lst = mcl_contact_commit_confirm(&node->contact,
                                         control->migration_ref,
                                         control->session_ref);
        if (lst != MCL_LINK_OK) {
            return MCL_SDK_ERR_LINK_FAILURE;
        }
        return MCL_SDK_OK;

    default:
        /*
         * Unreachable through mcl_node_receive_handoff, which rejects every
         * unassigned operation. Reachable if a caller builds one by hand.
         */
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
}

/*
 * The integration facade. See mcl/machine.h for what it is for.
 *
 * This file adds no protocol. Every rule it appears to implement is
 * `mcl_rdv_*`'s; what is here is the wiring an integrator would otherwise
 * write by hand, and the two places where the coordinator has to ask the
 * machine a question: open this bearer, and admit this stranger.
 */

#include "mcl/machine.h"

#include <string.h>

/* ------------------------------------------------------------- adapters
 *
 * The coordinator's platform vtable and this one are deliberately not the same
 * struct. `mcl_rdv_platform_t` is the protocol's view -- it needs a session
 * allocator and an endpoint-token allocator, which are protocol obligations
 * an integrator should not have to know exist. These shims supply both from
 * `random_bytes`, which is the correct answer for every deployment that has
 * one machine per coordinator, and is the answer this facade is for.
 */

static uint32_t shim_now(void *user)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    return m->platform.clock_ms(m->platform.user);
}

static int shim_random(void *user, uint8_t *out, size_t size)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    if (m->platform.random_bytes == NULL) {
        return -1;
    }
    return m->platform.random_bytes(m->platform.user, out, size);
}

static int shim_medium_busy(void *user)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    return (m->platform.medium_busy == NULL)
         ? 0
         : m->platform.medium_busy(m->platform.user);
}

static int shim_self_transmitting(void *user)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    return (m->platform.self_transmitting == NULL)
         ? 0
         : m->platform.self_transmitting(m->platform.user);
}

static int32_t shim_tx(void *user, uint8_t transport_id,
                       const uint8_t *data, size_t size)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    return m->platform.transport_send(m->platform.user, transport_id, data, size);
}

/*
 * A 32-bit value that is never zero, from the platform's randomness.
 *
 * Zero is the reserved "none" value for both a session and an endpoint token,
 * so a generator that could return it would occasionally produce a reference
 * the protocol is required to reject. Redrawing is correct and terminates; a
 * fallback of "1" would produce two machines with the same reference.
 */
static int nonzero_random(mcl_machine_t *m, uint32_t *out)
{
    uint8_t bytes[4];
    uint32_t value = 0u;
    unsigned attempt;

    if (m->platform.random_bytes == NULL) {
        return -1;
    }
    for (attempt = 0u; attempt < 8u; ++attempt) {
        if (m->platform.random_bytes(m->platform.user, bytes, sizeof(bytes)) != 0) {
            return -1;
        }
        value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
                ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
        if (value != 0u) {
            *out = value;
            return 0;
        }
    }
    return -1;
}

static int shim_allocate_session(void *user, uint32_t *out)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    return nonzero_random(m, out);
}

/*
 * This machine's reachability hint for the transaction being offered.
 *
 * Minted per transaction rather than fixed, because `BLE-ACTIVATE-1` makes the
 * token the match key of the advertisement a peer scans for: one machine
 * running two activations with one static token advertises identically for
 * both, and a scanner cannot tell them apart. Held in `local_endpoint_token`
 * because `candidate_open` needs it -- an offerer has to advertise the value
 * it published, and it never comes back to it on the wire.
 */
static int shim_allocate_endpoint_token(void *user, uint8_t transport_id,
                                        uint8_t profile_id, uint32_t *out)
{
    mcl_machine_t *m = (mcl_machine_t *)user;
    uint32_t value = 0u;

    (void)transport_id;
    (void)profile_id;
    if (nonzero_random(m, &value) != 0) {
        return -1;
    }
    m->local_endpoint_token = value;
    *out = value;
    return 0;
}

/* ------------------------------------------------- Base 1, arranged bearer
 *
 * The whole of MCL Base 1 on a bearer both machines already share. There is no
 * discovery, no offer/accept and no migration here: two machines announce
 * themselves on the bearer, each applies its own admission policy, and the
 * contact is established. That is exactly the claim in section 4 of
 * mcl-core/spec/conformance-profiles-v1.md and nothing more.
 */
#define MCL_BASE_STATE_IDLE        0u
#define MCL_BASE_STATE_ANNOUNCED   1u  /* our PRESENCE is out; nobody heard yet */
#define MCL_BASE_STATE_HEARD       2u  /* a peer PRESENCE arrived */
#define MCL_BASE_STATE_POLICY      3u  /* waiting for admit()/refuse() */
#define MCL_BASE_STATE_ADMITTED    4u  /* admitted; contact not yet reported */
#define MCL_BASE_STATE_ESTABLISHED 5u
#define MCL_BASE_STATE_REFUSED     6u

/*
 * Emit one PRESENCE on the arranged bearer, framed, at the Stable pair.
 *
 * Wire 1 inside Link 1 is the combination Base 1 requires and the one the node
 * API could not previously express.
 */
static mcl_machine_status_t base_announce(mcl_machine_t *machine)
{
    uint8_t scratch[MCL_LINK_FRAME_MAX_SIZE];
    mcl_wire_tier0_t obj;

    memset(&obj, 0, sizeof(obj));
    obj.kind = MCL_WIRE_KIND_PRESENCE;
    obj.priority = 1u;
    obj.source_ref = machine->config.source_ref;
    /*
     * machine_class is deliberately not set: major 1 does not carry it.
     * Populating it would be silently discarded rather than transmitted.
     */
    obj.body.presence.capability_tag = machine->config.capability_tag;
    obj.body.presence.ttl = machine->config.presence_ttl;

    if (mcl_node_send_framed_tier0_at_major(
            &machine->node, &obj,
            MCL_WIRE_STABLE_MAJOR, MCL_LINK_STABLE_MAJOR,
            MCL_LINK_CLASS_CONTACT, 0u,
            scratch, sizeof(scratch), NULL) != MCL_SDK_OK) {
        return MCL_MACHINE_ERR_STATE;
    }
    return MCL_MACHINE_OK;
}

/* --------------------------------------------------------- configuration */

mcl_machine_status_t mcl_machine_config_deployment(mcl_machine_config_t *config,
                                                   mcl_deployment_t deployment,
                                                   uint32_t source_ref,
                                                   mcl_contact_role_t role)
{
    if (config == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (deployment != MCL_DEPLOYMENT_REFERENCE_1 &&
        deployment != MCL_DEPLOYMENT_BASE_ARRANGED_1) {
        return MCL_MACHINE_ERR_CONFIG;
    }
    if (source_ref == 0u) {
        /*
         * Zero is what an uninitialised field looks like. A machine
         * announcing it would be indistinguishable from a machine that forgot
         * to set one, and every peer would correlate them together.
         */
        return MCL_MACHINE_ERR_CONFIG;
    }

    memset(config, 0, sizeof(*config));
    config->source_ref = source_ref;
    config->role = role;
    config->capability_tag = 1u;
    config->presence_ttl = 60u;

    if (deployment == MCL_DEPLOYMENT_BASE_ARRANGED_1) {
        /*
         * MCL-BASE-DEPLOYMENT-1. No bootstrap: Base 1 does not discover
         * anything, so there is no medium to solicit on and no candidate to
         * open. The arranged bearer is the contact's transport from the first
         * frame, and it carries the Stable pair.
         *
         * Kept in step with the published profile by
         * tools/check-base-deployment.sh, which reads both.
         */
        config->bootstrap_transport_id = MCL_CONTACT_TRANSPORT_IP;
        config->bearer_count = 1u;
        config->bearer_transport_id[0] = MCL_CONTACT_TRANSPORT_IP;
        config->bearer_profile_id[0] = 1u;   /* IP-DATAGRAM = 1, Stable */
        config->shared_medium = 0u;
        config->arranged_bearer = 1u;
        config->wire_major = MCL_WIRE_STABLE_MAJOR;
        config->deployment_profile_id = "MCL-BASE-DEPLOYMENT-1";
        return MCL_MACHINE_OK;
    }

    /* MCL-REFERENCE-DEPLOYMENT-1. Kept in step with the published profile by
       tools/check-reference-deployment.sh, which reads both. */
    config->wire_major = MCL_WIRE_EXPERIMENTAL_MAJOR;
    config->bootstrap_transport_id = MCL_CONTACT_TRANSPORT_AP;
    config->bearer_count = 2u;
    config->bearer_transport_id[0] = MCL_CONTACT_TRANSPORT_BLE;
    config->bearer_profile_id[0] = 1u;      /* BLE-GATT = 1, Stable */
    config->bearer_transport_id[1] = MCL_CONTACT_TRANSPORT_IP;
    config->bearer_profile_id[1] = 1u;      /* IP-DATAGRAM = 1, Stable */
    config->shared_medium = 1u;
    config->deployment_profile_id = "MCL-REFERENCE-DEPLOYMENT-1";
    return MCL_MACHINE_OK;
}

/* ---------------------------------------------------------------- init */

mcl_machine_status_t mcl_machine_init(mcl_machine_t *machine,
                                      const mcl_machine_config_t *config,
                                      const mcl_platform_t *platform)
{
    mcl_node_config_t node_config;
    mcl_rdv_config_t rdv_config;
    mcl_rdv_platform_t rdv_platform;
    unsigned i;

    if (machine == NULL || config == NULL || platform == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    /*
     * The three that cannot be defaulted. A machine with no clock cannot time
     * a retransmission, one with no transmit function cannot say anything, and
     * one with no way to open a bearer can meet a peer and never continue --
     * so the last is refused here rather than discovered as a contact that
     * always stops at agreement.
     */
    if (platform->clock_ms == NULL || platform->transport_send == NULL) {
        return MCL_MACHINE_ERR_CONFIG;
    }
    /*
     * `candidate_open` is required only where a candidate is opened. An
     * arranged-bearer Base 1 machine never opens one -- demanding a callback
     * that would never be called was the concrete reason the advertised
     * one-header API could not express the ordinary shared-bearer case.
     */
    if (config->arranged_bearer == 0u && platform->candidate_open == NULL) {
        return MCL_MACHINE_ERR_CONFIG;
    }
    if (config->bearer_count == 0u || config->bearer_count > MCL_RDV_MAX_BEARERS) {
        return MCL_MACHINE_ERR_CONFIG;
    }
    if (config->bootstrap_transport_id == MCL_CONTACT_TRANSPORT_RESERVED) {
        return MCL_MACHINE_ERR_CONFIG;
    }
    if (config->shared_medium != 0u &&
        (platform->random_bytes == NULL || platform->medium_busy == NULL ||
         platform->self_transmitting == NULL)) {
        /*
         * The same refusal mcl_rdv_init() makes, made one layer earlier so the
         * message names the operation the integrator did not implement. A
         * shared medium without randomness collides forever; without sensing,
         * a backoff can only delay.
         */
        return MCL_MACHINE_ERR_CONFIG;
    }

    memset(machine, 0, sizeof(*machine));
    machine->platform = *platform;
    machine->config = *config;

    memset(&node_config, 0, sizeof(node_config));
    /*
     * A Base 1 machine must support the Stable major, because that is what
     * Base 1 conformance means. The reference deployment keeps major 0: its
     * bootstrap choreography, its vectors and its retained receipts are all at
     * the experimental major, and moving it here would change bytes that
     * existing evidence describes.
     */
    node_config.supported_wire_majors_mask =
        (config->arranged_bearer != 0u)
            ? mcl_link_wire_major_mask(MCL_WIRE_STABLE_MAJOR)
            : mcl_link_wire_major_mask(MCL_WIRE_EXPERIMENTAL_MAJOR);
    node_config.source_ref = config->source_ref;
    node_config.transport_id = config->bootstrap_transport_id;
    node_config.role = config->role;
    node_config.tx_fn = shim_tx;
    node_config.user_ctx = machine;
    if (mcl_node_init(&machine->node, &node_config) != MCL_SDK_OK) {
        return MCL_MACHINE_ERR_CONFIG;
    }

    if (config->arranged_bearer != 0u) {
        /*
         * No rendezvous coordinator at all. Leaving it uninitialised would be
         * a trap for any later edit that polled it, so it is explicitly zeroed
         * and every arranged-bearer path below refuses to touch it.
         */
        memset(&machine->rdv, 0, sizeof(machine->rdv));
        machine->base_state = MCL_BASE_STATE_IDLE;
        machine->base_peer_ref = 0u;
        return MCL_MACHINE_OK;
    }

    memset(&rdv_platform, 0, sizeof(rdv_platform));
    rdv_platform.tx = shim_tx;
    rdv_platform.now_ms = shim_now;
    rdv_platform.random = (platform->random_bytes != NULL) ? shim_random : NULL;
    rdv_platform.medium_busy = (platform->medium_busy != NULL) ? shim_medium_busy : NULL;
    rdv_platform.self_transmitting =
        (platform->self_transmitting != NULL) ? shim_self_transmitting : NULL;
    rdv_platform.allocate_session =
        (platform->random_bytes != NULL) ? shim_allocate_session : NULL;
    rdv_platform.allocate_endpoint_token =
        (platform->random_bytes != NULL) ? shim_allocate_endpoint_token : NULL;
    rdv_platform.user = machine;

    memset(&rdv_config, 0, sizeof(rdv_config));
    rdv_config.source_ref = config->source_ref;
    rdv_config.bootstrap_transport_id = config->bootstrap_transport_id;
    rdv_config.bearer_count = config->bearer_count;
    for (i = 0u; i < config->bearer_count; ++i) {
        rdv_config.bearer_transport_id[i] = config->bearer_transport_id[i];
        rdv_config.bearer_profile_id[i] = config->bearer_profile_id[i];
    }
    rdv_config.role = config->role;
    rdv_config.shared_medium = config->shared_medium;
    rdv_config.capability_tag = config->capability_tag;
    rdv_config.presence_ttl = config->presence_ttl;
    /*
     * Timing left at zero, which means the profile's own values. This facade
     * does not expose the constants at all: a shared-medium deployment that
     * deviates from AP-BOOTSTRAP-1 is refused by mcl_rdv_init(), and offering
     * an integrator a knob whose only legal setting is the default would be
     * offering them a way to fail.
     */
    rdv_config.parameters = MCL_RDV_PARAMETERS_PROFILE;

    if (mcl_rdv_init(&machine->rdv, &rdv_config, &rdv_platform, &machine->node)
        != MCL_RDV_OK) {
        return MCL_MACHINE_ERR_CONFIG;
    }
    return MCL_MACHINE_OK;
}

mcl_machine_status_t mcl_machine_start(mcl_machine_t *machine)
{
    if (machine == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (machine->started != 0u) {
        return MCL_MACHINE_ERR_STATE;
    }
    if (machine->config.arranged_bearer != 0u) {
        mcl_machine_status_t bst;
        machine->started = 1u;
        machine->local_endpoint_token = 0u;
        bst = base_announce(machine);
        if (bst != MCL_MACHINE_OK) {
            machine->started = 0u;
            return bst;
        }
        machine->base_state = MCL_BASE_STATE_ANNOUNCED;
        return MCL_MACHINE_OK;
    }
    if (mcl_rdv_start(&machine->rdv) != MCL_RDV_OK) {
        return MCL_MACHINE_ERR_STATE;
    }
    machine->started = 1u;
    machine->local_endpoint_token = 0u;
    return MCL_MACHINE_OK;
}

/* ---------------------------------------------------------------- poll */

static void emit(mcl_machine_event_t *out, mcl_machine_event_kind_t kind,
                 const mcl_rdv_event_t *from)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (from != NULL) {
        out->transport_id = from->transport_id;
        out->profile_id = from->profile_id;
        out->peer_ref = from->peer_ref;
        out->session_ref = from->session_ref;
    }
}

/*
 * The one piece of work this facade does on the integrator's behalf that is
 * not pure translation: noticing that a bearer has been agreed and asking the
 * machine to open it.
 *
 * Without it every builder writes this themselves, and the ones who forget get
 * a coordinator that emits PATH_CHALLENGE on a bearer nobody opened -- which
 * looks exactly like a peer that never answered.
 */
static mcl_machine_status_t open_candidate(mcl_machine_t *machine,
                                           const mcl_rdv_event_t *ev)
{
    mcl_machine_candidate_t answer;
    uint32_t local_token;

    /* TRANSPORT_OFFER carries the offerer's endpoint token and ACCEPT carries
       no reciprocal token.  Therefore an acceptor must never be handed a
       local token left over from a transaction in which it was the offerer. */
    local_token = (ev->peer_endpoint_token == 0u)
                ? machine->local_endpoint_token : 0u;

    answer = machine->platform.candidate_open(machine->platform.user,
                                              ev->transport_id,
                                              ev->profile_id,
                                              ev->peer_endpoint_token,
                                              local_token);
    switch (answer) {
    case MCL_MACHINE_CANDIDATE_READY:
        machine->candidate_open_transport = ev->transport_id;
        machine->awaiting_candidate = 0u;
        if (mcl_rdv_candidate_ready(&machine->rdv) != MCL_RDV_OK) {
            if (machine->platform.candidate_close != NULL) {
                machine->platform.candidate_close(machine->platform.user,
                                                  ev->transport_id);
            }
            machine->candidate_open_transport = 0u;
            return MCL_MACHINE_ERR_STATE;
        }
        return MCL_MACHINE_OK;
    case MCL_MACHINE_CANDIDATE_PENDING:
        machine->candidate_open_transport = ev->transport_id;
        machine->awaiting_candidate = 1u;
        return MCL_MACHINE_OK;
    case MCL_MACHINE_CANDIDATE_REFUSED:
    default:
        machine->awaiting_candidate = 0u;
        (void)mcl_rdv_candidate_refused(&machine->rdv);
        return MCL_MACHINE_ERR_STATE;
    }
}

mcl_machine_status_t mcl_machine_poll(mcl_machine_t *machine,
                                      mcl_machine_event_t *out)
{
    mcl_rdv_event_t ev;
    mcl_rdv_status_t rc;
    mcl_rdv_state_t state;

    if (machine == NULL || out == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (machine->started == 0u) {
        return MCL_MACHINE_ERR_STATE;
    }

    if (machine->pending_platform_error != 0u) {
        machine->pending_platform_error = 0u;
        emit(out, MCL_MACHINE_EVENT_ERROR, NULL);
        out->status = MCL_MACHINE_ERR_STATE;
        return MCL_MACHINE_OK;
    }

    if (machine->config.arranged_bearer != 0u) {
        switch (machine->base_state) {
        case MCL_BASE_STATE_HEARD:
            emit(out, MCL_MACHINE_EVENT_PEER_DETECTED, NULL);
            out->transport_id = machine->config.bearer_transport_id[0];
            out->profile_id = machine->config.bearer_profile_id[0];
            out->peer_ref = machine->base_peer_ref;
            /*
             * Policy is the integrator's, never MCL's. With no callback the
             * caller is asked; there is no third branch in which reception
             * alone admits a peer.
             */
            if (machine->platform.policy_admit != NULL) {
                int admitted = machine->platform.policy_admit(
                    machine->platform.user, machine->base_peer_ref,
                    machine->config.bearer_transport_id[0]);
                machine->base_state = (admitted != 0)
                                          ? MCL_BASE_STATE_ADMITTED
                                          : MCL_BASE_STATE_REFUSED;
            } else {
                machine->base_state = MCL_BASE_STATE_POLICY;
            }
            return MCL_MACHINE_OK;

        case MCL_BASE_STATE_POLICY:
            emit(out, MCL_MACHINE_EVENT_POLICY_REQUIRED, NULL);
            out->transport_id = machine->config.bearer_transport_id[0];
            out->profile_id = machine->config.bearer_profile_id[0];
            out->peer_ref = machine->base_peer_ref;
            /* Held here until admit() or refuse() answers. */
            machine->base_state = MCL_BASE_STATE_POLICY + 0x10u;
            return MCL_MACHINE_OK;

        case MCL_BASE_STATE_ADMITTED:
            emit(out, MCL_MACHINE_EVENT_CONTACT_ESTABLISHED, NULL);
            out->transport_id = machine->config.bearer_transport_id[0];
            out->profile_id = machine->config.bearer_profile_id[0];
            out->peer_ref = machine->base_peer_ref;
            machine->base_state = MCL_BASE_STATE_ESTABLISHED;
            return MCL_MACHINE_OK;

        case MCL_BASE_STATE_REFUSED:
            emit(out, MCL_MACHINE_EVENT_CONTACT_LOST, NULL);
            out->peer_ref = machine->base_peer_ref;
            machine->base_peer_ref = 0u;
            machine->base_state = MCL_BASE_STATE_ANNOUNCED;
            return MCL_MACHINE_OK;

        default:
            emit(out, MCL_MACHINE_EVENT_NONE, NULL);
            return MCL_MACHINE_OK;
        }
    }

    memset(&ev, 0, sizeof(ev));
    rc = mcl_rdv_poll(&machine->rdv, &ev);
    if (rc != MCL_RDV_OK && rc != MCL_RDV_TX_UNCERTAIN) {
        emit(out, MCL_MACHINE_EVENT_ERROR, NULL);
        out->status = MCL_MACHINE_ERR_STATE;
        return MCL_MACHINE_OK;
    }

    /*
     * Some pre-contact failures correctly return the coordinator to
     * ANNOUNCING without inventing CONTACT_LOST: no contact existed yet.  The
     * platform resource is still real, though, and the facade owns the open,
     * so close it whenever the coordinator leaves every candidate/contact
     * state.  CONTACT_LOST below sees the zero and cannot close it twice.
     */
    state = mcl_rdv_state(&machine->rdv);
    if (machine->candidate_open_transport != 0u &&
        (state == MCL_RDV_STATE_IDLE ||
         state == MCL_RDV_STATE_ANNOUNCING ||
         state == MCL_RDV_STATE_HEARD ||
         state == MCL_RDV_STATE_OFFERING ||
         state == MCL_RDV_STATE_EXHAUSTED ||
         state == MCL_RDV_STATE_CLOSED ||
         state == MCL_RDV_STATE_ACCEPTING ||
         state == MCL_RDV_STATE_SOLICITING)) {
        if (machine->platform.candidate_close != NULL) {
            machine->platform.candidate_close(machine->platform.user,
                                              machine->candidate_open_transport);
        }
        machine->candidate_open_transport = 0u;
        machine->awaiting_candidate = 0u;
        machine->local_endpoint_token = 0u;
    }

    switch (ev.kind) {
    case MCL_RDV_EVENT_PEER_DISCOVERED:
        emit(out, MCL_MACHINE_EVENT_PEER_DETECTED, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_BEARER_AGREED:
        if (open_candidate(machine, &ev) != MCL_MACHINE_OK) {
            emit(out, MCL_MACHINE_EVENT_ERROR, &ev);
            out->status = MCL_MACHINE_ERR_STATE;
            return MCL_MACHINE_OK;
        }
        /*
         * Agreement is not a contact and is not reported as one. What the
         * caller gets is CONTACT_ESTABLISHED, after the path has actually been
         * proven and the migration has committed.
         */
        emit(out, MCL_MACHINE_EVENT_NONE, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_POLICY_REQUIRED:
        if (machine->platform.policy_admit != NULL) {
            const int admit = machine->platform.policy_admit(
                machine->platform.user, ev.peer_ref, ev.transport_id);
            if (admit != 0) {
                (void)mcl_rdv_admit(&machine->rdv);
            } else {
                (void)mcl_rdv_refuse(&machine->rdv);
            }
            emit(out, MCL_MACHINE_EVENT_NONE, &ev);
            return MCL_MACHINE_OK;
        }
        emit(out, MCL_MACHINE_EVENT_POLICY_REQUIRED, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_CANDIDATE_VALIDATED:
        /* Reachability is proven; the contact is not yet migrated. Not an
           application event -- the application asked for a contact. */
        emit(out, MCL_MACHINE_EVENT_NONE, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_CONTACT_MIGRATED:
        emit(out, MCL_MACHINE_EVENT_CONTACT_ESTABLISHED, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_CONTACT_LOST:
        if (machine->platform.candidate_close != NULL &&
            machine->candidate_open_transport != 0u) {
            machine->platform.candidate_close(machine->platform.user,
                                              machine->candidate_open_transport);
            machine->candidate_open_transport = 0u;
        }
        machine->awaiting_candidate = 0u;
        machine->local_endpoint_token = 0u;
        emit(out, MCL_MACHINE_EVENT_CONTACT_LOST, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_NO_COMMON_BEARER:
        emit(out, MCL_MACHINE_EVENT_NO_COMMON_BEARER, &ev);
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_SECURITY_ESTABLISHED:
        /*
         * Unreachable in this release, and reported as an error rather than
         * translated. MCL v1 has no security mechanism, so a machine that saw
         * this event would be being told a property no code here provides.
         */
        emit(out, MCL_MACHINE_EVENT_ERROR, &ev);
        out->status = MCL_MACHINE_ERR_STATE;
        return MCL_MACHINE_OK;

    case MCL_RDV_EVENT_NONE:
    default:
        emit(out, MCL_MACHINE_EVENT_NONE, NULL);
        return MCL_MACHINE_OK;
    }
}

mcl_machine_status_t mcl_machine_receive(mcl_machine_t *machine,
                                         uint8_t transport_id,
                                         const uint8_t *data,
                                         size_t size)
{
    if (machine == NULL || data == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (machine->started == 0u) {
        return MCL_MACHINE_ERR_STATE;
    }
    if (machine->config.arranged_bearer != 0u) {
        mcl_link_frame_t frame;
        mcl_wire_tier0_t obj;
        uint8_t has_object = 0u;
        size_t consumed = 0u;

        if (mcl_node_receive_framed(&machine->node, transport_id, data, size,
                                    &frame, &obj, &has_object, &consumed)
            != MCL_SDK_OK) {
            /* Undecodable bytes are normal on a real bearer. Not an error. */
            return MCL_MACHINE_OK;
        }
        if (has_object == 0u || obj.kind != MCL_WIRE_KIND_PRESENCE) {
            return MCL_MACHINE_OK;
        }
        if (obj.source_ref == machine->config.source_ref) {
            /* Our own announcement echoed back by a loopback bearer. */
            return MCL_MACHINE_OK;
        }
        if (machine->base_state == MCL_BASE_STATE_ANNOUNCED) {
            machine->base_peer_ref = obj.source_ref;
            machine->base_state = MCL_BASE_STATE_HEARD;
        }
        return MCL_MACHINE_OK;
    }

    if (mcl_rdv_deliver(&machine->rdv, transport_id, data, size) != MCL_RDV_OK) {
        /*
         * Not an error worth propagating. A public medium carries everything,
         * and a caller that treated undecodable bytes as a fault would report
         * a passing car as a protocol failure.
         */
        return MCL_MACHINE_OK;
    }
    return MCL_MACHINE_OK;
}

mcl_machine_status_t mcl_machine_candidate_ready(mcl_machine_t *machine)
{
    if (machine == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (machine->awaiting_candidate == 0u) {
        return MCL_MACHINE_ERR_STATE;
    }
    machine->awaiting_candidate = 0u;
    if (mcl_rdv_candidate_ready(&machine->rdv) != MCL_RDV_OK) {
        if (machine->platform.candidate_close != NULL &&
            machine->candidate_open_transport != 0u) {
            machine->platform.candidate_close(machine->platform.user,
                                              machine->candidate_open_transport);
        }
        machine->candidate_open_transport = 0u;
        return MCL_MACHINE_ERR_STATE;
    }
    return MCL_MACHINE_OK;
}

mcl_machine_status_t mcl_machine_candidate_refused(mcl_machine_t *machine)
{
    if (machine == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (machine->awaiting_candidate == 0u) {
        return MCL_MACHINE_ERR_STATE;
    }
    machine->awaiting_candidate = 0u;
    if (machine->platform.candidate_close != NULL &&
        machine->candidate_open_transport != 0u) {
        machine->platform.candidate_close(machine->platform.user,
                                          machine->candidate_open_transport);
    }
    machine->candidate_open_transport = 0u;
    if (mcl_rdv_candidate_refused(&machine->rdv) != MCL_RDV_OK) {
        return MCL_MACHINE_ERR_STATE;
    }
    machine->pending_platform_error = 1u;
    return MCL_MACHINE_OK;
}

mcl_machine_status_t mcl_machine_admit(mcl_machine_t *machine)
{
    if (machine != NULL && machine->config.arranged_bearer != 0u) {
        if (machine->base_state != (MCL_BASE_STATE_POLICY + 0x10u)) {
            return MCL_MACHINE_ERR_STATE;
        }
        machine->base_state = MCL_BASE_STATE_ADMITTED;
        return MCL_MACHINE_OK;
    }
    if (machine == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (mcl_rdv_admit(&machine->rdv) != MCL_RDV_OK) {
        return MCL_MACHINE_ERR_STATE;
    }
    return MCL_MACHINE_OK;
}

mcl_machine_status_t mcl_machine_refuse(mcl_machine_t *machine)
{
    if (machine != NULL && machine->config.arranged_bearer != 0u) {
        if (machine->base_state != (MCL_BASE_STATE_POLICY + 0x10u)) {
            return MCL_MACHINE_ERR_STATE;
        }
        machine->base_state = MCL_BASE_STATE_REFUSED;
        return MCL_MACHINE_OK;
    }
    if (machine == NULL) {
        return MCL_MACHINE_ERR_NULL;
    }
    if (mcl_rdv_refuse(&machine->rdv) != MCL_RDV_OK) {
        return MCL_MACHINE_ERR_STATE;
    }
    return MCL_MACHINE_OK;
}

mcl_node_t *mcl_machine_node(mcl_machine_t *machine)
{
    if (machine == NULL) {
        return NULL;
    }
    return &machine->node;
}

const char *mcl_machine_state_name(const mcl_machine_t *machine)
{
    if (machine == NULL) {
        return "NULL";
    }
    switch (mcl_rdv_state(&machine->rdv)) {
    case MCL_RDV_STATE_IDLE:              return "IDLE";
    case MCL_RDV_STATE_ANNOUNCING:        return "ANNOUNCING";
    case MCL_RDV_STATE_HEARD:             return "HEARD";
    case MCL_RDV_STATE_OFFERING:          return "OFFERING";
    case MCL_RDV_STATE_AGREED:            return "AGREED";
    case MCL_RDV_STATE_VALIDATING:        return "VALIDATING";
    case MCL_RDV_STATE_MIGRATED:          return "MIGRATED";
    case MCL_RDV_STATE_EXHAUSTED:         return "EXHAUSTED";
    case MCL_RDV_STATE_CLOSED:            return "CLOSED";
    case MCL_RDV_STATE_CANDIDATE_PENDING: return "CANDIDATE_PENDING";
    case MCL_RDV_STATE_ADMITTING:         return "ADMITTING";
    case MCL_RDV_STATE_COMMITTING:        return "COMMITTING";
    case MCL_RDV_STATE_ACCEPTING:         return "ACCEPTING";
    case MCL_RDV_STATE_SOLICITING:        return "SOLICITING";
    default:                              return "UNKNOWN";
    }
}

const char *mcl_machine_event_name(mcl_machine_event_kind_t kind)
{
    switch (kind) {
    case MCL_MACHINE_EVENT_NONE:                return "NONE";
    case MCL_MACHINE_EVENT_PEER_DETECTED:       return "PEER_DETECTED";
    case MCL_MACHINE_EVENT_POLICY_REQUIRED:     return "POLICY_REQUIRED";
    case MCL_MACHINE_EVENT_CONTACT_ESTABLISHED: return "CONTACT_ESTABLISHED";
    case MCL_MACHINE_EVENT_CONTACT_LOST:        return "CONTACT_LOST";
    case MCL_MACHINE_EVENT_NO_COMMON_BEARER:    return "NO_COMMON_BEARER";
    case MCL_MACHINE_EVENT_ERROR:               return "ERROR";
    default:                                    return "UNKNOWN";
    }
}

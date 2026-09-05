/*
 * Layer rules from mcl-core/spec/conformance-profiles-v1.md, as code.
 *
 * No allocation, no I/O, no global state. The evaluation is a pure function of
 * the two structs handed in, which is what lets a node run it at init on a
 * microcontroller and print the result.
 */

#include "mcl/conformance.h"

#include <string.h>

/* The two constants that make this file honest.
 *
 * They are not properties of any build. They are properties of MCL, and while
 * they hold, the corresponding layer is unclaimable by everyone -- including
 * this implementation. Each becomes 0 on the day its specification lands, and
 * nothing else in this file has to change.
 */
#define MCL_AP_BOOTSTRAP_1_EXISTS 0
#define MCL_SECURITY_PROFILE_EXISTS 0

static uint32_t base_unmet(const mcl_build_capabilities_t *c)
{
    uint32_t unmet = 0u;

    if (!c->wire_major_1)        unmet |= MCL_CONFORMANCE_UNMET_WIRE_MAJOR_1;
    if (!c->link_major_1)        unmet |= MCL_CONFORMANCE_UNMET_LINK_MAJOR_1;
    if (!c->stable_tier0_kernel) unmet |= MCL_CONFORMANCE_UNMET_TIER0_KERNEL;
    if (!c->negotiation)         unmet |= MCL_CONFORMANCE_UNMET_NEGOTIATION;
    if (!c->refusal_semantics)   unmet |= MCL_CONFORMANCE_UNMET_REFUSAL;
    /*
     * "At least one transport binding, so that the implementation can be made
     * to communicate at all. Which one is unconstrained, and that is exactly
     * why Base 1 guarantees nothing about meeting a stranger."
     */
    if (c->transports_mask == 0u) unmet |= MCL_CONFORMANCE_UNMET_NO_TRANSPORT;

    return unmet;
}

static uint32_t stranger_unmet(const mcl_build_capabilities_t *c)
{
    uint32_t unmet = base_unmet(c);

    if (!c->bootstrap_profile)       unmet |= MCL_CONFORMANCE_UNMET_BOOTSTRAP;
    if (!c->bootstrap_offer_accept)  unmet |= MCL_CONFORMANCE_UNMET_BOOTSTRAP_OFFER;
    if (!c->shared_medium)           unmet |= MCL_CONFORMANCE_UNMET_SHARED_MEDIUM;
    if (!c->no_common_bearer_report) unmet |= MCL_CONFORMANCE_UNMET_NO_BEARER_REPORT;

#if !MCL_AP_BOOTSTRAP_1_EXISTS
    /*
     * Independent of anything the caller asserted. A builder cannot set a flag
     * to make a profile exist, and a library that let them would be helping
     * them ship a false claim.
     */
    unmet |= MCL_CONFORMANCE_UNMET_BOOTSTRAP_UNSPECIFIED;
#endif

    return unmet;
}

static uint32_t secure_unmet(const mcl_build_capabilities_t *c)
{
    uint32_t unmet = stranger_unmet(c);

    if (c->security_profile_id == 0u) unmet |= MCL_CONFORMANCE_UNMET_SECURITY_PROFILE;

#if !MCL_SECURITY_PROFILE_EXISTS
    unmet |= MCL_CONFORMANCE_UNMET_SECURITY_UNSPECIFIED;
#endif

    return unmet;
}

mcl_sdk_status_t mcl_conformance_evaluate(
    const mcl_build_capabilities_t *caps,
    mcl_conformance_layer_t requested,
    mcl_conformance_report_t *out)
{
    if (caps == NULL || out == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }
    if (requested > MCL_CONFORMANCE_SECURE_STRANGER_1) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));
    out->requested = requested;

    /*
     * Layers are cumulative and a claim is all or nothing: an implementation
     * that meets part of a layer claims the layer below it. So `attainable` is
     * found by walking up, not by scoring.
     */
    out->attainable = MCL_CONFORMANCE_NONE;
    if (base_unmet(caps) == 0u) {
        out->attainable = MCL_CONFORMANCE_BASE_1;
        if (stranger_unmet(caps) == 0u) {
            out->attainable = MCL_CONFORMANCE_STRANGER_CONTACT_1;
            if (secure_unmet(caps) == 0u) {
                out->attainable = MCL_CONFORMANCE_SECURE_STRANGER_1;
            }
        }
    }

    switch (requested) {
    case MCL_CONFORMANCE_NONE:
        out->unmet = 0u;
        break;
    case MCL_CONFORMANCE_BASE_1:
        out->unmet = base_unmet(caps);
        break;
    case MCL_CONFORMANCE_STRANGER_CONTACT_1:
        out->unmet = stranger_unmet(caps);
        break;
    default:
        out->unmet = secure_unmet(caps);
        break;
    }

    out->claim_stands = (out->unmet == 0u) ? 1u : 0u;
    return MCL_SDK_OK;
}

mcl_sdk_status_t mcl_deployment_check(
    const mcl_build_capabilities_t *caps,
    const mcl_deployment_requirements_t *req,
    mcl_deployment_report_t *out)
{
    mcl_conformance_report_t layer;
    mcl_sdk_status_t rc;

    if (caps == NULL || req == NULL || out == NULL) {
        return MCL_SDK_ERR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));

    rc = mcl_conformance_evaluate(caps, req->required_layer, &layer);
    if (rc != MCL_SDK_OK) {
        return rc;
    }
    if (!layer.claim_stands) {
        out->unmet |= MCL_DEPLOYMENT_UNMET_LAYER;
    }

    /* Conjunction, not alternation. See the header. */
    out->missing_transports_mask =
        (uint32_t)(req->mandatory_transports_mask & ~caps->transports_mask);
    if (out->missing_transports_mask != 0u) {
        out->unmet |= MCL_DEPLOYMENT_UNMET_TRANSPORTS;
    }

    /*
     * The policy floor of mcl-link/research/mcl-s1-benchmark-design.md §1.
     * Where a deployment requires a security profile, a build that does not
     * have it is UNSATISFIED -- it must not quietly continue unsecured, and the
     * caller learns that here rather than discovering it in a street.
     */
    if (req->required_security_profile_id != 0u &&
        caps->security_profile_id != req->required_security_profile_id) {
        out->unmet |= MCL_DEPLOYMENT_UNMET_SECURITY;
    }

    out->satisfied = (out->unmet == 0u) ? 1u : 0u;
    return MCL_SDK_OK;
}

const char *mcl_conformance_layer_name(mcl_conformance_layer_t layer)
{
    switch (layer) {
    case MCL_CONFORMANCE_NONE:               return "none";
    case MCL_CONFORMANCE_BASE_1:             return "MCL Base 1";
    case MCL_CONFORMANCE_STRANGER_CONTACT_1: return "MCL Stranger-Contact 1";
    case MCL_CONFORMANCE_SECURE_STRANGER_1:  return "MCL Secure-Stranger 1";
    default:                                 return "unknown";
    }
}

const char *mcl_conformance_unmet_name(uint32_t single_bit)
{
    switch (single_bit) {
    case MCL_CONFORMANCE_UNMET_WIRE_MAJOR_1:
        return "Wire major 1 not implemented";
    case MCL_CONFORMANCE_UNMET_LINK_MAJOR_1:
        return "Link major 1 not implemented";
    case MCL_CONFORMANCE_UNMET_TIER0_KERNEL:
        return "Stable Tier-0 kernel not implemented";
    case MCL_CONFORMANCE_UNMET_NEGOTIATION:
        return "capability/version negotiation not implemented";
    case MCL_CONFORMANCE_UNMET_REFUSAL:
        return "refusal semantics not implemented";
    case MCL_CONFORMANCE_UNMET_NO_TRANSPORT:
        return "no transport binding";
    case MCL_CONFORMANCE_UNMET_BOOTSTRAP:
        return "AP-BOOTSTRAP-1 not implemented in both directions";
    case MCL_CONFORMANCE_UNMET_BOOTSTRAP_OFFER:
        return "offer/accept not carried over the bootstrap path";
    case MCL_CONFORMANCE_UNMET_SHARED_MEDIUM:
        return "shared-medium behaviour not implemented";
    case MCL_CONFORMANCE_UNMET_NO_BEARER_REPORT:
        return "no explicit no-common-bearer outcome";
    case MCL_CONFORMANCE_UNMET_SECURITY_PROFILE:
        return "no security profile implemented";
    case MCL_CONFORMANCE_UNMET_BOOTSTRAP_UNSPECIFIED:
        return "AP-BOOTSTRAP-1 is not specified yet: this layer is unclaimable "
               "by anyone, and that is a fact about MCL rather than this build";
    case MCL_CONFORMANCE_UNMET_SECURITY_UNSPECIFIED:
        return "no named security profile exists yet: this layer is unclaimable "
               "by anyone";
    default:
        return "unknown requirement";
    }
}

const char *mcl_deployment_unmet_name(uint32_t single_bit)
{
    switch (single_bit) {
    case MCL_DEPLOYMENT_UNMET_LAYER:
        return "build does not attain the conformance layer this deployment requires";
    case MCL_DEPLOYMENT_UNMET_TRANSPORTS:
        return "build lacks a mandatory continuation bearer";
    case MCL_DEPLOYMENT_UNMET_SECURITY:
        return "deployment requires a security profile this build does not have";
    default:
        return "unknown deployment requirement";
    }
}

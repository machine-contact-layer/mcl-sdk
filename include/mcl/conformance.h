/*
 * What this build may honestly claim, and what a deployment still wants.
 *
 * `mcl-core/spec/conformance-profiles-v1.md` defines the named claims an
 * implementation may make about itself. `mcl-core/spec/deployment-profile-v1.md`
 * defines how a deployment selects the optional pieces that are mandatory
 * there. Both are documents. This is the part a builder can run.
 *
 * WHY THIS EXISTS
 *
 * The two-builder audit found that "implements MCL" describes a legal
 * implementation and tells another machine nothing it can rely on. The fix was
 * named conformance layers -- but a layer stated only in prose is a layer a
 * builder discovers they failed to meet after shipping. So the layer rules are
 * also here, as code, evaluated against what the build actually contains.
 *
 * WHAT THIS IS NOT
 *
 * It is not proof. Every field in mcl_build_capabilities_t is asserted by the
 * builder, and nothing here can detect a false assertion -- exactly as
 * `link-negotiation-v1.md` §7 says a CAPABILITY frame is a claim rather than a
 * verification. What this catches is the honest mistake: a build that claims a
 * layer it did not wire up, or that meets a deployment's requirements in every
 * respect but one.
 *
 * The conformance kit in `mcl-core/conformance/` is what tests behaviour. This
 * checks consistency between a claim and a configuration, at init, before a
 * machine is in a street.
 */

#ifndef MCL_CONFORMANCE_H
#define MCL_CONFORMANCE_H

#include "mcl/sdk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ layers */

typedef uint8_t mcl_conformance_layer_t;
enum {
    /* Claims nothing. The honest value for a partial build. */
    MCL_CONFORMANCE_NONE = 0u,
    /*
     * Given a bearer both machines already share, two Base 1 implementations
     * interoperate. Guarantees nothing about meeting a stranger, and a build
     * claiming only this must not describe itself as capable of stranger
     * contact.
     */
    MCL_CONFORMANCE_BASE_1 = 1u,
    /*
     * Two implementations placed within range of one another, with no prior
     * configuration, will detect one another and exchange first contact.
     *
     * Claimable since 2026-09-06, when mcl-ap/spec/ap-bootstrap-1.md landed.
     * The claim carries MCL_CONFORMANCE_CAVEAT_BOOTSTRAP_CANDIDATE, because
     * that profile is Candidate rather than Stable.
     *
     * This comment said "NOT CLAIMABLE TODAY by anyone" while the same header
     * below it, and the implementation, granted the claim. A reader who
     * stopped at the enum got the opposite of the truth.
     */
    MCL_CONFORMANCE_STRANGER_CONTACT_1 = 2u,
    /* Reserved name. Not specified; requires a security profile that does not
       exist. */
    MCL_CONFORMANCE_SECURE_STRANGER_1 = 3u
};

/* ------------------------------------------------ what the build contains */

/*
 * Asserted by the builder about their own build. Every flag is 0 or 1;
 * `transports_mask` carries one bit per transport_id from the registry in
 * mcl/contact.h, so bit 2 is MCL_IP and bit 3 is MCL_BLE.
 */
typedef struct {
    uint8_t  wire_major_1;          /* encodes and decodes Wire major 1 */
    uint8_t  link_major_1;          /* frame layout and frozen class dispositions */
    uint8_t  stable_tier0_kernel;   /* PRESENCE, TRANSPORT_OFFER, TRANSPORT_ACCEPT */
    uint8_t  negotiation;           /* capability/version negotiation and the floor */
    uint8_t  refusal_semantics;     /* unknown opcode, major, reserved bit refused */
    uint32_t transports_mask;       /* at least one bit set for Base 1 */

    /* Stranger-Contact 1 only. */
    uint8_t  bootstrap_profile;     /* AP-BOOTSTRAP-1, emit AND receive */
    uint8_t  bootstrap_offer_accept;/* offer/accept carried over the bootstrap path */
    uint8_t  shared_medium;         /* listen-before-transmit, reply scheduling,
                                       duplicate suppression, backoff */
    uint8_t  no_common_bearer_report; /* the distinct, legible outcome */

    /* Secure-Stranger 1 only. 0 means none. */
    uint8_t  security_profile_id;
} mcl_build_capabilities_t;

/* ---------------------------------------------------------- unmet reasons */

enum {
    MCL_CONFORMANCE_UNMET_WIRE_MAJOR_1      = 0x0001u,
    MCL_CONFORMANCE_UNMET_LINK_MAJOR_1      = 0x0002u,
    MCL_CONFORMANCE_UNMET_TIER0_KERNEL      = 0x0004u,
    MCL_CONFORMANCE_UNMET_NEGOTIATION       = 0x0008u,
    MCL_CONFORMANCE_UNMET_REFUSAL           = 0x0010u,
    MCL_CONFORMANCE_UNMET_NO_TRANSPORT      = 0x0020u,
    MCL_CONFORMANCE_UNMET_BOOTSTRAP         = 0x0040u,
    MCL_CONFORMANCE_UNMET_BOOTSTRAP_OFFER   = 0x0080u,
    MCL_CONFORMANCE_UNMET_SHARED_MEDIUM     = 0x0100u,
    MCL_CONFORMANCE_UNMET_NO_BEARER_REPORT  = 0x0200u,
    MCL_CONFORMANCE_UNMET_SECURITY_PROFILE  = 0x0400u,
    /*
     * Set whenever Stranger-Contact 1 or above is requested, regardless of what
     * the caller asserted, for as long as AP-BOOTSTRAP-1 does not exist.
     *
     * `conformance-profiles-v1.md` §5.2: until that profile is specified,
     * bake-off selected, clean-room implemented and assigned a Standards Action
     * identifier, NO implementation may claim the layer. A builder cannot opt
     * out of this by setting a flag, because the thing missing is not in their
     * build -- it is in MCL.
     *
     * NO LONGER SET AS OF 2026-09-06: `mcl-ap/spec/ap-bootstrap-1.md` exists.
     * The bit is retained rather than renumbered, because a caller compiled
     * against the earlier header must not silently start reading a different
     * reason from the same value.
     */
    MCL_CONFORMANCE_UNMET_BOOTSTRAP_UNSPECIFIED = 0x0800u,
    /* Same shape, for the layer above: no named security profile exists. */
    MCL_CONFORMANCE_UNMET_SECURITY_UNSPECIFIED  = 0x1000u
};

/* ------------------------------------------------------------- caveats */

/*
 * A caveat is not an unmet requirement. The claim STANDS and something about it
 * is weaker than the layer name suggests, so it travels with the claim instead
 * of blocking it.
 *
 * The distinction earns its place: refusing a claim whose specification is in
 * the tree and implementable would be as wrong as granting one that sounds
 * frozen and is not.
 */
enum {
    /*
     * AP-BOOTSTRAP-1 exists and is CANDIDATE, not Stable.
     *
     * Every measurement of its waveform comes from one transmitter class, and
     * there is direct evidence the choice does not travel: a laptop speaker
     * with a measured notch at one of the two tones recovered 1 of 3 where the
     * reference transmitter recovers 9 of 15. §11 of the profile lists what
     * promotion requires. A deployment may implement it and MUST NOT describe
     * it as frozen.
     */
    MCL_CONFORMANCE_CAVEAT_BOOTSTRAP_CANDIDATE = 0x0001u
};

typedef struct {
    /* The highest layer this build may honestly claim. */
    mcl_conformance_layer_t attainable;
    /* What the caller asked to claim. */
    mcl_conformance_layer_t requested;
    /* Why `requested` was not attainable. Zero when the claim stands. */
    uint32_t unmet;
    /*
     * Weaknesses that travel WITH a standing claim rather than blocking it.
     * A caller that reports `claim_stands` without reporting these is
     * overstating what it was told.
     */
    uint32_t caveats;
    /* 1 when requested <= attainable. */
    uint8_t  claim_stands;
} mcl_conformance_report_t;

/*
 * Evaluate a build against the layer rules.
 *
 * Returns MCL_SDK_OK whether or not the claim stands -- an unmet claim is a
 * finding to report, not a call that failed. Check `claim_stands`.
 */
mcl_sdk_status_t mcl_conformance_evaluate(
    const mcl_build_capabilities_t *caps,
    mcl_conformance_layer_t requested,
    mcl_conformance_report_t *out);

/* ------------------------------------------------------ deployment checks */

/*
 * The machine-relevant subset of a deployment profile. A builder reads the
 * profile JSON with whatever their platform provides and fills this in; the
 * SDK does not parse JSON, because a freestanding node has no reason to carry
 * a parser and `mcl-core/tools/validate_deployment_profile.c` already validates
 * the document where a filesystem exists.
 */
typedef struct {
    mcl_conformance_layer_t required_layer;
    uint32_t mandatory_transports_mask;  /* every bit is required, not any */
    uint8_t  required_security_profile_id;
} mcl_deployment_requirements_t;

enum {
    MCL_DEPLOYMENT_UNMET_LAYER      = 0x0001u,
    MCL_DEPLOYMENT_UNMET_TRANSPORTS = 0x0002u,
    MCL_DEPLOYMENT_UNMET_SECURITY   = 0x0004u
};

typedef struct {
    uint32_t unmet;
    /* Mandatory transports the deployment requires and this build lacks. */
    uint32_t missing_transports_mask;
    uint8_t  satisfied;
} mcl_deployment_report_t;

/*
 * Check a build against a deployment's requirements.
 *
 * `mandatory_transports_mask` is a CONJUNCTION: every bit must be present.
 * That mirrors `deployment-profile-v1.md` §3.2, where there is deliberately no
 * way to write "BLE or IP" -- an alternation would let two members of the same
 * deployment satisfy it differently and share nothing, which is the empty
 * intersection the whole layer exists to remove.
 *
 * Returns MCL_SDK_OK for a satisfied or unsatisfied deployment alike. An
 * unsatisfied one is a fact the machine must surface, not an error in asking.
 */
mcl_sdk_status_t mcl_deployment_check(
    const mcl_build_capabilities_t *caps,
    const mcl_deployment_requirements_t *req,
    mcl_deployment_report_t *out);

/* -------------------------------------------------------------- reporting */

/* Stable, non-NULL names for logs and start-up banners. */
const char *mcl_conformance_layer_name(mcl_conformance_layer_t layer);
const char *mcl_conformance_unmet_name(uint32_t single_bit);
const char *mcl_deployment_unmet_name(uint32_t single_bit);

/* Names one caveat bit. A caller reporting `claim_stands` without reporting
   these is overstating what it was told. */
const char *mcl_conformance_caveat_name(uint32_t single_bit);

#ifdef __cplusplus
}
#endif

#endif /* MCL_CONFORMANCE_H */

/*
 * Conformance-layer and deployment-requirement checks.
 *
 * The cases that matter are the refusals. A checker that says yes to everything
 * passes a suite of things that should pass, so most of this file is builds
 * that must NOT get the claim they asked for.
 */

#include "mcl/conformance.h"

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void check(int condition, const char *what)
{
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL %s\n", what);
    }
}

/* A build with everything Base 1 requires and nothing above it. */
static void base_build(mcl_build_capabilities_t *c)
{
    memset(c, 0, sizeof(*c));
    c->wire_major_1 = 1u;
    c->link_major_1 = 1u;
    c->stable_tier0_kernel = 1u;
    c->negotiation = 1u;
    c->refusal_semantics = 1u;
    c->transports_mask = (1u << 2) | (1u << 3);   /* MCL_IP and MCL_BLE */
}

static void test_base(void)
{
    mcl_build_capabilities_t c;
    mcl_conformance_report_t r;

    printf("[conformance] Base 1\n");

    base_build(&c);
    check(mcl_conformance_evaluate(&c, MCL_CONFORMANCE_BASE_1, &r) == MCL_SDK_OK,
          "evaluate returns OK");
    check(r.claim_stands == 1u, "a complete Base build may claim Base 1");
    check(r.unmet == 0u, "nothing unmet");
    check(r.attainable == MCL_CONFORMANCE_BASE_1, "attainable is Base 1");

    /* A library with no transport binding cannot be made to communicate. */
    base_build(&c);
    c.transports_mask = 0u;
    (void)mcl_conformance_evaluate(&c, MCL_CONFORMANCE_BASE_1, &r);
    check(r.claim_stands == 0u, "no transport binding fails Base 1");
    check((r.unmet & MCL_CONFORMANCE_UNMET_NO_TRANSPORT) != 0u,
          "and says which requirement");
    check(r.attainable == MCL_CONFORMANCE_NONE, "attains nothing");

    /* A claim is all or nothing: one missing piece drops the whole layer. */
    base_build(&c);
    c.negotiation = 0u;
    (void)mcl_conformance_evaluate(&c, MCL_CONFORMANCE_BASE_1, &r);
    check(r.claim_stands == 0u, "missing negotiation fails Base 1");
    check((r.unmet & MCL_CONFORMANCE_UNMET_NEGOTIATION) != 0u,
          "and says so specifically");
}

static void test_stranger_is_unclaimable(void)
{
    mcl_build_capabilities_t c;
    mcl_conformance_report_t r;

    printf("[conformance] Stranger-Contact 1 is unclaimable by anyone\n");

    /*
     * The load-bearing test. A builder asserts EVERY stranger-contact
     * capability -- as an implementation with a real acoustic bootstrap
     * eventually will -- and the claim must still be refused, because
     * AP-BOOTSTRAP-1 does not exist. A builder cannot set a flag to make a
     * profile exist, and a library that let them would help them ship a false
     * claim.
     */
    base_build(&c);
    c.bootstrap_profile = 1u;
    c.bootstrap_offer_accept = 1u;
    c.shared_medium = 1u;
    c.no_common_bearer_report = 1u;

    (void)mcl_conformance_evaluate(&c, MCL_CONFORMANCE_STRANGER_CONTACT_1, &r);
    check(r.claim_stands == 0u,
          "a fully asserted stranger build STILL cannot claim the layer");
    check((r.unmet & MCL_CONFORMANCE_UNMET_BOOTSTRAP_UNSPECIFIED) != 0u,
          "and the reason names MCL, not the build");
    check((r.unmet & MCL_CONFORMANCE_UNMET_BOOTSTRAP) == 0u,
          "the build's own bootstrap assertion is not second-guessed");
    check(r.attainable == MCL_CONFORMANCE_BASE_1,
          "so the highest honest claim stays Base 1");

    /* Secure-Stranger is refused for its own reason as well as inheriting. */
    c.security_profile_id = 1u;
    (void)mcl_conformance_evaluate(&c, MCL_CONFORMANCE_SECURE_STRANGER_1, &r);
    check(r.claim_stands == 0u, "Secure-Stranger 1 is unclaimable too");
    check((r.unmet & MCL_CONFORMANCE_UNMET_SECURITY_UNSPECIFIED) != 0u,
          "because no named security profile exists");
}

static void test_cumulative(void)
{
    mcl_build_capabilities_t c;
    mcl_conformance_report_t r;

    printf("[conformance] layers are cumulative\n");

    /* Stranger capabilities without the Base ones underneath them. */
    memset(&c, 0, sizeof(c));
    c.bootstrap_profile = 1u;
    c.bootstrap_offer_accept = 1u;
    c.shared_medium = 1u;
    c.no_common_bearer_report = 1u;

    (void)mcl_conformance_evaluate(&c, MCL_CONFORMANCE_STRANGER_CONTACT_1, &r);
    check((r.unmet & MCL_CONFORMANCE_UNMET_WIRE_MAJOR_1) != 0u,
          "a higher layer still reports the lower layer's unmet requirements");
    check(r.attainable == MCL_CONFORMANCE_NONE, "and attains nothing");
}

static void test_none_and_arguments(void)
{
    mcl_build_capabilities_t c;
    mcl_conformance_report_t r;

    printf("[conformance] claiming nothing, and bad arguments\n");

    memset(&c, 0, sizeof(c));
    (void)mcl_conformance_evaluate(&c, MCL_CONFORMANCE_NONE, &r);
    check(r.claim_stands == 1u, "an empty build may claim nothing, honestly");

    check(mcl_conformance_evaluate(NULL, MCL_CONFORMANCE_BASE_1, &r)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "NULL caps refused");
    check(mcl_conformance_evaluate(&c, MCL_CONFORMANCE_BASE_1, NULL)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "NULL out refused");
    check(mcl_conformance_evaluate(&c, (mcl_conformance_layer_t)99u, &r)
              == MCL_SDK_ERR_INVALID_ARGUMENT, "unknown layer refused");
}

static void test_deployment(void)
{
    mcl_build_capabilities_t c;
    mcl_deployment_requirements_t req;
    mcl_deployment_report_t d;

    printf("[deployment] requirements\n");

    /* A Base deployment requiring BLE, met by a build that has BLE. */
    base_build(&c);
    memset(&req, 0, sizeof(req));
    req.required_layer = MCL_CONFORMANCE_BASE_1;
    req.mandatory_transports_mask = (1u << 3);

    check(mcl_deployment_check(&c, &req, &d) == MCL_SDK_OK, "check returns OK");
    check(d.satisfied == 1u, "satisfied when the bearer is present");

    /* The same deployment, a build with IP only. */
    base_build(&c);
    c.transports_mask = (1u << 2);
    (void)mcl_deployment_check(&c, &req, &d);
    check(d.satisfied == 0u, "unsatisfied when the mandatory bearer is absent");
    check((d.unmet & MCL_DEPLOYMENT_UNMET_TRANSPORTS) != 0u, "reports why");
    check(d.missing_transports_mask == (1u << 3), "and names the bearer");

    /*
     * Conjunction, not alternation. A deployment requiring BOTH bearers is not
     * satisfied by a build with one, which is the empty-intersection bug the
     * schema forbids expressing.
     */
    base_build(&c);
    c.transports_mask = (1u << 3);
    req.mandatory_transports_mask = (1u << 2) | (1u << 3);
    (void)mcl_deployment_check(&c, &req, &d);
    check(d.satisfied == 0u, "two mandatory bearers are a conjunction");
    check(d.missing_transports_mask == (1u << 2), "and the missing one is named");

    /*
     * The policy floor: a deployment requiring security is UNSATISFIED for a
     * build without it. It must not quietly continue unsecured.
     */
    base_build(&c);
    memset(&req, 0, sizeof(req));
    req.required_layer = MCL_CONFORMANCE_BASE_1;
    req.required_security_profile_id = 1u;
    (void)mcl_deployment_check(&c, &req, &d);
    check(d.satisfied == 0u, "required security is a hard failure");
    check((d.unmet & MCL_DEPLOYMENT_UNMET_SECURITY) != 0u, "reported as such");

    /* A deployment requiring a layer the build cannot attain. */
    base_build(&c);
    memset(&req, 0, sizeof(req));
    req.required_layer = MCL_CONFORMANCE_STRANGER_CONTACT_1;
    (void)mcl_deployment_check(&c, &req, &d);
    check(d.satisfied == 0u, "unattainable required layer is unsatisfied");
    check((d.unmet & MCL_DEPLOYMENT_UNMET_LAYER) != 0u, "reported as such");
}

static void test_names(void)
{
    printf("[reporting] names\n");
    check(strcmp(mcl_conformance_layer_name(MCL_CONFORMANCE_BASE_1),
                 "MCL Base 1") == 0, "layer name matches the specification");
    check(mcl_conformance_layer_name((mcl_conformance_layer_t)99u) != NULL,
          "unknown layer still names something");
    check(mcl_conformance_unmet_name(MCL_CONFORMANCE_UNMET_NO_TRANSPORT) != NULL,
          "every unmet reason has a name");
    check(mcl_deployment_unmet_name(MCL_DEPLOYMENT_UNMET_SECURITY) != NULL,
          "every deployment reason has a name");
}

int main(void)
{
    printf("=== MCL conformance and deployment checks ===\n");
    test_base();
    test_stranger_is_unclaimable();
    test_cumulative();
    test_none_and_arguments();
    test_deployment();
    test_names();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

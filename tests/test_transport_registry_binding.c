/*
 * Pins the transport-id registry against the binding headers that implement it.
 *
 * WHY THIS TEST EXISTS
 *
 * These two artifacts drifted apart once already and nothing caught it. The
 * registry in mcl-link assigns MCL_IP = 2 and MCL_BLE = 3; a published Wire
 * conformance vector encodes a TRANSPORT_OFFER with transport_id 2 while
 * borrowing its source_ref and endpoint_token from a research-corpus scenario
 * that describes a BLE handoff. The vector bytes are correct and the binding
 * headers are correct, but anyone cross-referencing the vector to the corpus
 * would conclude that 2 means BLE and would build an implementation that could
 * not migrate to anything.
 *
 * A transport identifier is the one value two unrelated implementations must
 * agree on before they can change transport at all. A silent disagreement here
 * does not fail loudly at run time: each peer connects to the wrong kind of
 * endpoint and simply never completes contact.
 *
 * So this test asserts the agreement mechanically rather than trusting that
 * three repositories and a registry stay aligned by review.
 *
 * The registry is parsed as text on purpose. Reproducing the assignments as C
 * constants here would only pin this file against itself.
 */

#include "mcl/sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MCL_HAVE_IP_BINDING)
#include "mcl/ip_binding.h"
#endif
#if defined(MCL_HAVE_BLE_BINDING)
#include "mcl/ble_binding.h"
#endif
#if defined(MCL_HAVE_UWB_BINDING)
#include "mcl/uwb_binding.h"
#endif

#ifndef MCL_TRANSPORT_REGISTRY_PATH
#error "MCL_TRANSPORT_REGISTRY_PATH must be defined by the build"
#endif

#define CHECK_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL at %s:%d: (%s) is false\n", \
                __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

#define MAX_REGISTRY_BYTES 16384

static char g_registry[MAX_REGISTRY_BYTES];
static int g_checks = 0;

static void load_registry(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        fprintf(stderr, "FAIL: cannot open transport registry at %s\n", path);
        exit(1);
    }
    n = fread(g_registry, 1u, sizeof(g_registry) - 1u, f);
    fclose(f);

    /*
     * A truncated read would silently weaken every assertion below, because a
     * name that fell off the end would simply not be found and the lookup would
     * report "absent" rather than "unreadable".
     */
    if (n == 0u || n >= sizeof(g_registry) - 1u) {
        fprintf(stderr, "FAIL: registry unreadable or larger than %d bytes\n",
                (int)sizeof(g_registry));
        exit(1);
    }
    g_registry[n] = '\0';
}

/*
 * Returns the id assigned to `name`, or -1 if the name is absent. Scans for the
 * name and then reads the "id" of the object containing it, by walking back to
 * the opening brace. Deliberately simple: this test must not become a JSON
 * parser whose own bugs could mask a real disagreement.
 */
static int registry_id_for(const char *name)
{
    char needle[64];
    const char *hit;
    const char *obj;
    const char *id_key;
    int value = 0;

    (void)snprintf(needle, sizeof(needle), "\"name\": \"%s\"", name);
    hit = strstr(g_registry, needle);
    if (hit == NULL) {
        /* Tolerate the compact spelling as well, so formatting is not load-bearing. */
        (void)snprintf(needle, sizeof(needle), "\"name\":\"%s\"", name);
        hit = strstr(g_registry, needle);
    }
    if (hit == NULL) {
        return -1;
    }

    obj = hit;
    while (obj > g_registry && *obj != '{') {
        --obj;
    }
    if (*obj != '{') {
        return -1;
    }

    id_key = strstr(obj, "\"id\"");
    if (id_key == NULL || id_key > hit) {
        return -1;
    }
    id_key += 4;
    while (*id_key == ' ' || *id_key == ':') {
        ++id_key;
    }
    if (*id_key < '0' || *id_key > '9') {
        return -1;
    }
    while (*id_key >= '0' && *id_key <= '9') {
        value = (value * 10) + (*id_key - '0');
        ++id_key;
    }
    return value;
}

/*
 * Each binding declares its own transport_id at the top of its profile
 * registry. That is a second place the assignment is written down, so it is a
 * second place it can drift. Checked here for the same reason as the headers.
 */
static void check_profile_registry(const char *path, const char *name, int expected_id)
{
    char buf[MAX_REGISTRY_BYTES];
    FILE *f = fopen(path, "rb");
    const char *hit;
    int value = 0;
    size_t n;

    if (f == NULL) {
        fprintf(stderr, "FAIL: cannot open profile registry %s\n", path);
        exit(1);
    }
    n = fread(buf, 1u, sizeof(buf) - 1u, f);
    fclose(f);
    if (n == 0u || n >= sizeof(buf) - 1u) {
        fprintf(stderr, "FAIL: %s unreadable or too large\n", path);
        exit(1);
    }
    buf[n] = '\0';

    hit = strstr(buf, "\"transport_id\"");
    if (hit == NULL) {
        fprintf(stderr, "FAIL: %s declares no transport_id\n", path);
        exit(1);
    }
    hit += strlen("\"transport_id\"");
    while (*hit == ' ' || *hit == ':') { ++hit; }
    if (*hit < '0' || *hit > '9') {
        fprintf(stderr, "FAIL: %s transport_id is not a number\n", path);
        exit(1);
    }
    while (*hit >= '0' && *hit <= '9') {
        value = (value * 10) + (*hit - '0');
        ++hit;
    }

    ++g_checks;
    if (value != expected_id) {
        fprintf(stderr, "FAIL: %s declares transport_id %d, registry says %d\n",
                path, value, expected_id);
        exit(1);
    }
    printf("%-9s profile registry declares transport %d, matching\n", name, value);
}

int main(void)
{
    int checked = 0;

    load_registry(MCL_TRANSPORT_REGISTRY_PATH);

    /*
     * The registry must define all four assignments. If one disappeared, the
     * per-binding checks below would pass vacuously.
     */
    CHECK_TRUE(registry_id_for("MCL_AP") == 1);
    CHECK_TRUE(registry_id_for("MCL_IP") == 2);
    CHECK_TRUE(registry_id_for("MCL_BLE") == 3);
    CHECK_TRUE(registry_id_for("MCL_UWB") == 4);
    printf("registry assignments present: AP=1 IP=2 BLE=3 UWB=4\n");

    /* Distinctness. Two bindings sharing an id would make offers ambiguous. */
    CHECK_TRUE(registry_id_for("MCL_AP") != registry_id_for("MCL_IP"));
    CHECK_TRUE(registry_id_for("MCL_IP") != registry_id_for("MCL_BLE"));
    CHECK_TRUE(registry_id_for("MCL_BLE") != registry_id_for("MCL_UWB"));

    /* Zero is permanently reserved so an uninitialised field is never valid. */
    CHECK_TRUE(registry_id_for("MCL_AP") != 0);
    CHECK_TRUE(registry_id_for("MCL_IP") != 0);
    CHECK_TRUE(registry_id_for("MCL_BLE") != 0);
    CHECK_TRUE(registry_id_for("MCL_UWB") != 0);

#if defined(MCL_HAVE_IP_BINDING)
    CHECK_TRUE((int)MCL_IP_TRANSPORT_ID == registry_id_for("MCL_IP"));
    printf("mcl-ip   header %u matches registry\n", (unsigned)MCL_IP_TRANSPORT_ID);
    ++checked;
#endif
#if defined(MCL_HAVE_BLE_BINDING)
    CHECK_TRUE((int)MCL_BLE_TRANSPORT_ID == registry_id_for("MCL_BLE"));
    printf("mcl-ble  header %u matches registry\n", (unsigned)MCL_BLE_TRANSPORT_ID);
    ++checked;
#endif
#if defined(MCL_HAVE_UWB_BINDING)
    CHECK_TRUE((int)MCL_UWB_TRANSPORT_ID == registry_id_for("MCL_UWB"));
    printf("mcl-uwb  header %u matches registry\n", (unsigned)MCL_UWB_TRANSPORT_ID);
    ++checked;
#endif

#if defined(MCL_AP_PROFILE_REGISTRY_PATH)
    check_profile_registry(MCL_AP_PROFILE_REGISTRY_PATH, "mcl-ap", registry_id_for("MCL_AP"));
#endif
#if defined(MCL_IP_PROFILE_REGISTRY_PATH)
    check_profile_registry(MCL_IP_PROFILE_REGISTRY_PATH, "mcl-ip", registry_id_for("MCL_IP"));
#endif
#if defined(MCL_BLE_PROFILE_REGISTRY_PATH)
    check_profile_registry(MCL_BLE_PROFILE_REGISTRY_PATH, "mcl-ble", registry_id_for("MCL_BLE"));
#endif
#if defined(MCL_UWB_PROFILE_REGISTRY_PATH)
    check_profile_registry(MCL_UWB_PROFILE_REGISTRY_PATH, "mcl-uwb", registry_id_for("MCL_UWB"));
#endif

    printf("PASS: transport registry agrees with %d binding header(s) "
           "and %d profile registry/registries\n", checked, g_checks);
    return 0;
}

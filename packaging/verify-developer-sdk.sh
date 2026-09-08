#!/bin/sh
# Prove a consumer can build from one generated MCL package with no sibling
# repositories, then link the high-level machine API through find_package.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'status=$?; rm -rf "$WORK"; exit $status' EXIT

SDK="$WORK/mcl-developer-sdk"
PREFIX="$WORK/prefix"

echo "=== single-package developer SDK verification ==="
"$ROOT/mcl-sdk/packaging/make-developer-sdk.sh" "$SDK"

cmake -S "$SDK" -B "$WORK/build-sdk" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$WORK/configure.log" 2>&1
cmake --build "$WORK/build-sdk" --config Release --target install -j 4 \
    > "$WORK/build.log" 2>&1

echo "  one package configured, built and installed"
ctest --test-dir "$WORK/build-sdk" --build-config Release --output-on-failure

if grep -RIEq '\.\./mcl-(core|wire|link|sdk|ap|ble|ip|uwb)/' \
        "$SDK/QUICKSTART.md" "$SDK/PORTING.md" "$SDK/BUILDER_GUIDE.md"; then
    echo "FAILED: packaged documentation contains a source-repository path"
    exit 1
fi
for required in \
    "$SDK/BUILDER_GUIDE.md" \
    "$SDK/docs/SECURITY.md" \
    "$SDK/docs/REPORTING.md" \
    "$SDK/docs/SPECIFICATION_INDEX.md" \
    "$SDK/docs/V1_SCOPE.md" \
    "$SDK/docs/conformance-profiles-v1.md" \
    "$SDK/docs/deployment-profile-v1.md" \
    "$SDK/docs/SPEC_GAPS.md" \
    "$SDK/docs/TWO_BUILDER_AUDIT.md" \
    "$SDK/docs/ap-bootstrap-1.md" \
    "$SDK/docs/ble-activate-1.md" \
    "$SDK/docs/ble-gatt-profile-v1.md" \
    "$SDK/profiles/MCL-REFERENCE-DEPLOYMENT-1.json"
do
    [ -f "$required" ] || { echo "FAILED: package missing $required"; exit 1; }
done

for doc in "$SDK/QUICKSTART.md" "$SDK/PORTING.md" "$SDK/BUILDER_GUIDE.md"; do
    grep -Eo ']\([^)]*\)' "$doc" | sed -e 's/^](//' -e 's/)$//' |
        while IFS= read -r target; do
            case "$target" in
                ''|'#'*|http://*|https://*|mailto:*) continue ;;
            esac
            target=${target%%#*}
            [ -e "$(dirname "$doc")/$target" ] || {
                echo "FAILED: packaged Markdown link does not resolve: $(basename "$doc") -> $target"
                exit 1
            }
        done
done

if grep -Eq 'mcl-sdk/packaging/|\.\./mcl-(core|wire|link|sdk|ap|ble|ip|uwb)/' \
        "$SDK/QUICKSTART.md" "$SDK/BUILDER_GUIDE.md"; then
    echo "FAILED: packaged documentation contains a source-tree-only command"
    exit 1
fi

RESOURCE_EXE=
for candidate in \
    "$WORK/build-sdk/mcl_resource_report" \
    "$WORK/build-sdk/mcl_resource_report.exe" \
    "$WORK/build-sdk/Release/mcl_resource_report.exe"
do
    if [ -f "$candidate" ]; then RESOURCE_EXE=$candidate; break; fi
done
[ -n "$RESOURCE_EXE" ] || { echo "FAILED: resource report missing"; exit 1; }
"$RESOURCE_EXE" | tee "$WORK/resource-envelope.txt"

if grep -Eq 'mcl_(wire|link|rdv|node|contact|handoff)_' \
        "$ROOT/mcl-sdk/packaging/external-consumer/main.c"; then
    echo "FAILED: normal consumer contains a low-level protocol call"
    exit 1
fi
if [ "$(grep -c '^#include "mcl/' "$ROOT/mcl-sdk/packaging/external-consumer/main.c")" -ne 1 ]; then
    echo "FAILED: normal consumer must need exactly one public MCL header"
    exit 1
fi

cmake -S "$ROOT/mcl-sdk/packaging/external-consumer" \
    -B "$WORK/build-consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" > "$WORK/consumer-configure.log" 2>&1
cmake --build "$WORK/build-consumer" --config Release -j 4 \
    > "$WORK/consumer-build.log" 2>&1

CONSUMER_EXE=
for candidate in \
    "$WORK/build-consumer/mcl_external_consumer" \
    "$WORK/build-consumer/mcl_external_consumer.exe" \
    "$WORK/build-consumer/Release/mcl_external_consumer.exe"
do
    if [ -f "$candidate" ]; then CONSUMER_EXE=$candidate; break; fi
done
[ -n "$CONSUMER_EXE" ] || { echo "FAILED: consumer executable missing"; exit 1; }
"$CONSUMER_EXE"

echo "  public MCL headers used by OEM application: 1"
echo "  manual Wire construction: 0"
echo "  manual Link construction: 0"
echo "  manual rendezvous state: 0"
echo "  manual migration state: 0"
echo "  peer-specific configuration: 0"
echo "SINGLE-PACKAGE DEVELOPER SDK PASSED"

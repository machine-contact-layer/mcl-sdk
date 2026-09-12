#!/bin/sh
# Assemble one self-contained source SDK from the separately governed MCL
# repositories. The output has one CMake project and no dependency-install
# order. It is generated, never maintained as a second copy of the sources.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-}

if [ -z "$OUT" ]; then
    echo "usage: $0 OUTPUT_DIRECTORY" >&2
    exit 2
fi
if [ -e "$OUT" ]; then
    echo "refusing to overwrite existing output: $OUT" >&2
    exit 2
fi

mkdir -p "$OUT/include/mcl" "$OUT/src" "$OUT/examples" "$OUT/cmake" \
    "$OUT/tests" "$OUT/docs" "$OUT/profiles"

for repo in mcl-wire mcl-link mcl-sdk mcl-ap mcl-ble mcl-ip; do
    if [ ! -d "$ROOT/$repo" ]; then
        echo "missing required repository: $ROOT/$repo" >&2
        exit 1
    fi
done

for header in \
    "$ROOT"/mcl-wire/include/mcl/*.h \
    "$ROOT"/mcl-link/include/mcl/*.h \
    "$ROOT"/mcl-sdk/include/mcl/*.h \
    "$ROOT"/mcl-ap/include/mcl/*.h \
    "$ROOT"/mcl-ble/include/mcl/*.h \
    "$ROOT"/mcl-ip/include/mcl/*.h
do
    cp "$header" "$OUT/include/mcl/"
done

for source in \
    "$ROOT"/mcl-wire/src/wire.c \
    "$ROOT"/mcl-wire/src/extension.c \
    "$ROOT"/mcl-link/src/link.c \
    "$ROOT"/mcl-link/src/contact.c \
    "$ROOT"/mcl-link/src/endpoint_rendezvous.c \
    "$ROOT"/mcl-link/src/handoff.c \
    "$ROOT"/mcl-link/src/control.c \
    "$ROOT"/mcl-link/src/negotiation.c \
    "$ROOT"/mcl-sdk/src/sdk.c \
    "$ROOT"/mcl-sdk/src/conformance.c \
    "$ROOT"/mcl-sdk/src/rendezvous.c \
    "$ROOT"/mcl-sdk/src/machine.c \
    "$ROOT"/mcl-ap/src/ap_channel.c \
    "$ROOT"/mcl-ap/src/ap_modem.c \
    "$ROOT"/mcl-ap/src/ap_listen.c \
    "$ROOT"/mcl-ble/src/ble_binding.c \
    "$ROOT"/mcl-ip/src/ip_binding.c
do
    cp "$source" "$OUT/src/"
done

cp "$ROOT/mcl-sdk/examples/base_arranged_bearer.c" "$OUT/examples/"
cp "$ROOT/mcl-sdk/examples/first_contact.c" "$OUT/examples/"
cp "$ROOT/mcl-sdk/examples/resource_report.c" "$OUT/examples/"
cp "$ROOT/mcl-sdk/tests/test_machine.c" "$OUT/tests/"
sed \
    -e 's|../mcl-core/SECURITY.md|docs/SECURITY.md|g' \
    -e 's|../mcl-ble/spec/ble-activate-1.md|docs/ble-activate-1.md|g' \
    -e 's|../mcl-core/deployments/MCL-REFERENCE-DEPLOYMENT-1.json|profiles/MCL-REFERENCE-DEPLOYMENT-1.json|g' \
    -e 's|hardware/dfr1154-autonomous-node/|PORTING.md|g' \
    -e 's|../mcl-ble/hardware/host-ble-probe/|https://github.com/machine-contact-layer/mcl-ble/tree/main/hardware/host-ble-probe/|g' \
    -e '/Maintainers working from the separately governed repositories generate that/,/^```$/d' \
    -e '/Maintainers working from all eight source repositories additionally run:/,/^```$/d' \
    -e 's|../mcl-core/conformance/|docs/|g' \
    "$ROOT/mcl-sdk/QUICKSTART.md" > "$OUT/QUICKSTART.md"
cp "$ROOT/mcl-sdk/PORTING.md" "$OUT/"
cp "$ROOT/mcl-sdk/RESOURCE_ENVELOPE.md" "$OUT/"
sed \
    -e 's|mcl-core/research/TWO_BUILDER_AUDIT.md|docs/TWO_BUILDER_AUDIT.md|g' \
    -e 's|mcl-ap/experiments/008-embedded-node/|the retained embedded-node evidence index|g' \
    -e 's|mcl-ap/spec/ap-bootstrap-1.md|docs/ap-bootstrap-1.md|g' \
    -e 's|mcl-ble/spec/ble-activate-1.md|docs/ble-activate-1.md|g' \
    -e 's|mcl-core/SECURITY.md|docs/SECURITY.md|g' \
    -e 's|mcl-link/research/mcl-s1-benchmark-round1.md|the post-v1 security research record|g' \
    -e 's|mcl-core/spec/deployment-profile-v1.md|docs/deployment-profile-v1.md|g' \
    -e 's|mcl-core/deployments/MCL-REFERENCE-DEPLOYMENT-1.json|profiles/MCL-REFERENCE-DEPLOYMENT-1.json|g' \
    -e 's|mcl-core/conformance/independent/SPEC_GAPS.md|docs/SPEC_GAPS.md|g' \
    -e 's|mcl-core/conformance/|docs/|g' \
    -e 's|mcl-core/REPORTING.md|docs/REPORTING.md|g' \
    -e 's|mcl-core/errata/|the project errata process|g' \
    -e 's|mcl-core/governance/V1_SCOPE.md|docs/V1_SCOPE.md|g' \
    -e 's|mcl-core/spec/conformance-profiles-v1.md|docs/conformance-profiles-v1.md|g' \
    -e 's|mcl-core/SPECIFICATION_INDEX.md|docs/SPECIFICATION_INDEX.md|g' \
    "$ROOT/mcl-sdk/BUILDER_GUIDE.md" > "$OUT/BUILDER_GUIDE.md"
cp "$ROOT/mcl-core/SECURITY.md" "$OUT/docs/"
cp "$ROOT/mcl-core/REPORTING.md" "$OUT/docs/"
cp "$ROOT/mcl-core/SPECIFICATION_INDEX.md" "$OUT/docs/"
cp "$ROOT/mcl-core/governance/V1_SCOPE.md" "$OUT/docs/"
cp "$ROOT/mcl-core/spec/conformance-profiles-v1.md" "$OUT/docs/"
cp "$ROOT/mcl-core/spec/deployment-profile-v1.md" "$OUT/docs/"
cp "$ROOT/mcl-core/conformance/independent/SPEC_GAPS.md" "$OUT/docs/"
cp "$ROOT/mcl-core/research/TWO_BUILDER_AUDIT.md" "$OUT/docs/"
cp "$ROOT/mcl-ap/spec/ap-bootstrap-1.md" "$OUT/docs/"
cp "$ROOT/mcl-ble/spec/ble-activate-1.md" "$OUT/docs/"
cp "$ROOT/mcl-ble/spec/ble-gatt-profile-v1.md" "$OUT/docs/"
cp "$ROOT/mcl-core/deployments/MCL-REFERENCE-DEPLOYMENT-1.json" "$OUT/profiles/"
cp "$ROOT/mcl-sdk/LICENSE" "$OUT/"
cp "$ROOT/mcl-sdk/NOTICE" "$OUT/"
cp "$ROOT/mcl-sdk/packaging/developer-sdk/CMakeLists.txt" "$OUT/CMakeLists.txt"
cp "$ROOT/mcl-sdk/packaging/developer-sdk/mcl_sdkConfig.cmake.in" "$OUT/cmake/"

python3 "$ROOT/mcl-sdk/packaging/package-docs.py" "$OUT" --root "$ROOT"

(
    cd "$OUT"
    find . -type f ! -name SHA256SUMS.txt -print | LC_ALL=C sort |
        while IFS= read -r file; do sha256sum -b "$file"; done > SHA256SUMS.txt
)

echo "MCL developer SDK assembled at $OUT"
echo "Build with: cmake -S \"$OUT\" -B BUILD_DIRECTORY"

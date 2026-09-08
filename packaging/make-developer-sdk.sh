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

mkdir -p "$OUT/include/mcl" "$OUT/src" "$OUT/examples" "$OUT/cmake"

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

cp "$ROOT/mcl-sdk/examples/first_contact.c" "$OUT/examples/"
cp "$ROOT/mcl-sdk/examples/resource_report.c" "$OUT/examples/"
cp "$ROOT/mcl-sdk/QUICKSTART.md" "$OUT/"
cp "$ROOT/mcl-sdk/PORTING.md" "$OUT/"
cp "$ROOT/mcl-sdk/RESOURCE_ENVELOPE.md" "$OUT/"
cp "$ROOT/mcl-sdk/LICENSE" "$OUT/"
cp "$ROOT/mcl-sdk/NOTICE" "$OUT/"
cp "$ROOT/mcl-sdk/packaging/developer-sdk/CMakeLists.txt" "$OUT/CMakeLists.txt"
cp "$ROOT/mcl-sdk/packaging/developer-sdk/mcl_sdkConfig.cmake.in" "$OUT/cmake/"

(
    cd "$OUT"
    find . -type f ! -name SHA256SUMS.txt -print | LC_ALL=C sort |
        while IFS= read -r file; do sha256sum -b "$file"; done > SHA256SUMS.txt
)

echo "MCL developer SDK assembled at $OUT"
echo "Build with: cmake -S \"$OUT\" -B BUILD_DIRECTORY"

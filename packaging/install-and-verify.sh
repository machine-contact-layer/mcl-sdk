#!/bin/sh
#
# Release gate item 13: prove the SDK is installable and externally consumable.
#
# Installs mcl-wire, mcl-link and mcl-sdk into a throwaway prefix, then builds
# packaging/external-consumer against THAT PREFIX ONLY and runs it.
#
# WHAT MAKES THIS A REAL TEST
#
# The consumer project contains no path to any MCL repository. It resolves
# everything through find_package on CMAKE_PREFIX_PATH. So if any public header
# is missing from the install, if any exported target names something that was
# never installed, or if a config file fails to find its dependencies, the
# consumer fails to configure or link -- which is the result being measured.
#
# The prefix is created fresh each run. Reusing one would let a stale artifact
# from a previous install satisfy a dependency this build forgot to declare.

set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${MCL_PACKAGING_WORKDIR:-/tmp/mcl-packaging}
PREFIX="$WORK/prefix"

echo "=== MCL install and external-consumer verification ==="
echo "root:   $ROOT"
echo "prefix: $PREFIX"
echo

rm -rf "$WORK"
mkdir -p "$PREFIX"

build_and_install() {
    name=$1
    src=$2
    echo "--- installing $name"
    cmake -S "$src" -B "$WORK/build-$name" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DCMAKE_PREFIX_PATH="$PREFIX" \
          -DMCL_WIRE_BUILD_TESTS=OFF \
          -DMCL_WIRE_BUILD_BENCHMARKS=OFF \
          -DMCL_LINK_BUILD_TESTS=OFF \
          -DMCL_SDK_BUILD_TESTS=OFF > "$WORK/$name-configure.log" 2>&1 || {
        echo "CONFIGURE FAILED for $name"; tail -30 "$WORK/$name-configure.log"; exit 1; }
    cmake --build "$WORK/build-$name" --config Release --target install -j 4 \
          > "$WORK/$name-build.log" 2>&1 || {
        echo "BUILD/INSTALL FAILED for $name"; tail -30 "$WORK/$name-build.log"; exit 1; }
    echo "    ok"
}

# Order matters: the SDK's find_package(mcl_wire) must succeed, which means
# wire and link have to be in the prefix first. That ordering requirement is
# itself part of what is being verified -- if the SDK could configure without
# them it would be falling back to the sibling sources.
build_and_install mcl_wire "$ROOT/mcl-wire"
build_and_install mcl_link "$ROOT/mcl-link"
build_and_install mcl_sdk  "$ROOT/mcl-sdk"

echo
echo "--- checking the SDK really used the installed dependencies"
if grep -q "using installed mcl_wire and mcl_link" "$WORK/mcl_sdk-configure.log"; then
    echo "    ok: SDK resolved its dependencies through find_package"
else
    echo "FAILED: the SDK fell back to sibling sources, so the exported package"
    echo "        would reference targets that were never installed."
    grep -i "mcl-sdk:" "$WORK/mcl_sdk-configure.log" || true
    exit 1
fi

echo
echo "--- installed public headers"
find "$PREFIX/include/mcl" -name '*.h' | sed "s|$PREFIX/|      |" | sort

echo
echo "--- installed package configs"
find "$PREFIX" -name '*Config.cmake' | sed "s|$PREFIX/|      |" | sort

echo
echo "--- building the external consumer against the prefix ONLY"
cmake -S "$ROOT/mcl-sdk/packaging/external-consumer" -B "$WORK/build-consumer" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$PREFIX" > "$WORK/consumer-configure.log" 2>&1 || {
    echo "CONSUMER CONFIGURE FAILED"; tail -40 "$WORK/consumer-configure.log"; exit 1; }
cmake --build "$WORK/build-consumer" --config Release -j 4 > "$WORK/consumer-build.log" 2>&1 || {
    echo "CONSUMER BUILD FAILED"; tail -40 "$WORK/consumer-build.log"; exit 1; }
echo "    ok"

echo
echo "--- running the external consumer"
CONSUMER_EXE=
for candidate in \
    "$WORK/build-consumer/mcl_external_consumer" \
    "$WORK/build-consumer/mcl_external_consumer.exe" \
    "$WORK/build-consumer/Release/mcl_external_consumer.exe"
do
    if [ -f "$candidate" ]; then
        CONSUMER_EXE=$candidate
        break
    fi
done
if [ -z "$CONSUMER_EXE" ]; then
    echo "FAILED: built external consumer executable was not found"
    find "$WORK/build-consumer" -maxdepth 2 -type f | sed 's/^/      /'
    exit 1
fi
"$CONSUMER_EXE"

echo
echo "--- negative control: the consumer MUST fail without the prefix"
# If this succeeded, find_package would be resolving MCL from somewhere other
# than the install -- a system copy, a stale cache, or the source tree -- and
# the positive result above would mean nothing.
if cmake -S "$ROOT/mcl-sdk/packaging/external-consumer" -B "$WORK/build-neg" \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_PREFIX_PATH="$WORK/empty" > "$WORK/neg.log" 2>&1; then
    echo "FAILED: the consumer configured with no MCL in the prefix path."
    echo "        find_package resolved MCL from somewhere unintended."
    exit 1
else
    echo "    ok: refused to configure, as it must"
fi

echo
echo "=== PACKAGING VERIFICATION PASSED ==="
echo
echo "What this does NOT establish:"
echo "  - It uses the compiler selected by CMake; run it under each release"
echo "    toolchain rather than treating one successful compiler as all of them."
echo "  - Nothing about the protocol. It measures whether the library can be"
echo "    consumed, not whether it is correct."

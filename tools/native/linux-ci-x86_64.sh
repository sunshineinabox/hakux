#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# Build, test and package the AMD64 KVM core with the DSP JIT.
# Release builds use the Ubuntu 22.04 sysroot in the build image.
# Set HAKUX_SYSROOT= to test with host libraries in separate directories,
# without producing a release package.
# Usage: sh tools/native/linux-ci-x86_64.sh [version]
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
version=${1:-dev}
sysroot=${HAKUX_SYSROOT-/opt/sysroot-jammy}
if [ -n "$sysroot" ]; then
    if [ ! -d "$sysroot/usr/include" ]; then
        echo "linux-ci-x86_64.sh: no sysroot at $sysroot (HAKUX_SYSROOT= builds and tests against the host's libraries, without the package)" >&2
        exit 1
    fi
    export HAKUX_BUILD="${HAKUX_BUILD:-$repo/build/linux-x86_64}"
else
    export HAKUX_BUILD="${HAKUX_BUILD:-$repo/build/linux-x86_64-host}"
fi
# CMake caches the sysroot and compiler paths; refuse a mismatched build.
cache=$HAKUX_BUILD/CMakeCache.txt
if [ -f "$cache" ] && [ "$(sed -n 's/^CMAKE_SYSROOT:[A-Z]*=//p' "$cache")" != "$sysroot" ]; then
    echo "linux-ci-x86_64.sh: $HAKUX_BUILD was configured with another sysroot; remove it or set HAKUX_BUILD" >&2
    exit 1
fi
export HAKUX_DEPS="${HAKUX_DEPS:-$repo/build/deps}"
export HAKUX_VERSION="$version"
jobs=${JOBS:-4}

python3 tools/native/linux-deps.py --dest "$HAKUX_DEPS"
if [ -n "$sysroot" ]; then
    set -- "-DCMAKE_SYSROOT=$sysroot"
else
    set --
fi
sh tools/native/linux-build.sh "$@"
if ! grep -q '^HAKUX_DSP_JIT:BOOL=ON$' "$HAKUX_BUILD/CMakeCache.txt"; then
    echo "linux-ci-x86_64.sh: the build has no DSP JIT" >&2
    exit 1
fi
# Check KVM and DSP JIT again for the CI log.
"$HAKUX_BUILD/kvm-smoke"
"$HAKUX_BUILD/linux-smoke" | grep -E '^(PASS|FAIL)'
"$HAKUX_BUILD/apu-jit-smoke" | grep -E '^(PASS|FAIL)'

tests=$repo/build/hle-tests-x86_64
[ -n "$sysroot" ] || tests=$tests-host
cmake -S tests/hle -B "$tests" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" \
    -DNC_DEPS="$HAKUX_DEPS" > /dev/null
cmake --build "$tests" -j "$jobs"
ctest --test-dir "$tests" --output-on-failure
python3 tools/native/statelock.py
if [ -z "$sysroot" ]; then
    echo "PASS: linux-ci-x86_64 (the host's libraries, no package)"
    exit 0
fi
sh tools/native/linux-package.sh "$version"
python3 tools/native/linux-verify.py "$version"
echo "PASS: linux-ci-x86_64"

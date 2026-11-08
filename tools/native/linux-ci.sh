#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# The Linux release build and its checks, in the hakux-linux image
# (Dockerfile.linux), as CI runs them (.github/workflows/linux.yml): the
# pinned sources, the core and its tests (linux-build.sh), the package
# (linux-package.sh) and its checks (linux-verify.py), the pure HLE tests,
# every shader they generate compiled by glslang (built from the pinned
# source) for Vulkan 1.3, and the savestate lock. On a clean checkout: the
# package is the commit's.
#   sh tools/native/linux-ci.sh <version>
set -eu

version=${1:?usage: linux-ci.sh <version>}
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
export HAKUX_DEPS="${HAKUX_DEPS:-$repo/build/deps}"
export HAKUX_BUILD="${HAKUX_BUILD:-$repo/build/linux}"
export HAKUX_VERSION="$version"
jobs=${JOBS:-4}

python3 tools/native/linux-deps.py --dest "$HAKUX_DEPS"
sh tools/native/linux-build.sh
sh tools/native/linux-package.sh "$version"
python3 tools/native/linux-verify.py "$version"

tests=$repo/build/hle-tests
cmake -S tests/hle -B "$tests" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" \
    -DNC_DEPS="$HAKUX_DEPS" > /dev/null
cmake --build "$tests" -j "$jobs"
ctest --test-dir "$tests" --output-on-failure

glslang=$repo/build/glslang-tool
cmake -S "$HAKUX_DEPS/glslang" -B "$glslang" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF -DBUILD_SHARED_LIBS=OFF > /dev/null
cmake --build "$glslang" --target glslang-standalone -j "$jobs"
shaders=$repo/build/hle-shaders
rm -rf "$shaders"
mkdir -p "$shaders"
for t in test_psh test_ffhw test_vsh test_ffgen; do
    HLE_TEST_DUMP="$shaders" "$tests/$t" > /dev/null
done
count=0
for f in "$shaders"/*.vert "$shaders"/*.frag; do
    [ -f "$f" ] || continue
    "$glslang/StandAlone/glslang" --target-env vulkan1.3 -o /dev/null "$f"
    count=$((count + 1))
done
[ "$count" -gt 0 ]
echo "PASS: $count generated shaders compile for Vulkan 1.3"

python3 tools/native/statelock.py
python3 tools/native/statelock.py --sizes --build "$HAKUX_BUILD"
echo "PASS: linux-ci $version"

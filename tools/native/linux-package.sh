#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# Package a committed libretro build with its metadata and licences, and the
# standalone front end (hakux), stripped.
# Run after linux-build.sh in the same image from a clean checkout.
set -eu

version=${1:?usage: linux-package.sh <version>}
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
build=${HAKUX_BUILD:-$repo/build/linux}
arch=$(uname -m)
case "$arch" in
    aarch64|x86_64) ;;
    *) echo "linux-package.sh: unsupported architecture: $arch" >&2; exit 1 ;;
esac
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "linux-package.sh: the checkout has changes; package a commit" >&2
    exit 1
fi
if [ ! -f "$build/CMakeCache.txt" ]; then
    echo "linux-package.sh: missing $build/CMakeCache.txt; build with linux-build.sh first" >&2
    exit 1
fi

out=$repo/build/package
name=hakux-$version
rm -rf "${out:?}/$name"
mkdir -p "$out/$name/lib/libretro" "$out/$name/share/licenses/hakux"
"${STRIP:-llvm-strip-19}" -o "$out/$name/lib/libretro/hakux_libretro.so" "$build/hakux_libretro.so"
sed "s/^display_version = .*/display_version = \"$version\"/" native/libretro/hakux_libretro.info \
    > "$out/$name/lib/libretro/hakux_libretro.info"
cp LICENSE COPYING licenses/* "$out/$name/share/licenses/hakux/"
# Record the glibc requirement from the binary's ELF symbol versions.
# Host builds may require newer libraries than the release sysroot.
glibc=$("${READELF:-readelf}" --version-info "$out/$name/lib/libretro/hakux_libretro.so" |
    grep -o 'GLIBC_2\.[0-9.]*' | sort -u -t. -k2,2n -k3,3n | tail -n1)
glibc=${glibc#GLIBC_}
if [ -z "$glibc" ]; then
    echo "linux-package.sh: no glibc version in the core's symbol versions" >&2
    exit 1
fi
dsp_jit=off
if grep -q '^HAKUX_DSP_JIT:BOOL=ON$' "$build/CMakeCache.txt"; then
    dsp_jit=on
    cp -R "$build/dsp-jit/licenses" "$out/$name/share/licenses/hakux/dsp-jit"
fi
{
    echo "hakuX $version"
    echo "source $(git rev-parse HEAD) (https://github.com/portare-ch/hakuX-lr)"
    echo "compiler $("${CC:-clang}" --version | head -1)"
    echo "architecture $arch"
    echo "DSP JIT $dsp_jit"
    if [ "$dsp_jit" = on ]; then
        cat "$build/dsp-jit/licenses/BUILD-INFO"
    fi
    echo "runtime libz, libstdc++, libm, libgcc_s, glibc $glibc+ ($arch); libvulkan.so.1 loaded at run time"
    if [ "$arch" = x86_64 ]; then
        echo "CPU KVM; Linux 5.16+, /dev/kvm, TSC scaling and UMIP required"
    fi
    echo "dependencies (tools/native/linux-deps.json):"
    cat tools/native/linux-deps.json
} > "$out/$name/BUILD-INFO"
"${STRIP:-llvm-strip-19}" -o "$out/$name-standalone-$arch" "$build/hakux"
(cd "$out" && sha256sum "$name-standalone-$arch" > "$name-standalone-$arch.sha256")
tar -C "$out" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git log -1 --format=%ct)" -cf - "$name" | gzip -n > "$out/$name-$arch.tar.gz"
cd "$out" && sha256sum "$name-$arch.tar.gz"

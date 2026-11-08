# Android build (upstream)

Upstream Android build notes. Android builds are not validated by this fork.

The app packages native libraries from `app/src/main/jniLibs`. Build the
core first, copy its libraries there, then build the APK.

## Toolchain

- Android SDK 36, Build Tools 36.1.0
- NDK 29.0.14206865
- CMake 3.20 or later, Ninja
- JDK 21

## Sources

- The FEX fork ([rfandango/FEX](https://github.com/rfandango/FEX), branch
  `hakuX`) lives in `external/fex`. Initialize it and its dependencies:

  ```
  git submodule update --init external/fex
  git -C external/fex submodule update --init External/fmt External/range-v3 \
    External/rpmalloc External/unordered_dense External/xxhash Source/Common/cpp-optparse
  ```

  Set CMake's `HAKUX_FEX_DIR` to use another checkout.
- The mods and cheats the APK ships
  ([portare-ch/hakux-lr-patches](https://github.com/portare-ch/hakux-lr-patches))
  live in `patches`; the build stops without them:

  ```
  git submodule update --init patches
  ```
- The core's third-party sources, outside the repository (CMake `NC_DEPS`):

| Path in `NC_DEPS` | Revision |
|---|---|
| `volk` | 1.4.304 |
| `glslang` | 16.1.0 |
| `spirv_reflect` | vulkan-sdk-1.4.328.0 |
| `vma` | v3.3.0 |
| `SDL3` | release-3.4.18 |
| `adrenotools` | 8fae8ce, with `tools/native/adrenotools-kgsl-context-priority.patch` applied (`git apply`): the KGSL draw context priority hook |
| `root/subprojects/nv2a_vsh_cpu` | 561fe80 |
| `libchdr` (optional: CHD images) | 47d7941 |
| `libslirp` | 4.7.0 (26be815b), unmodified: the network's NAT (`native/net/nat.c`) |

## Steps

From the repository root:

```
cmake -S native -B build/native/core -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS=<native-deps>
ninja -C build/native/core hakuxcore
bash tools/native/app-libs.sh
cd android
./gradlew assembleDebug
```

`app-libs.sh` strips and copies the core, SDL3 and adrenotools libraries to
`app/src/main/jniLibs`. Run it after every core build to avoid packaging
stale libraries.

A release build (`assembleRelease`) is signed with the keystore that
`key.properties` names (`storeFile`, `storePassword`, `keyAlias`,
`keyPassword`).

## Host tests

The HLE layer's pure modules have host tests (any C compiler with CMake).
Use the dependency directory from the native build:

```
cmake -S tests/hle -B build/hle-tests -G Ninja -DNC_DEPS=<native-deps>
ninja -C build/hle-tests
ctest --test-dir build/hle-tests --output-on-failure
```

## Networking

Xbox Live (Insignia) mode forwards the console's UDP 3074 through the
device's UDP 3074 and requests a router forward through UPnP. For Open NAT,
enable UPnP on the router or forward UDP 3074 to the device manually. Only
one console behind the same router can use external UDP 3074.

The native options `natport=`, `upnp=` and `upnplease=` use the same backend
as the libretro core; see [Insignia NAT](../native/README.linux.md#insignia-nat)
for defaults and behavior.

## Debug builds

Debug builds also carry the Khronos validation layer, which Android loads
on request (see [app/src/debug/jniLibs/arm64-v8a/README.md](app/src/debug/jniLibs/arm64-v8a/README.md)).

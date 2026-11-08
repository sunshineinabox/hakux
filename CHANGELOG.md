# Changelog

## Unreleased

## linux-v0.25.2-rf.1 (RetroForge)

hakuX-lr 0.25.2 with the standalone Linux front end back,
for RetroForge.

### What's new

- **Standalone `hakux`** — Play without RetroArch: fullscreen through SDL3 and
  Vulkan, sound through SDL audio, every gamepad as player 1, the title's
  rumble on all of them. Options are the core's plus the picture's shape,
  FXAA and upscaling. Released as `hakux-0.25.2-rf.1-standalone-aarch64`.
- **Quick savestate** — `states=DIR` keeps one state per game; F5 loads it,
  Shift+F5 saves it.
- **Clean exit** — SIGTERM and Ctrl+Q stop the core and sync the hard disk
  before the process ends. A title's relaunch and a state load start hakux
  again in the same process.
- **Its own log file** — The core's statistics (about 17 lines a second in a
  game) go to `hakux.log` in the data directory, not the caller's stderr (a
  session's journal); `HAKUX_LOG=stderr` keeps them there.

## linux-v0.25.2 (Linux, aarch64 and x86-64, and Android)

- **Cheats** — Off by default in RetroArch and the Android app. Applied
  cheats, patches, mods and uncapped frame rates disable Xbox Live until
  the game restarts. System link remains available.

## linux-v0.25.1 (Linux, aarch64 and x86-64, and Android)

- **Frame rate** — The core skips unchanged frames by default so FPS
  counters show the game's frame rate. Enable **Repeat Frames** for
  RetroArch recording, streaming or black-frame insertion.

## linux-v0.25.0 (Linux, aarch64 and x86-64, and Android)

- **Insignia account recovery** (#185) — Under **Xbox Live**, select
  **Recover an Insignia account** and enter your gamertag and the 16-digit
  recovery number revealed on Insignia's website. Recovery refreshes the
  matching account slot or uses a free slot, preserves other accounts, and
  resumes a failed local save without repeating the recovery request.
  Recovery and persistence after restart were verified on Linux x86-64.

## linux-v0.24.0 (Linux, aarch64 and x86-64, and Android)

Set up Insignia from the bundled dashboard. In RetroArch, load the core
and select **Start Core**. On Android, the app's Xbox Live setup wizard
opens the same dashboard.

- **Insignia** (experimental, #192) — Under **Xbox Live**, select
  **Register this console**, then **Create an Insignia account** with the
  subscription code from Insignia. Registration and signup work without
  Microsoft's dashboard or the separate Setup Assistant. On-console
  account recovery is still pending.
- **Dashboard** (#188, #192) — At boot, the core installs the bundled
  dashboard at `C:\xboxdash.xbe` and `C:\xodash\xonlinedash.xbe`, replacing
  any dashboard executables at those paths. It keeps other files on C:.
- **Network** (#84, #192) — One switch enables Xbox Live and system link
  together and defaults to On. System link consoles negotiate separate
  IP addresses. Older Android network modes migrate to On, and explicit
  Off stays off.
- **Console HDD keys** (#191) — New consoles store a random HDD key in
  EEPROM. Existing consoles keep their key. First boots with saves or
  downloadable content already on E: use the legacy key. Android
  re-signs saves transferred between consoles.
- **Homebrew and EEPROM** (#186, #187) — Honor NV2A DMA base addresses so
  nxdk programs such as the dashboard can render. Persist guest EEPROM
  writes over SMBus, with RetroArch settings taking priority at boot.
- **Multi-game discs** (#188, #190) — Map `D:` to a game's disc subfolder
  when it launches from there. The Star Wars - The Clone Wars + Tetris Worlds
  disc reaches its menu and can launch both games.

Downloads include Linux ARM64 and x86-64 builds and an experimental
RetroArch core for Android ARM64.

## linux-v0.23.0 (Linux, aarch64 and x86-64, and Android)

A compatibility-focused release with fixes for Xbox audio, rendering,
memory, file I/O and game loading. It also adds XBE patches through
RetroArch's Cheats menu.

### Graphics

- **Full-height framebuffers** (#180) — Thrillville and Wallace & Gromit:
  The Curse of the Were-Rabbit now render their full 640 x 480 images
  instead of losing the bottom of the frame. Adjacent allocations are
  treated as one physical run only when they share a caller, preserving
  the oversized-target guard needed by NBA Inside Drive 2003.
- **NV2A 2D blits** (#182) — Respect source/destination pitch and GPU tile
  boundaries, including a partial last row. WWE WrestleMania 21 reaches
  its main menu with audio instead of corrupting its own push buffer.
  A separate crash after further menu input remains under investigation.

### Kernel and game compatibility

- **Unbuffered overlapped writes** (#179) — Return `STATUS_PENDING` for
  asynchronous hard-disk transfers without an explicit event. Sniper
  Elite now reaches player-name entry; Rogue Trooper reaches its tutorial
  with sound. Buffered writes retain immediate completion.
- **More contiguous allocations** (#175) — Raise the limit from 4,096 to
  32,768 blocks. Wings of War loads its first mission, SNK vs. Capcom:
  SVC Chaos reaches a fight, and Shrek Super Party reaches its game
  selection. With the DSP fix below, The Simpsons: Road Rage reaches
  playable races.
- **Directory wildcard matching** (#176) — Correct `*.` and `*.*`
  matching for names without extensions. Trivial Pursuit: Unhinged
  now finds its language directories and reaches profile selection.
- **More XBE sections** (#174) — Support up to 1,024 sections instead of
  256. Spikeout: Battle Street loads and reaches its player-name screen.
- **Nested exceptions** (#177) — Raise the per-thread exception limit
  from 8 to 64, fixing XIII's early exception overflow. Older savestates
  remain supported.

### Audio and stability

- **DSP56300 JIT loop returns** (#178) — Handle subroutine returns at
  the end of a `DO` loop correctly. Fixes DMA stalls, missing audio and
  hangs in games including WWE Raw 2, Oddworld: Munch's Oddysee,
  Project: Snowblind, Genma Onimusha, The Simpsons: Road Rage,
  RollerCoaster Tycoon and Whiplash. The DSP JIT regression suite
  passed 1,053 tests on x86-64.

### Patches and tools

- **XBE patches through RetroArch cheats** (#169) — Apply widescreen
  and other executable patches from `.cht` entries, matched to the
  original XBE by CRC-32 and size. Use `tools/mods/xbe-to-cht.py`
  for IPS/BPS or patched-XBE conversion; enable the cheat, apply
  changes and restart. Ordinary memory cheats remain supported.
- **Community patch repository** (#173) — Keep Android mods and
  converted RetroArch patches in
  [hakux-lr-patches](https://github.com/portare-ch/hakux-lr-patches).
  The Android app uses the `patches` submodule; initialize it when
  building the app. Six 60 FPS mods have been converted to `.cht`
  entries; refresh-rate-dependent 120 FPS mods remain Android-only.

### Testing

The fixes above were tested primarily with RetroArch 1.22.2 on x86-64
Linux with KVM. Linux CI builds and tests both architectures; the Android
ARM64 core remains experimental and has not been tested on a device.
Game progress described above is not a guarantee of full playability.

## linux-v0.22.0 (Linux, aarch64 and x86-64, and Android)

### Graphics

- **NV2A logic operations** — Apply framebuffer logic operations on
  Vulkan GPUs that support them. Logic operations take precedence over
  blending, and invalid register values leave the previous state intact.
- **GL-style texture clamping** — Keep the edge texel with nearest
  filtering and blend with the border color with linear filtering.
  Mip selection still uses the original texture gradients, and fractional
  minimum and maximum LOD limits are now applied with mipmapping.
  Draws that do not use GL-style clamp keep their existing shader cache keys.

These paths were checked with shader tests and Linux smoke tests.
Their use in retail games and output on Xbox hardware remain unverified.

### Licensing

- Clarify which hakuX-lr files are available under GPL-2.0-or-later and
  document upstream hakuX, xemu, and the combined core's licensing in
  `LICENSE`.

The Android ARM64 core remains experimental and targets Android 9 or newer.

## linux-v0.21.0 (Linux, aarch64 and x86-64)

This release brings the measured Xbox voice processor behavior from xemu
to hakuX-lr, improves video overlays and NV2A rendering, and makes the
x86-64 core load on Ubuntu 22.04.

### What's new

- **More accurate Xbox audio** — Port raddad772's work from
  [xemu #3105](https://github.com/xemu-project/xemu/pull/3105). HRTF routing,
  filters, attenuation, envelopes, LFOs and multipass mixing follow retail
  Xbox measurements. The hardware resampler is now the default. Sinc and
  linear remain selectable under **Audio Resampler**. In the
  [retail console probe comparison](https://github.com/portare-ch/hakuX-lr/pull/166#issuecomment-6712645972),
  exact matches rose from 237 to 587 of 770 rows. Envelope and LFO timing
  still differ in some cases.
- **NV2A rendering** — Add solid, wireframe and point fill modes for front
  and back faces, with line and polygon smoothing where Vulkan supports
  them. **Combiner Precision** offers optional 9-bit stage rounding.
  Accurate is the default on x86-64 Linux and Fast on ARM64.
- **Ubuntu 22.04 on x86-64** — Lower the core's glibc and libstdc++ runtime
  requirements. KVM still needs Linux 5.16 or newer, so Ubuntu 22.04 users
  on the original 5.15 kernel need an HWE or newer kernel.
- **ISO to CHD tool** — Use `python3 tools/iso_to_chd.py Game.iso` to repack
  a Redump ISO as XISO before DVD CHD compression. This requires
  `xdvdfs` and `chdman` and preserves the input file.

### Fixes

- Scale and crop PVIDEO overlays correctly and apply their color keys
  against the original framebuffer. This fixes clipped video in titles
  including SpyHunter: Nowhere to Run, Jedi Academy and Worms 4: Mayhem.
- Preserve voice page addresses when games write output-buffer SGEs and
  handle unusual multipass layouts without asserting.
- Restore voice modulation, ramps and filter history when loading internal
  APU state.

Original fork code marked `GPL-2.0-or-later` is available under those terms.
The combined core is distributed under `GPL-3.0-or-later`.

## linux-v0.20.0 (Linux, aarch64 and x86-64, and Android)

### What's new

- **ETC2 textures on GPUs without BC** — Re-encode DXT textures as ETC2
  instead of expanding to RGBA where ETC2 is supported. Voodoo Vince's
  first-level textures drop from about 125 MB to 21 MB on a 1 GB RG DS Plus.
  `HAKUX_HLE_DXT_ETC2=0` disables this; `=1` also enables it on GPUs with BC.
  Android uses `debug.hakux.dxt_etc2`. Volume textures still expand to RGBA.
- **Texture-memory budget** — Evict older cached textures above 96 MiB on
  hosts with at most 1.5 GiB of RAM. Other hosts have no default limit.
  Set `HAKUX_HLE_TEX_BUDGET_MB` (`debug.hakux.tex_budget_mb` on Android), or
  use `0` to disable the budget. Textures needed by the current draw are kept.
- **Compatibility list** — Link the [game compatibility list](https://hakuxlr.portareos.org/)
  from the README.

### Fixes

- Reserve the top 128 KiB of RAM at boot, preventing early heap-guard faults
  in Conflict: Desert Storm, Desert Storm II, and The Great Escape.
- Seed the guest page directory and support RAM aliases created from its
  4 MiB entries, fixing Hitman 2's early heap accesses. Preserve page
  ownership and dirty tracking when alias tracking fills.
- Map a zeroed page above each kernel stack, fixing reads past the stack
  top in Dave Mirra Freestyle BMX 2 and Tomb Raider: Legend.
- Build `vm-alias-test` before running CTest in clean Linux builds.

A separate intermittent Tomb Raider: Legend heap crash remains under investigation.

## linux-v0.19.2 (Linux, aarch64 and x86-64, and Android)

### Fixes

- **Fix memory leaks after failed allocations** — `NtAllocateVirtualMemory`
  now rolls back failed commits and removes reservations created by the
  failed call. Games can retry smaller heap allocations without losing RAM.
  Fixes Jedi Academy's crash after New Game (#92) and loading failures in
  Aeon Flux, BloodRayne 2, Beat Down, and other titles. On AMD64, 12 of 13
  affected games now reach gameplay; Cabela's Outdoor Adventures reaches
  its gameplay options screen without crashing. Repeated failure logs are
  rate-limited.

## linux-v0.19.1 (Linux, aarch64 and x86-64, and Android)

3D sound is stereo again, and a game that polls the system time no longer
enters the kernel for each reading.

### Fixes

- **3D sound by ear** — A 3D voice's four HRTF outputs carry the left ear
  (the first two) and the right ear (the last two), as the game's DSP image
  mixes them. hakuX took the left ear from the even outputs, so with
  DirectSound's submix table each ear reached both sides and positional
  sound was mono: The Thing's gameplay audio had L = R exactly. The Full
  DSP path also dropped a mono 3D voice's right ear. Every game with 3D
  sound is affected; The Thing's gameplay now has its stereo width back,
  at the same level and balance.
- **System time without a kernel call** — `KeQuerySystemTime` now runs in
  guest code and reads `KeSystemTime`, as on the Xbox, in its 1 ms steps.
  The Thing's intro polls it about 140,000 times a second on AMD64 and over
  400,000 on the Nova, each poll a kernel entry; its kernel entries fell to
  about 2,100 a second on AMD64 and 14,200 on the Nova, at the same frame
  rates. `NC_NO_FAST=1` brings the kernel call back for debugging.
- **Android core link** — The core for RetroArch on Android links AAudio,
  which the Communicator needs; without it the core didn't link. 0.19.0's
  Android zip was already built with this fix.

### Testing

The Linux builds pass CI's checks on both architectures; the `kernel-io`
guest test now also checks the system time. The APU's stereo tests use
DirectSound's real submix table, with new checks that a voice heard only by
the left ear stays on the left. The Thing ran with the new system time on
AMD64 (KVM, RADV) and on the Retroid Pocket Nova (PortareOS, RetroArch) at
the same frame rates as before. The Android core builds and packages with
NDK r29 but hasn't run on an Android device.

Savestates load in both directions; a state saved by an older build keeps
the kernel call for the system time until the next fresh boot.

Implementations: [3D sound by ear](https://github.com/portare-ch/hakuX-lr/pull/91),
[system time in guest code](https://github.com/portare-ch/hakuX-lr/pull/89),
[Android AAudio link](https://github.com/portare-ch/hakuX-lr/pull/88).

## linux-v0.19.0 (Linux, aarch64 and x86-64, and Android)

The Xbox kernel is complete: every one of its 337 functions is implemented,
so no game stops on a missing one any more. Threads woken by another now run
first, as on the Xbox. Also the first core for RetroArch on Android
(experimental) and the fixes from 0.18.1-exp.1 and exp.2.

### What's new

- **Every kernel function** — hakuX implemented 207 of the Xbox kernel's
  337 functions; a game that called one of the other 130 stopped with
  `kunimpl`, which the log names. All 337 are there now: the I/O manager's
  driver model (IRPs, a game's own drivers with their StartIo and
  completion routines), I/O completion ports and kernel queues, device
  queues, directory objects, thread notify routines, interlocked lists, the
  runtime library's strings, numbers and `sprintf`, exceptions and stack
  walks, and the rest. NBA Inside Drive 2003's memory unit driver and the
  games that use completion ports had imports this kernel didn't have.
  Where the Xbox's behaviour isn't fully known or needed, the function is a
  stub that logs its first call; `native/kernel/STUBS.md` lists them.
- **Threads woken by another run first** — On the Xbox, a thread released
  from a wait gets a priority boost (the releaser's increment: 1 for an
  event set or a semaphore released), so it runs before the thread that
  woke it. hakuX ignored the boost, and the woken thread waited until the
  waker blocked or used up its 20 ms time slice. Games hand work between
  threads this way thousands of times a minute (an event set with that
  increment, in each game measured); they now take turns as on
  the console.
- **RetroArch on Android (experimental, untested)** —
  `hakux-0.19.0-android-arm64.zip` holds the core for RetroArch on 64-bit
  Android 9 or newer with a Vulkan 1.2 or newer GPU driver. It hasn't run on
  an Android device yet. If you try it, please report whether a game
  starts, with your device, GPU, Android version and the log (`adb logcat`,
  tags starting with `hakuX`). To install, set **Settings → Drivers →
  Video** to **vulkan** and restart RetroArch, then choose **Load Core →
  Install or Restore a Core** and select `hakux_libretro_android.so`; the
  [README](https://github.com/portare-ch/hakuX-lr/blob/linux-v0.19.0/README.md#play-on-android)
  has the details. The DSP JIT is Linux-only, so **Audio Processing**
  defaults to **Fast** on Android.

### Fixes

- **Device I/O requests** — A device control request sent through
  `IoSynchronousDeviceIoControlRequest` got zeros and success whatever it
  asked, so a game asking a drive's geometry got 0 bytes per sector. It now
  goes to the device, which answers as the same request through a file
  does, and the disc reports its geometry.
- **Small render targets read by texel** — At a render scale above 1x, a
  small render-to-texture target (kept at 1x) that a game reads by texel
  was read from part of its area, stretched. Reported for Sega GT 2002's
  car skins. From upstream hakuX.
- **Arm Mali drivers** — From upstream hakuX: on Arm's Mali-G57 driver,
  W-buffered draws use the rasterizer's depth, avoiding a GPU hang
  (Morrowind). On Arm's drivers before r40, timeline waits poll the GPU's
  counter instead of the driver's wait, which lost the device. On Arm's
  Mali-G52 driver, presents skip the CPU wait and GPU timestamps are off.
- **Vulkan 1.3 GPUs without descriptor indexing** — The core no longer asks
  for descriptor-indexing features it doesn't use, which a Vulkan 1.3
  driver without them refused, so the core couldn't start (ROCKNIX's
  S922X, Mali-G52). Before creating the device, the core logs the GPU, its
  driver and each feature it asks for that the GPU lacks.

### Testing

The Linux builds pass CI's checks on both architectures, among them a new
guest test (`kernel-io`) that drives the kernel's queues, completion ports, a
game's own driver, IRPs to the disc and the priority boosts, on ARM64 (FEX)
and AMD64 (KVM). On the Retroid Pocket Nova (PortareOS, RetroArch, on
screen, 70 s each without input, so title screens and attract modes),
Burnout 3, Dead or Alive 3, Halo, NBA Inside Drive 2003 and RalliSport
Challenge ran at the same frame rates with the priority boosts as without
(59.6, 59.6, 29.8, 30.0 and 30.0 fps), Halo's slowest 5 s at 29.8 fps with
them against 29.2 without, and nothing stopped. On AMD64 (KVM, RADV) NBA
Inside Drive 2003, Burnout 3 and Dead or Alive 3 ran at their frame rates
too. The Android core builds and packages cleanly with the NDK, but hasn't
run on an Android device.

Savestates from 0.18 load: their threads' priorities become the threads'
base priorities.

Implementations: [every kernel function](https://github.com/portare-ch/hakuX-lr/pull/86),
[priority boosts and device I/O requests](https://github.com/portare-ch/hakuX-lr/pull/87),
[Android core](https://github.com/portare-ch/hakuX-lr/pull/81),
[upstream fixes](https://github.com/portare-ch/hakuX-lr/pull/80),
[Vulkan 1.3 without descriptor indexing](https://github.com/portare-ch/hakuX-lr/pull/79).

## linux-v0.18.1-exp.2 (Linux, aarch64 and x86-64, and Android, experimental)

A first build for RetroArch on Android, untested on devices so far, and
upstream hakuX's latest fixes. It's a pre-release; 0.18.0 stays the latest
stable release.

### What's new

- **RetroArch on Android (experimental, untested)** —
  `hakux-0.18.1-exp.2-android-arm64.zip` holds the core for RetroArch on
  64-bit Android 9 or newer with a Vulkan 1.2 or newer GPU driver. It hasn't
  run on an Android device yet. If you try it, please report whether a game
  starts, with your device, GPU, Android version and the log (`adb logcat`,
  tags starting with `hakuX`). To install, set **Settings → Drivers →
  Video** to **vulkan** and restart RetroArch, then choose **Load Core →
  Install or Restore a Core** and select `hakux_libretro_android.so`; the
  [README](https://github.com/portare-ch/hakuX-lr/blob/linux-v0.18.1-exp.2/README.md#play-on-android)
  has the details. The DSP JIT is Linux-only, so **Audio Processing**
  defaults to **Fast** on Android.

### Fixes

- **Small render targets read by texel** — At a render scale above 1x, a
  small render-to-texture target (kept at 1x) that a game reads by texel
  was read from part of its area, stretched. Reported for Sega GT 2002's
  car skins. From upstream hakuX.
- **Arm Mali drivers** — From upstream hakuX: on Arm's Mali-G57 driver,
  W-buffered draws use the rasterizer's depth, avoiding a GPU hang
  (Morrowind). On Arm's drivers before r40, timeline waits poll the GPU's
  counter instead of the driver's wait, which lost the device. On Arm's
  Mali-G52 driver, presents skip the CPU wait and GPU timestamps are off.
  A failed timeline wait now logs what it waited for.
- Everything in 0.18.1-exp.1: Vulkan 1.3 GPUs without descriptor indexing,
  and the device-creation log.

### Testing

The Linux builds pass CI's checks on both architectures, and on an AMD64
machine (RADV) Burnout 3 ran normally at 2x. The Mali fixes haven't been
tried on Mali hardware here. The Android core builds and packages cleanly
with the NDK. The stop it uses (the core unloads without ending RetroArch's
process) ran four load/unload cycles of Burnout 3 on AMD64, but the core
hasn't run on Android.

Implementations: [upstream fixes](https://github.com/portare-ch/hakuX-lr/pull/80),
[Android core](https://github.com/portare-ch/hakuX-lr/pull/81).

## linux-v0.18.1-exp.1 (Linux, aarch64 and x86-64, experimental)

A fix for Vulkan 1.3 GPUs without descriptor indexing, for testing on
ROCKNIX's S922X (Mali-G52). It's a pre-release; 0.18.0 stays the latest
stable release, and everything else is as in 0.18.0.

### Fixes

- **Vulkan 1.3 GPUs without descriptor indexing** — On a Vulkan 1.3
  driver, the core asked for four descriptor-indexing features even when
  it used its fixed-size descriptor sets, which need none of them. Vulkan
  1.3 doesn't require these features, and a driver without them refuses to
  create the device, so the core can't start. It now asks for them only for
  its bindless descriptor set, as it already did below 1.3. ROCKNIX's first
  test on the S922X (Mali-G52, Arm's driver) stopped at device creation
  with an unsupported feature.
- **Device creation in the log** — Before creating the device, the core
  logs the GPU, its Vulkan version, the driver and its release, and each
  feature it asks for that the GPU doesn't report, as `vk: features: ...
  asked for, not supported`. A failed start now shows them.

### Testing

On an AMD GPU made to report Vulkan 1.3 without these features (the
`VP_HAKUX_vk13_nodi` profile, see TESTING.md), Burnout 3 ran on the
fixed-size descriptor sets with no validation errors or warnings. It hasn't
run on a Mali yet. If the core still doesn't start there, the `vk:
features:` lines in the log name what the GPU lacks.

Implementation: [Vulkan 1.3 without descriptor indexing](https://github.com/portare-ch/hakuX-lr/pull/79).

## linux-v0.18.0 (Linux, aarch64 and x86-64)

Adds RetroArch cheats, optional frame pacing, and Full DSP and sinc audio
on AMD64, alongside game compatibility and renderer fixes.

### What's new

- **RetroArch cheats** — Enable codes through **Quick Menu → Cheats** or
  a `.cht` file. Supports hakuX's cheat syntax and `AAAAAAAA VVVVVVVV`
  32-bit writes, with multiple codes joined by `+`, spaces or newlines.
  Changes apply during play and survive a reset; invalid codes stay off
  with a log message. See the [code formats](https://github.com/portare-ch/hakuX-lr/blob/linux-v0.18.0/native/README.linux.md#the-libretro-core).
- **Full DSP on AMD64** — The DSP56300 JIT now runs on x86-64 Linux as
  well, so AMD64 defaults to **Full DSP (experimental)** like ARM64 and plays
  the game's Xbox reverb and other effects. Choose **Fast** if audio
  crackles or a game runs slowly.
- **Sinc resampling and 3D audio with AVX2 on AMD64** — The 32-tap sinc
  resampler, now the default on AMD64 as well, and the 3D audio (HRTF)
  filter use AVX2 and FMA on CPUs that have them (Intel Haswell, AMD Zen and
  newer), producing the same samples as ARM64's NEON, and SSE2 on older
  CPUs. The voice mix and output conversion also gain vectorized paths.
- **Frame Pacing (experimental)** — A core option from rfandango/hakuX#14,
  off by default. When a game's rendering falls behind its simulation,
  **On** holds the game to the render pace so unrendered frames don't queue
  up as input lag; **Lowest Latency** waits one frame less. It switches
  during play. Each second a game draws, the renderer logs a `frame pace`
  line for measuring it, pacing on or off.

### Fixes

- **DSP effects on both architectures** — Update the DSP56300 JIT to
  v1.0.0, fixing stale addresses in repeated DSP instructions that could
  produce incorrect reverb and filter output.
- **ADPCM and streaming audio** — Accept ADPCM blocks with a non-zero
  reserved byte, as used by Jet Set Radio Future, and clamp out-of-range
  step indexes. Wait for unfinished stream entries instead of reading
  audio from address zero.
- **Deep disc directories** — Find and list files beyond the previous
  64-level limit, including Conker's deep audio directory. Cyclic directory
  trees terminate without overflowing the host stack.
- **DVD security checks** — Provide the security-sector response checked
  by titles such as Rayman 3 (EUR), which previously returned to the
  dashboard.
- **Guest faults and game code patches** — Dispatch access violations to
  the game's exception handlers and preserve forward REP-copy progress
  on ARM64. Refresh translated code when a game rewrites its executable
  pages. These address Serious Sam (EUR) streaming stalls and Rainbow Six
  3: Black Arrow mission-loading crashes. Exception and interrupt dispatch
  also make tracked stack pages writable before writing to them.
- **Crimson Skies with DLC** — Trim an image copy that overwrote the USB
  driver's memory after selecting a profile with DLC installed.
- **Renderer lookups and texture cleanup** — Keep texture and surface
  lookups fast as IDs grow during long sessions. Grow the retired-texture
  table when needed to prevent an out-of-bounds write during large bursts
  of texture destruction.

### Testing

- x86-64 CI builds the DSP JIT with the image's Rust toolchain and runs its
  checks; the audio unit tests run at each vector level the runner's CPU
  has and compare the AVX2 paths with scalar models of NEON's arithmetic.
- Regression coverage includes cheat parsing, concurrent updates and
  resets; frame-pacing settings; deep and cyclic disc directories; DVD
  signatures; ADPCM headers; and exception dispatch on tracked stacks.

In Burnout 3 on a Ryzen 7 9800X3D (headless, 20 seconds from 20 s in), the
audio thread used about 14% of one CPU core with Full DSP (29% with the C
interpreter, 1% with Fast). Full DSP and sinc resampling on AMD64 still
need in-game listening tests.

The newly ported game fixes have not been verified in those games on
Linux. Real-game cheats and frame pacing on
the Retroid Pocket Nova also still need device testing. The AMD64 backend
and Full DSP remain experimental; frame pacing is off by default.

Implementations: [renderer](https://github.com/portare-ch/hakuX-lr/pull/70),
[audio guards](https://github.com/portare-ch/hakuX-lr/pull/71),
[disc access](https://github.com/portare-ch/hakuX-lr/pull/72),
[kernel and CPU](https://github.com/portare-ch/hakuX-lr/pull/73),
[cheats](https://github.com/portare-ch/hakuX-lr/pull/74),
[frame pacing](https://github.com/portare-ch/hakuX-lr/pull/75),
[AMD64 audio](https://github.com/portare-ch/hakuX-lr/pull/77),
[DSP update](https://github.com/portare-ch/hakuX-lr/pull/78).

## linux-v0.17.4 (Linux, aarch64 and x86-64)

### Fixes

- **Halo level loading** — Fix a regression since `linux-v0.14.0` that
  crashed Halo while loading its first level. Move the initial kernel pool
  to the top MiB of RAM, leaving the game's low memory pages available and
  preserving space for Burnout 2's fixed memory layout.
- **Allocation diagnostics** — Include the caller, address bounds and
  alignment when a contiguous memory allocation fails.

### Testing

- Add a regression check that kernel boot leaves title memory free between
  the kernel and the top MiB of RAM.
- Halo's first level, opening cutscene and gameplay were verified on a
  Retroid Pocket Nova (ARM64, Turnip).

Halo gameplay on AMD64 and Burnout 2 were not tested.

## linux-v0.17.3 (Linux, aarch64 and x86-64)

### What's new

- **Vulkan 1.2 compatibility** — Requirements and core metadata now reflect
  the renderer's existing compatibility path. Vulkan 1.3 is recommended.
  The Vulkan loader (`libvulkan.so.1`) must still be 1.3 or newer; the setup
  script warns about older loaders.

### Fixes

- **Debug frame dumps** — Prevent a second free of buffer memory when the
  renderer stops after a render-target dump.
- **Compatibility log** — Report `Vulkan below 1.3` instead of incorrectly
  identifying Vulkan 1.2 drivers as Vulkan 1.1.

### Testing

- Add compatibility-path smoke tests for resets and video driver rebuilds,
  plus a Vulkan 1.2 test profile and instructions.
- On x86-64 with RADV and simulated Vulkan 1.2, Burnout 3 and Dead or Alive 3
  ran at full speed in the harness without Vulkan validation errors.
  Dead or Alive 3 also ran at 60 fps in RetroArch. Repeatable scenes matched
  the normal rendering path pixel for pixel.

Vulkan 1.2 capabilities were simulated on a modern GPU. Real Vulkan 1.2
hardware was not tested.

## linux-v0.17.2 (Linux, aarch64 and x86-64)

### Fixes

- **AMD64 movie playback** — Preserve MMX and SSE registers when switching
  guest threads. This fixes the partially black intro videos in Burnout 3
  caused by the KVM backend losing the decoder's alpha mask.
- **Vulkan image lifetime** — Wait for the shared queue before replacing
  export images after a frame-size or sync-mask change. This prevents
  freeing images still in use after RetroArch rebuilds its swapchain.
  Steady-state rendering adds no wait.
- **Vulkan device setup** — Enable the mirror-clamp sampling feature when
  supported, matching the extension RetroArch enables for its samplers.
  This resolves the related Vulkan validation error during device creation.
- **Updated libretro headers** — Match RetroArch `11e8931021`, now used by
  PortareOS.

### Setup and documentation

- **Linux setup script** — `tools/linux/setup-hakux.sh` installs RetroArch,
  downloads the latest stable core for the CPU, and creates a separate
  configuration and game launcher. It checks KVM access on AMD64 and
  supports desktop and direct-display use. The launcher accepts file names
  or partial names and keeps separate core and RetroArch logs.
- **README** — Simplify the installation instructions and architecture
  comparison, explain the settings in plain language, and document where
  to find logs.

### Testing

- Add a regression check for MMX and SSE state restored over an initial
  FPU state. On the AMD64 test machine, Burnout 3's intro plays without the
  black rows, and Dead or Alive 3 reaches its attract mode at 60 fps.
- Make repeated-load memory checks distinguish allocated memory from
  heap space retained by glibc, avoiding false leak reports with RADV.

## linux-v0.17.1 (Linux, aarch64 and x86-64)

### Fixes

- **Overscan crop preserves the aspect ratio** — Crop Overscan now removes
  3.75% from each edge. At 2x, a 640 x 480 game produces 1184 x 888,
  exactly 4:3, while still hiding Burnout 3's black border. RetroArch also
  receives an aspect ratio adjusted for the crop, avoiding distortion when
  crop sizes are rounded to whole pixels.

## linux-v0.17.0 (Linux, aarch64 and x86-64)

Adds an experimental AMD64 (x86-64) Linux core alongside the ARM64 build
used by PortareOS. Download the archive for your machine's architecture.

### What's new

- **AMD64 Linux core** — Runs Xbox code on Intel and AMD CPUs through
  KVM, sharing the ARM64 build's kernel, devices and Vulkan renderer.
  Requires Linux 5.16 or newer, access to `/dev/kvm`, TSC scaling and UMIP.
  See the [AMD64 installation guide](https://github.com/portare-ch/hakuX-lr/blob/linux-v0.17.0/README.md#play-on-amd64-x86-64-linux-systems)
  for virtualization setup, permissions and RetroArch installation.
- **Archives for both architectures** — Releases now include `aarch64`
  and `x86_64` archives, each with a SHA-256 checksum and `BUILD-INFO`.
  Publication waits for both architectures' build, regression and package
  checks to pass.
- **KVM regression coverage** — Checks CPU exits, thread state, memory
  faults, interrupts, Xbox clock timing and page protection across threads.
  The libretro harness also tests synthetic titles, graphics and repeated
  loading and unloading under KVM.
- **Comparing ARM64 and AMD64** — The testing guide covers using the
  same game, in-game saves and audio settings to investigate differences
  between FEX and KVM.

The KVM backend and core have automated test coverage on GitHub's Linux
runners. Real-game play and GPU driver compatibility on AMD64 still need
device validation. [Implementation](https://github.com/portare-ch/hakuX-lr/issues/56).

### Current limitations

- AMD64 uses linear audio resampling and defaults to Fast audio
  processing; the DSP JIT is available only on ARM64.
- Only one console behind the same router can use external UDP 3074.
  An upstream NAT, including carrier-grade NAT, may still block peers.
- Dashboard files are not included; install your own copy to use Start Core.
  Dashboard navigation and end-to-end Insignia setup still need device validation.
- Save states, rewind and run-ahead are unavailable. Transfer progress
  between architectures using in-game saves.
- XLink Kai and a general-purpose Internet (NAT) option are unavailable in
  RetroArch; NAT is used by the Insignia option.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.16.0 (PortareOS)

Adds UDP forwarding and automatic router port mapping for Xbox Live
(Insignia), allowing peers to reach the console before it sends to them.

### What's new

- **Insignia UDP forwarding** — Forwards the console's UDP 3074 through
  the device's UDP 3074. With **Network → Xbox Live (Insignia)** enabled,
  enable UPnP on the router or forward UDP 3074 to the device manually for
  Open NAT. If the device has a firewall, allow inbound UDP 3074.
- **Automatic router setup** — Discovers UPnP routers, requests a port
  forward, renews leases with retries and checks ownership before cleanup
  on shutdown. Supports routers that require permanent leases.
- **Fallbacks and diagnostics** — If UDP 3074 is busy, outgoing traffic
  uses a free port without automatic forwarding. Logs explain port
  conflicts, missing routers and upstream NAT. Advanced overrides are
  documented in the
  [NAT options](https://github.com/portare-ch/hakuX-lr/blob/linux-v0.16.0/native/README.linux.md#insignia-nat).
- **Network regression checks** — Adds tests using console Ethernet
  frames and a fake UPnP router for incoming traffic, busy ports, lease
  renewal, router replies and ownership checks during shutdown.

The merged code passed the Linux build, automated regression tests, package
verification, generated Vulkan shaders and save-state schema checks.
Real-router interoperability, dashboard navigation in RetroArch and
end-to-end Insignia registration, account setup and online play still need
device validation.
[Implementation and validation](https://github.com/portare-ch/hakuX-lr/pull/55).

### Current limitations

- Only one console behind the same router can use external UDP 3074.
  An upstream NAT, including carrier-grade NAT, may still block peers.
- Dashboard files are not included; install your own copy to use Start Core.
- Save states are not yet available.
- XLink Kai and a general-purpose Internet (NAT) option are unavailable in
  RetroArch; NAT is used by the Insignia option.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.15.0 (PortareOS)

Boot the Xbox dashboard from RetroArch to access its Xbox Live pages for
Insignia account setup.

### What's new

- **Xbox dashboard** — Select **Load Core → Microsoft - Xbox (hakuX) →
  Start Core** without loading a game. Copy your dashboard files
  (`xboxdash.xbe` and its folders) into `hdd/Partition2/` under RetroArch's
  save directory (`/storage/saves/xbox/hdd/Partition2/` on PortareOS).
  If the dashboard is missing, the core shows where to install it.
- **Dashboard restarts** — Supports resets, video driver rebuilds and
  navigation between the dashboard's main menu and Xbox Live pages.
  Process relaunches preserve the configuration and save directory,
  including when the core was loaded through RetroArch's menu.
- **Insignia guidance** — Prompts when a game has no Insignia account or
  the dashboard needs **Network → Xbox Live (Insignia)** enabled. Existing
  Insignia gamertags are listed in the log. Register the console with
  Insignia's Setup Assistant before creating an account in the dashboard.
- **Regression checks** — Adds dashboard boot, reset, game switching,
  relaunch and account-detection coverage. Includes the restart memory-check
  fix documented after 0.14.0.

The merged code passed the Linux build, automated regression tests, package
verification, generated Vulkan shaders and save-state schema checks.
Dashboard coverage uses synthetic executables in the libretro harness;
real RetroArch dashboard navigation and end-to-end Insignia registration,
account setup and sign-in still need device validation.
[Implementation and validation](https://github.com/portare-ch/hakuX-lr/pull/54).

### Current limitations

- Dashboard files are not included; install your own copy to use Start Core.
- Save states are not yet available.
- XLink Kai and a general-purpose Internet (NAT) option are unavailable in
  RetroArch; NAT is used by the Insignia option.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.14.0 (PortareOS)

Brings upstream hakuX 1.3.0's game fixes to RetroArch and adds an experimental
Xbox Live (Insignia) network option.

### What's new

- **Xbox Live (Insignia)** — Choose **Quick Menu → Core Options → Network →
  Xbox Live (Insignia)**, then restart the game. Register the console first
  with Insignia's Setup Assistant. If your provider blocks Insignia's DNS
  server, put an insigniaDNS server's IPv4 address in `insignia-dns.txt` in
  the save directory. Only the first address is used; a missing or invalid
  address falls back to Insignia's default DNS server.
- **Console drive timing** — Disc and hard-disk reads now take a little
  time, activating upstream's fix for Morrowind freezing when leaving a
  building.
- **Graphics and memory handling** — Imports upstream's render-target
  coherence fixes while retaining the fork's allocation checks, CHD
  support, graphics features and DSP audio.
- **Restarts** — Cleans up the new timer, networking and title-update
  resources when closing, resetting or switching games. Fixes a network
  receive wait that could keep most of a CPU core busy.

### Game fixes from upstream

- **Fable: The Lost Chapters** — Fixes black or noisy faces and arms.
- **Morrowind** — Fixes the inventory character's red background, black
  band and ghost copies after loading.
- **Halo 2** — Fixes striped fog and the light-blue view around the scope.
- **Munch's Oddysee** — Removes the dark box around character shadows.
- **Project Zero II (PAL), Amped 2, Burnout 2, Def Jam: Fight for NY and
  Soldier of Fortune II** — Fixes startup, loading and intro failures.
- **Double-S.T.E.A.L: The Second Clash and Touge R** — Fixes scene startup
  and black-screen rendering problems.

The Linux build, automated regression tests, package verification, generated
Vulkan shaders and save-state schema checks passed. These upstream game
fixes have not been verified on the Nova for this release. Insignia sign-in
and Setup Assistant registration remain untested.
[Implementation and validation](https://github.com/portare-ch/hakuX-lr/pull/53).

### Current limitations

- The Xbox dashboard cannot be booted from RetroArch yet. Creating or
  recovering an Insignia account requires the dashboard.
- Save states are not yet available.
- XLink Kai and a general-purpose Internet (NAT) option are unavailable in
  RetroArch; NAT is used by the Insignia option.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.13.0 (PortareOS)

Xbox reverb and other DSP effects now play through stereo output, enabled by
default and accelerated by a DSP56300 JIT on ARM64 Linux.

### What's new

- **Full DSP by default** — Runs the game's Xbox audio programs for reverb and
  other effects. Uses the DSP JIT automatically; no setting change is needed
  unless a previous build saved an explicit audio-processing preference.
- **Fast fallback** — If a game has audio or performance problems, choose
  **Quick Menu → Core Options → Audio Processing → Fast**, then restart the
  game. This restores the previous dry mix and uses less CPU.
- **Voice completion** — Rechecks released voices before notifying the game,
  preventing stale notifications when a voice has been locked, restarted or
  removed. Also optimizes the C interpreter retained for debugging and builds
  without the JIT.

On the Retroid Pocket Nova, paired replay of DOA3's captured audio programs used
**27% less DSP CPU time with the JIT** than the optimized interpreter
(0.483 → 0.351 CPU-seconds per audio-second). Seven silence, impulse and tone
recordings were byte-identical, including the stereo reverb response. A live
fight passed listening and control checks with no observed audio underruns.
[Implementation and validation](https://github.com/portare-ch/hakuX-lr/pull/52).

Full DSP remains experimental and costs more CPU than Fast. Please report the
game and scene if audio sounds wrong or performance regresses, and whether
switching to Fast helps.

### Current limitations

- Save states are not yet available.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.12.2 (PortareOS)

Corrects misplaced specular highlights from point and spot lights, including
character lighting during Dead or Alive 3's electrical-fence effect in X-Octagon.

Verified against original Xbox lighting-test captures and in DOA3 on the Retroid
Pocket Nova. [Matched before/after screenshots and test results](https://github.com/portare-ch/hakuX-lr/pull/51).

### Current limitations

- Save states are not yet available.
- Reverb and other Xbox DSP effects are missing from the default audio mix.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.12.1 (PortareOS)

Clearer core options and documentation. The emulator is unchanged from 0.12.0.

### What's changed

- **Core options** — Shorter, plainer descriptions in **Quick Menu → Core Options**. Options that apply at the next boot now say "Restart the game to apply", and RetroArch's message after changing one says the same.
- **Core information** — The description and notes RetroArch shows for the core are rewritten.
- **`BUILD-INFO`** — Links the source revision to the hakuX-lr repository.

### Current limitations

- Save states are not yet available.
- Reverb and other Xbox DSP effects are missing from the default audio mix.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.12.0 (PortareOS)

Better stereo audio, a NEON-accelerated audio resampler, and broader graphics support. Replaces 0.11.0 and all 0.12.0 experimental builds.

### What's new

- **Stereo positioning** — Sounds keep their left/right placement, including 3D effects. Surround channels use the stereo downmix measured from Halo's audio programs, correcting rear sounds that were too loud.
- **Clearer audio with ARM NEON** — A new 32-tap sinc resampler preserves high frequencies in speech and effects and reduces aliasing in high-pitched sounds. It uses ARM NEON vector instructions. To use the lighter, previous resampler, choose **Quick Menu → Core Options → Audio Resampler → Linear**, then restart the game.
- **Fewer audio glitches** — Short sounds play to the end, new 3D sounds no longer inherit audio from a previous sound, and audio left over after a partial RetroArch write is queued instead of dropped.
- **More texture formats** — L16, V16U16, R6G5B5, R5G5B5A1, R4G4B4A4 and depth textures used by pixel shaders render instead of appearing white.
- **Clipping** — Supports exclusive and multiple clip windows used for split-screen, picture-in-picture and HUD masks.
- **Lighting and shaders** — More accurate spotlight falloff and specular highlights. Adds depth replacement (`texm3x2depth`), BRDF lookups and constant-eye specular reflections (`texm3x3spec`).

On the Nova, brief play sessions in Burnout 3 and Halo showed no graphics glitches. Halo ran at 30 fps with sinc resampling; audio processing used about 3% of one CPU core. The new stereo mix has been measured against Halo's audio programs, but still needs in-game listening tests.

### Current limitations

- Save states are not yet available.
- Reverb and other Xbox DSP effects are missing from the default audio mix.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.12.0-exp.3 (PortareOS, experimental)

Clearer audio and complete playback of short sounds. Experimental; 0.11.0 remained stable at release.

### What's new

- **NEON sinc resampler** — Preserves high-frequency detail in speech and effects and reduces aliasing in high-pitched sounds. In tests with 22 kHz input, 10 kHz content was within 0.2 dB of its original level, compared with a 6 dB loss before. Choose **Quick Menu → Core Options → Audio Resampler → Linear** for the previous resampler, then restart the game.
- **Complete short sounds** — Fixes lost audio at the end of non-looping sounds (2–3 ms) and very short effects disappearing at high pitch.
- Everything in 0.12.0-exp.2 below.

### Experimental

Sinc passed automated tests and device benchmarks; in-game listening tests were still pending. With 64 simultaneous sounds, it used about 5–9% of a frame on the Nova's big cores, two to three times the linear resampler. If audio sounds worse or crackles, try **Linear** and report the game.

### Current limitations

- Save states are not yet available.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.12.0-exp.2 (PortareOS, experimental)

Audio fixes alongside the graphics changes in 0.12.0-exp.1. Experimental; 0.11.0 remained stable at release.

### What's new

- **Stereo panning** — 2D sounds, including music and menu effects, keep their left/right balance instead of playing equally through both speakers.
- **Clean 3D sound starts** — New sounds no longer inherit audio from the previous sound using the same voice.
- **Audio buffering** — Queues samples RetroArch could not accept, fixing clicks and gaps caused by dropping about 17 ms of audio per partial write.
- Everything in 0.12.0-exp.1 below.

### Experimental

Automated tests passed; in-game listening tests were still pending. These changes may not fix quiet, off-centre audio in Halo and Ninja Gaiden Black: 3D sounds, surround channels and reverb are unchanged in this build. Report differences from 0.11.0 with the game and scene.

### Current limitations

- Save states are not yet available.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.12.0-exp.1 (PortareOS, experimental)

Broader Xbox GPU support, shipped in PortareOS for testing. Experimental; 0.11.0 remained stable at release.

### What's new

- **Texture formats** — L16, V16U16, R6G5B5, R5G5B5A1, R4G4B4A4 and depth textures used by pixel shaders render instead of appearing white.
- **Clipping** — Supports exclusive and multiple clip windows for split-screen, picture-in-picture and HUD masks.
- **Lighting** — Uses the Xbox lighting curve for spotlight falloff and specular highlights.
- **Pixel shaders** — Adds depth replacement (`texm3x2depth`), BRDF lookups and constant-eye specular reflections (`texm3x3spec`).

### Experimental

Based on NVIDIA documentation, xemu and available Xbox captures from nxdk_pgraph_tests. Brief Nova sessions in Burnout 3 and Halo showed no graphics glitches; other games were untested. Report regressions from 0.11.0 with the game and scene.

### Current limitations

- Save states are not yet available.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.11.0 (PortareOS)

System link: play a game's LAN mode with other devices on your network.

### What's new

- **System link (LAN)** — Enable **Quick Menu → Core Options → Network → LAN (system link)** on each device, then restart the game. Devices on the same Wi-Fi discover each other. Add peers on another subnet or a VPN to `lan-peers.txt` in the save directory. Off by default.
- **Duplicate console identities** — Detects devices sharing a copied hard disk identity and warns in RetroArch. A new identity is generated on the next game start; existing saves remain valid.

Verified two-way LAN traffic between Halo on the Nova and another computer. Multiplayer between two devices was still untested.

### Current limitations

- Save states are not yet available.
- XLink Kai and Internet (NAT) networking are unavailable in RetroArch.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.10.0 (PortareOS)

Fixes an NBA Inside Drive 2003 crash and nxdk startup, and adds antialiasing controls.

### What's new

- **NBA Inside Drive 2003** — Fixes a crash after the intro caused by an oversized framebuffer copy overwriting game code. Confirmed past the intro at full speed on the Nova.
- **Antialiasing controls** — Adds alpha-to-coverage and sample masks for effects such as foliage, fences and hair.
- **nxdk homebrew** — Programs built with the open-source Xbox SDK get past startup. nxdk_pgraph_tests completes, but its graphics still show a white screen.
- **Cache rebuild** — Graphics pipelines rebuild on first launch after updating, which may cause brief stutter.

Antialiasing controls follow register documentation for the NV2A's successor and pass automated tests and shader checks. In-game use, sample order and coverage patterns still need verification against an Xbox.

### Current limitations

- Save states are not yet available.
- Networking is off; RetroArch network settings are not available in this release.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- NBA Inside Drive 2003 has blank saved player textures because GPU readback is incomplete.

## linux-v0.9.0 (PortareOS)

More of the Xbox GPU's texture and drawing features.

### What's new

- **Anisotropic filtering** — Textures a game filters anisotropically, such as floors and roads seen at an angle, are filtered at the game's degree (2x to 8x, up to what the GPU supports) instead of blurring.
- **Two-sided lighting** — Lit geometry drawn with both sides visible, such as foliage, cloth and flags, gets its own lighting on the back faces instead of the front's.
- **Colour key** — Textures keyed by a colour, such as sprites, no longer show that colour: matching texels become transparent, black, or are not drawn, as the game chooses.
- **Line width** — Lines, such as HUDs, radars and beams, are as wide as the game asks, scaled with the render scale. At 2x they were half as thick as on an Xbox.
- **Dithering** — Games that dither into 16-bit images are dithered, so skies, fog and gradients there no longer show bands.
- **Quincunx and gaussian filters** — Textures read through the Xbox's convolution filters, used by D3D's quincunx and gaussian antialiasing and by blur effects, are filtered with those kernels instead of plainly.
- **Signed textures** — Texture channels a game reads as signed, such as the normal maps of bump lighting, are read as signed, so bumps are no longer flat or inverted.
- **First start after updating** — The GPU pipelines are built again once, so a game may stutter briefly the first time.

These follow the Xbox GPU's registers and its published hardware tests, and pass the build's tests and shader checks. They haven't been tried in games on the Nova yet. The dither pattern, the quincunx kernel and the exact signed range haven't been compared against an Xbox.

### Current limitations

- Save states are not yet available.
- Networking is off; RetroArch network settings are not available in this release.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- Full disc dumps (Redump), as ISO or CHD, are untested with real games.

## linux-v0.8.0 (PortareOS)

Controller rumble and a sharper default picture.

### What's new

- **Rumble** — Enable **Quick Menu → Core Options → Rumble**; it applies immediately. Vibration stops when paused, closed or restarted. Adjust strength in **Settings → Input → Haptic Feedback/Vibration**.
- **2x render scale by default** — Most games render at 1280x960, with sharper text and edges. Overscan stays cropped to fill a 4:3 screen. Choose **1x** if a game runs slowly.

### Current limitations

- Save states are not yet available.
- Networking is off; RetroArch network settings are not available in this release.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- Full disc dumps (Redump), as ISO or CHD, are untested with real games.

## linux-v0.7.2 (PortareOS)

Every frame the game draws reaches RetroArch.

### What's fixed

- **Repeated and skipped frames** — Fixes frame timing with variable RetroArch delays, such as audio sync waits. On ROCKNIX, these delays reduced a 59.9 fps game to 47.5 displayed frames/s. Frame selection now uses the actual read time. PortareOS's settings were unaffected.

### Current limitations

- Controller rumble is unavailable.
- Save states are not yet available.
- Networking is off; RetroArch network settings are not available in this release.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- Full disc dumps (Redump), as ISO or CHD, are untested with real games.

## linux-v0.7.1 (PortareOS)

Sound after a game starts again in RetroArch.

### What's fixed

- **Sound after a restart** — After RetroArch's Restart, a video driver rebuild, or a game switching to another of its programs (Jade Empire after New Game), the game went on without sound. Each new boot now gets its audio output set up again. Reported, with the fix and a test, by the pocket-ui project.
- **Build image** — The pinned Debian image now has a Vulkan loader with Mesa's drivers and `llvm-strip-19`, so `linux-build.sh`'s checks and `linux-package.sh` run in it without extra packages.

### Current limitations

- Controller rumble is unavailable.
- Save states are not yet available.
- Networking is off; RetroArch network settings are not available in this release.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- Full disc dumps (Redump), as ISO or CHD, are untested with real games.

## linux-v0.7.0 (PortareOS)

Upstream hakuX 1.2.0, with game compatibility and sound fixes.

### What's new

- **Upstream hakuX 1.2.0** — Includes upstream fixes for Doom 3, Fatal Frame II, Silent Hill 2, Splinter Cell: Double Agent, Ultimate Spider-Man, Grand Theft Auto III, Super Monkey Ball Deluxe, Sega GT 2002, Project Gotham Racing 2 and Dungeons & Dragons Heroes. These game fixes have not been checked on the Nova in this release.
- **Sound** — Includes upstream's fixes for games freezing while they hold audio voices, or continuing to play a voice after stopping it.
- **Linux integration** — The new network and Ethernet controller threads stop with the game, so closing, restarting and loading another game release their resources. Upstream's controller updates use the same hot-plugging state as RetroArch's ports.
- **Getting started** — The README covers installation, game formats, controls and troubleshooting on PortareOS and other ARM64 Linux systems.

### Current limitations

- Controller rumble is unavailable.
- Save states are not yet available.
- Networking is off; RetroArch network settings are not available in this release.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- Full disc dumps (Redump), as ISO or CHD, are untested with real games.

## linux-v0.6.0 (PortareOS)

CHD disc images in the RetroArch core.

### What's new

- **CHD disc images** — Games can be `.chd` files made with `chdman createdvd` from an `.iso` image. The core reads them in place, without unpacking them. A CHD takes about a third less space than its `.iso` (Burnout 3: 2.48 GB to 1.49 GB), and the game runs from it as fast as from the `.iso`.

### Fixed

- **Discs that can't be read** — When a game's disc image can't be read, the core closes its file once, not a second time when the game stops.

### Current limitations

- Controller rumble is unavailable.
- Save states are not yet available.
- CD CHDs and CHDs that require a parent image are unsupported. Use `chdman createdvd`.
- Full disc dumps (Redump), as ISO or CHD, are untested with real games.

## linux-v0.5.0 (PortareOS)

Players 2-4 in the RetroArch core.

### What's new

- **Players 2-4** — RetroArch's ports 1-4 are players 1-4: a second controller plays as player 2, and so on up to four. Each port has a controller plugged in unless its Device Type (Quick Menu → Controls → Port N Controls) is None.
- **Plugging in and out** — Changing a port's Device Type during the game plugs its controller in or pulls it out, and the game sees it as on a console: most games pause when player 1's controller goes. The ports stay so when the game restarts.

### Current limitations

- Controller rumble is unavailable.
- Save states are not yet available.
- Games must be `.iso` disc images.

## linux-v0.4.0 (PortareOS)

The RetroArch core stops and starts games inside RetroArch, and the Linux standalone is gone.

### What's new

- **Restart** — RetroArch's Restart starts the game again inside RetroArch.
- **Games that start another of their programs** — They continue inside RetroArch without restarting it.
- **Closing a game** — The core stops the game inside RetroArch. On PortareOS, RetroArch then returns to the launcher, as with every core; elsewhere it can load another game in the same session.
- **Video driver restarts** — If RetroArch restarts its video driver, the game starts again with a message, instead of RetroArch crashing.
- **No freeze on closing** — If stopping a game ever takes longer than 10 seconds, RetroArch closes instead of freezing.
- **Testing instructions** — [TESTING.md](TESTING.md) covers installing a release, logs and what to check, on PortareOS and on other ARM64 Linux systems.

### Removed

- **The Linux standalone `hakux`** — It wasn't tested, and PortareOS no longer used it. The release now holds only the RetroArch core.

### Current limitations

- Only player 1 is supported.
- Save states are not yet available.
- Games must be `.iso` disc images.

## linux-v0.3.0 (PortareOS)

Core options for the RetroArch core, and upstream hakuX 1.1.0.

### What's new

- **Core options** — RetroArch's Quick Menu has options for the render scale (1x to 4x) and the console's settings: screen format (widescreen games then show 16:9), language (RetroArch's by default) and video region. They take effect the next time the game starts.
- **Overscan crop option** — The border a TV hid is still cropped by default; turning the option off shows the whole picture, at once.
- **PAL games at 60 Hz** — European games that offer 60 Hz now run at it.
- **Upstream hakuX 1.1.0** — Includes upstream's emulator changes up to its version 1.1.0, mainly support for GPUs other than Adreno. Adreno GPUs, such as the Nova's, are unchanged.

### Current limitations

- Closing a game also closes RetroArch.
- Only player 1 is supported.
- Reset and save states are not yet available.
- Games must be XISO disc images with a `.iso` extension.

## linux-v0.2.1 (PortareOS)

Two fixes for the RetroArch core.

### What's fixed

- **Games that switch programs** — When a game starts another of its programs, RetroArch restarts on the same game and continues into it instead of returning to the launcher.
- **Video driver restarts** — If RetroArch restarts its video driver while a game is running, the game now closes and returns to the launcher, with a log entry, instead of possibly crashing.

### Current limitations

- Closing a game also closes RetroArch.
- Only player 1 is supported.
- Reset, core settings and save states are not yet available.
- Games must be XISO disc images with a `.iso` extension.

## linux-v0.2.0 (PortareOS)

Play Xbox games through RetroArch on PortareOS (Retroid Pocket Nova).

### What's new

- **RetroArch support** — Run hakuX as a RetroArch core, with access to RetroArch's menu, hotkeys and controller remapping.
- **Display timing** — RetroArch handles when frames appear on screen, including variable refresh rates and compensation for low frame rates. Graphics stay on the GPU throughout.
- **Xbox timing** — Matches the original Xbox's 59.94 Hz video timing and keeps audio in sync.
- **4:3 display** — Removes the border normally hidden by a TV, so standard-definition games fill a 4:3 screen.
- **Game saves** — Stores the emulated Xbox hard drive in RetroArch's save folder (`/storage/saves/xbox` on PortareOS).
- **Performance logs** — Optional logs show frame rates, frame delivery delays and the core's processing time per frame. Enable them with `HAKUX_DISP_STATS` and `HAKUX_LR_STATS`.

### Current limitations

- Closing a game also closes RetroArch.
- Only player 1 is supported.
- Reset, core settings and save states are not yet available.
- Games must be XISO disc images with a `.iso` extension.

## linux-v0.1.0 (PortareOS)

The first standalone hakuX release for PortareOS on the Retroid Pocket Nova, replacing xemu.

### What's new

- **Standalone emulator** — Play fullscreen with `hakux` on ARM64 Linux (glibc). A command-line version, `xboxrun`, is also available for testing without a display.
- **Audio** — Game audio plays through PipeWire.
- **Controllers** — All connected gamepads control player 1. Press **Home + Start** to quit.
- **PortareOS package** — Adds a script to create reproducible release archives for installation on PortareOS.

## v1.3.0

Xbox Live through Insignia, and game fixes.

### Xbox Live
- **Insignia** — Settings → Network → Connection → Xbox Live: Insignia connects the console to Insignia, the revival of the original Xbox Live: Live games sign in, find matches and play online. If your Internet provider blocks Insignia's DNS server, point it at a computer running insigniaDNS instead
- **Xbox Live setup** — Settings → Network → Xbox Live setup walks you through it, one step at a time: the connection, the Xbox dashboard, registering the console with Insignia's Setup Assistant (downloaded for you), then making or recovering your account in the dashboard
- **Dashboard** — the library's Dashboard button (top left) starts the Xbox dashboard from the console's hard disk, where Xbox Live accounts are made and recovered. The first time, it installs the dashboard from a zip of its files (dashboard 5960) that you provide: hakuX doesn't include one
- **Title updates** — a game's Xbox Live update, once downloaded, starts with the game. Forza Motorsport's 60 and 120 fps mods cover its update too

### Games
- **Fable: The Lost Chapters** — the hero's face and arms are no longer black or noise
- **Morrowind** — no longer freezes when you leave a building: disc and hard-disk reads now take a little time, as on the console. The inventory's character is drawn properly: it no longer stands on red with a black band over its head, or leaves ghost copies of itself around it when you change gear after a load
- **Halo 2** — the fog no longer shows as stripes that move with the camera, and with the scope up the view around it is dimmed with black lines, not light blue
- **Munch's Oddysee** — no dark box over the ground under the characters' shadows
- **Project Zero II (PAL)** — no longer hangs on its first frame
- **Amped 2** — no longer crashes as its first level loads
- **Mortal Kombat: Shaolin Monks** — the models no longer explode on Mali-G57 GPUs
- **Burnout 2: Point of Impact** — no longer crashes as it starts
- **Def Jam: Fight for NY** — no longer freezes on one frame at the intro cutscene: while hakuX holds a game up (such as while its shaders compile), the game's clock now waits too
- **Double-S.T.E.A.L: The Second Clash** — no longer stops as its first scene begins, and the screen is no longer black, also after loading a savestate
- **Soldier of Fortune II: Double Helix** — no longer crashes when you skip an intro movie
- **Touge R** — races are no longer black under the HUD

### Performance
- **Network** — with the network on (system link or XLink Kai), the console no longer keeps most of a CPU core busy

### App
- **On-screen controls editor** — its toolbar wraps onto more rows on narrower screens, such as 4:3 handhelds, instead of pushing buttons off the edge

## v1.2.0

System link, stability and compatibility improvements.

### Network
- **System link over your network** — Settings → Network → System link → LAN: hakuX devices on the same Wi-Fi network find each other as Xbox consoles on a LAN do. Devices the network's broadcasts don't reach, on another subnet or a VPN such as ZeroTier or Tailscale, are added by address under Other consoles
- **XLink Kai** — system link over the Internet through an XLink Kai engine running on a computer on your network; its web page picks the game's arena
- **Console identity** — each console has its own serial number, network address and online key. A device whose hard disk was copied from another one offers to make itself a new identity, since two consoles with one identity can't see each other


### Games
- **Sega GT 2002** — the cars have their paint, reflections and sun highlights
- **Doom 3** — no longer crashes as its main menu loads
- **Fatal Frame II** — starts and reads its disc in the order it asks for
- **AirForce Delta Storm** — answers the controller and the on-screen controls (it ignored all input)
- **Silent Hill 2** — no longer crashes when it loads a room
- **Splinter Cell: Double Agent** — no longer closes on boot
- **Super Monkey Ball Deluxe** — its title movie plays and its movies aren't black
- **Ultimate Spider-Man** — no longer crashes a second into boot
- **Grand Theft Auto III** — no longer shows its bad-disc screen
- **Project Gotham Racing 2** — the attract replay no longer goes black
- **Dungeons & Dragons Heroes** — the tutorial's prompts show
- **Sound** — games that hold their voices while the sound engine updates them, or stop a voice as it changes, no longer freeze or play a stopped voice on

### Performance
- **Fewer stutters with Turnip** — frames reach the screen without a CPU wait in front of each one

### App
- **Controllers** — only the players with a controller are plugged into the console (player 1 always, for the on-screen controls), and a controller plugs in or out as it connects, disconnects or is assigned to another player, without restarting the game

## v1.1.0

GPUs beyond Adreno (experimental).

### GPUs
- **Mali, Xclipse and PowerVR (experimental)** — hakuX runs on GPUs other than Adreno: features they lack are replaced (BC and 4444 textures decoded up front, standard border colours, the push block in a buffer, D32S8 depth), and known driver bugs are worked around (`hw/xbox/hle/vk/quirks.c`). Burnout 3 and Mercenaries run cleanly on a Pixel 8 (Mali-G715)
- **Vulkan 1.1 drivers** — GPUs whose drivers stop at Vulkan 1.1 (Valhall Mali on older or vendor-capped drivers, Qualcomm's own older drivers, early Xclipse, PowerVR BXM) run through a compatibility layer (`hw/xbox/hle/vk/compat.c`)
- **Older GPUs without descriptor indexing (experimental)** — Bifrost Mali (G71, G72, G76, G52) and PowerVR Rogue: each draw binds its own textures, and depth is clamped in the shader where the GPU can't
- **Adreno** — unchanged: every replacement is off where the GPU has the feature.

### App
- **GPU driver** — a screen of its own (Settings → GPU driver): the system driver and the stored packages in one list, with their versions and authors. Packages are checked when added and can be deleted (deleting the one in use goes back to the system driver)
- **Turnip recommended** — a Qualcomm phone running its own driver older than Vulkan 1.3 suggests a Turnip driver once, from the library
- **Back key** — the device's back key opens the in-game menu, from any input, including the Retroid Pocket 5's own Back. A controller with both Back and Select keeps Back for the menu; on one without a Select key, Back is the Xbox Back button
- **Dark theme** — Settings → Display → App theme: the phone's, light or dark; the in-game menus follow it, in the app's green rather than Android's teal
- **Controller navigation** — the app works with a controller: the focused item stands out (a game is outlined in green), A selects, B goes back, and Y or Select opens a game's menu. The search field opens the keyboard only on A, the library has a refresh button, the mapping sliders move with left and right, release notes scroll, and X deletes a savestate from the in-game Load state list


## v1.0.0

A new core replaces xemu's QEMU-based one.

### New core
- **CPU** — the Xbox's x86 code runs on a FEX JIT (FEXCore fork), and the Xbox kernel is reimplemented natively (`native/kernel`): no QEMU
- **GPU** — the NV2A push buffer is decoded into Vulkan (HLE, `hw/xbox/hle`). Custom drivers (Turnip) load through adrenotools; driver-specific workarounds are quirks (`hw/xbox/hle/vk/quirks.c`, `debug.hakux.quirks`)
- **Audio** — the MCPX APU runs on the core's own services (`native/dev/apu`), out through AAudio
- **Savestates** — per game, in slots, with the hard disk's files
- **Mods and cheats** — per game, from `mods/` (library long-press "Mods & cheats"): XBE patches and replaced disc files; 60 and 120 fps mods for 13 games (`mods/README.md`, generators in `tools/mods`)
- **Controllers** — each physical controller routed to a player with its own mapping; the on-screen controller adds to player 1

### New features
- **Updates** — hakuX looks for a newer release on GitHub when it starts and on demand (Settings → Updates); one found shows its notes and installs from there
- **Frontends** — a game launched from ES-DE or another frontend returns to it when it closes
- **Console clock** — follows the phone's time and time zone, or a time set by hand (Settings → Console → Clock)
- **Start errors** — a game that can't start says why (the file can't be read, isn't an Xbox disc, or its program can't load) and goes back to the library
- **Mods** — Halo 2's 60 fps mod draws characters, vehicles and objects at 60; DOA2's 120 fps mod keeps the select screen's legend

### Performance
- **No pipeline-cache stutter** — each game keeps its own pipeline cache, written only after new compiles; the one shared by every game was rewritten every few seconds and froze the picture for 0.2–0.8 s (Ninja Gaiden Black)
- **Audio** — about 32 ms of latency instead of 128 ms, and less CPU for the sound engine
- **GPU emulation** — faster push-buffer decoding, texture unswizzling (NEON) and hashing, fewer lookups per draw
- **Savestates** — compressed on several threads (a Halo 2 save: 9.6 s to 3.9 s)
- **Idle frames** — the screen isn't redrawn when nothing changed, and the on-screen controller redraws only what changed
- **Disc reads** — a large read waits only its own thread, not the whole game
- **Library** — folders scanned in one pass (38 games: 270 ms to 63 ms)

### App
- **On-screen controller** — the menu button is a green dot
- **Large screens** — Android 16 keeps the landscape lock (the app is declared a game)
- **Storage** — JIT symbol maps left by earlier builds (gigabytes on some devices) are deleted at start
- **Backups** — hakuX's data is left out of Android's cloud backups and device transfers
- **Library** — savestate thumbnails and driver ZIPs load off the main thread

## v0.3.1

### New Features
- **Persistent log capture** — logs kept per session, rotated

### Improvements
- **Texture decompression** — S3TC decoded on several threads; 3D textures no longer use native BC (it corrupted BC3)
- **Surface eviction** — lazy, without a GPU finish per eviction, and no download for VRAM data that is never read
- **Texture cache** — no thrashing from an aggressive memory budget trim
- **Profiling** — all instrumentation behind `NV2A_PERF_LOG` (off by default)
- **XISO conversion** — extract-xiso replaces the Rust xdvdfs converter (no Rust/Cargo dependency)

## v0.3.0

### New Features
- **Per-game settings** — overrides per game in the settings UI
- **Xbox dashboard** — dashboard management, HDD tools, Insignia networking (NAT)
- **Texture dump and replacement**
- **Game compatibility quirks** — a per-game layer (Fable's cycle-breaker)
- **Debug log export** — from the settings
- **Skip boot animation** toggle

### Improvements
- **Vulkan** — native BC texture upload when the GPU has it, batched render sync, NEON blits, descriptor management without bindless textures
- **Vertex shaders** — the emulator stub replaced; uniform uploads optimized
- **Audio** — aligned with x1_box
- **Emulator process** — runs separately again (`:xemu`)
- **XISO** — the original ISO kept after conversion

### Bug Fixes
- Surface eviction VRAM corruption and stale texture sampling
- Vulkan pipeline exhaustion and eviction crashes
- A crash on a quick emulation restart (RCU)
- The 30 fps trap
- XISO conversion launching the original file when SAF renamed the output

## v0.2.1

### Bug Fixes
- **Fix HDD corruption** — restore block device flush on app terminate and background to persist QCOW2 metadata. Cache partition clear now uses synchronous writes
- **Fix game freeze on TB flush** — disable post-flush rewarm on Android which could hang on self-modified code
- **Fix TB cache prewarm crash** — disable startup prewarm on Android. Add build hash to tb_cache.bin for auto-invalidation on new builds
- **Fix on-screen controller hidden on some devices** — filter out virtual/internal devices (accelerometers, gyroscopes) that falsely report as gamepads

### Improvements
- **Simplified FPS overlay** — shows only FPS count
- **Unified log tags** — all logcat output uses hakuX prefix. Filter with: `adb logcat | grep hakuX`
- **HDD cache clear on background thread** — prevents ANR on large images

## v0.2.0

### New Features
- **OpenGL ES renderer** — experimental alternative to Vulkan, selectable in Graphics settings
- **EEPROM editor** — change Xbox language, video standard, resolution modes, aspect ratio, refresh rate
- **Settings index** — reorganized into 4 sections: hakuX Data, Graphics, Debug, EEPROM
- **System file pickers** — change MCPX ROM, Flash ROM, HDD image from settings
- **HDD export** — save a copy of the Xbox HDD to any folder
- **Games folder in settings** — moved from library screen to settings
- **Game search** — search bar with live filtering in game library
- **Pull-to-refresh** — swipe down to rescan games folder
- **XISO progress bar** — real progress tracking across copy/convert/save phases
- **XISO integrity check** — verifies XISO before launch, auto-rebuilds from ISO if corrupt

### Improvements
- **Controller overlay** — D-pad/stick repositioned, LS/RS as separate buttons, menu button, swipe-up gesture, trigger axis fix (-1.0 for release), per-pointer tracking
- **Pause menu** — actually pauses emulation, simplified UI
- **NEON optimizations** — S3TC decompression and texture swizzle (benefits both renderers)
- **XISO conversion** — staged in temp directory, clean up on interruption, write permission check
- **FPS overlay** — shows driver info or "OpenGL ES" label, frame pacing, shader stats
- **Clear code cache** button in Debug settings
- **Graceful VK texture LRU handling** — skip instead of crash when cache exhausted

### Bug Fixes
- Fix TB cache crash when FP settings change between sessions
- Fix pause/resume not working from in-game menu
- Fix GLES geometry shader crash (gl_PointSize unavailable without extension)
- Fix GLES shader version detection (was hardcoded to 320, now auto-detected)
- Fix shader link failure crash (graceful fallback instead of assert)
- Fix shared GLSL changes breaking VK (gated behind renderer check)

### Rebranding
- Package renamed to `com.rfandango.haku_x`
- Debug builds install alongside release (`com.rfandango.haku_x.debug`)

## v0.1.0

- Initial release

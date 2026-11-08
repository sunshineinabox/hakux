# hakuX libretro core for PortareOS

Status: design targets. The initial core and Vulkan handoff merged in PR #10.
The core stops, unloads and loads again in RetroArch's process (M2); ports
1-4 are players 1-4; rumble and save states remain unfinished. See [native/README.linux.md](native/README.linux.md) for current usage.
Written: 2026-10-24, from the owner's draft and its review against the source.
Target: Retroid Pocket Nova (SM8550), AArch64 Linux, PortareOS, RetroArch
Vulkan on KMS.
Related: [issue #8](https://github.com/portare-ch/hakuX-lr/issues/8) (VRR,
timed presents and the kernel's LFC).

## 1. Decision and result

Build hakuX as `hakux_libretro.so` and let RetroArch present, play the sound,
read the controllers and provide the menu, hotkeys, remapping and per-game
overrides. The Xbox emulation, FEX execution, Vulkan renderer and MCPX APU are
kept as they are: hakuX already plays BLACK and Burnout 3 on PortareOS with
good picture and sound. This integrates the working emulator into PortareOS's
RetroArch path; it is not another port.

Issue #8's aim comes with it: play at the emulated video rate through
PortareOS's existing timed presents, VRR and kernel LFC. No standalone VRR
work is a prerequisite.

**Frame model (owner's decision, 2026-10-24): phase-locked, not lockstep.**
`retro_run` is the display's vsync for hakuX's existing vblank lock, and the
emulation keeps running on its own threads as it does now. Strict lockstep
(one `retro_run` runs exactly one emulated period and parks every thread
that changes guest state) is out of scope: its payoffs are run-ahead, rewind,
netplay and exact frame advance, and none is wanted for the Xbox. Run-ahead
in particular is too much overhead.

## 2. Baseline and verified constraints

| Component | Revision |
| --- | --- |
| portare-ch/hakuX `master` | `93bad3d7fe2261e65adc9b3e69602e175d8d7da7` |
| hakuX's FEX (`external/fex`) | `52018bf46a8279b22bce738548ab3f40db467064` |
| portare-ch/portareos `next` | `321912638871a90caf8cf517ff4e277b63df422b` (portareos#798) |
| RetroArch in PortareOS | `11e8931021eb6b7fa69c04f52dd5d385841fdbbe` |
| `libretro-common/include/libretro_vulkan.h` there | blob `2a47d0ad583d8f14ffbf52bd7adb25faed1361d7`: render interface 5, negotiation interface 2 (`create_instance`, `create_device2`) |

Vendor the libretro headers from that RetroArch revision; they govern what the
target supports.

| In the source | Consequence |
| --- | --- |
| `native/dev/nv2a.c`: a detached, endless vblank thread at the Xbox's exact 60000/1001 Hz (16683333 ns and a carried third; 50 Hz mode, optional multiplier). It locks its period and phase to an outside vsync (`nc_dev_nv2a_display_vsync`, `nc_dev_nv2a_display_pickup`), which Android's Choreographer feeds (`native/app/vsync.c`). | `retro_run` feeds the same lock (section 5). |
| `native/core/irq.c` / `native/kernel/kernel.c`: `nc_pause_set` pulls the vCPU into the kernel (FEX's `InterruptPending`, checked at block boundaries, so a tight loop is reached too), parks it at `nc_kernel_pause_point`, parks the registered device threads in `nc_pause_wait`, and keeps the guest's kernel time and RDTSC (`CycleCounterOffset`) from advancing. | The menu's pause and frame gaps use it. Check which threads are not registered (the HLE render, watcher and timer threads). |
| Both front ends end with `_exit`; nothing destroys the FEX context or thread (no `nc_cpu_destroy`). FEX has `DestroyThread`, a virtual `~Context` and `Config::Shutdown` (its config layers are process-global statics). `EnableExitOnHLT` makes `ExecuteThread` return. | A stoppable core is the largest single work item (section 6). |
| Detached endless threads: vblank, AC'97 PCM, OHCI frame, kernel clock, I/O reader and closer, HLE render, watcher and timer, APU drain/reopen/trace; joinable APU frame and voice workers. | Each needs a stop, a wakeup and a join. |
| `native/core/vm.c` reserves a relocatable 4 GiB guest range with `mmap(NULL, ...)` and maps aliases inside it with `MAP_FIXED`. | No fixed host address. Release the reservation and the memfd on unload. |
| `hw/xbox/hle/vk/vkdev.c` creates its own instance and device; `swapchain.c` presents into the window. The Adreno 740 has one graphics queue (family 0; family 1 is sparse binding only). | Negotiated device, offscreen rendering, `set_image`, and RetroArch's queue lock around every submission (section 7). |
| `native/dev/apu/out_sdl.c` is the APU's output: a ring drained by the device's callback, a timed drain without a device. | A libretro sink, drained by `retro_run` (section 8). |
| `native/state/save.c` saves to a path and loads into a fresh boot of the state's XBE; `cache.c` keeps cache-drive contents in a `blobs/` folder beside the states. | Save states are their own milestone (section 11). |
| PortareOS's `retroarch.cfg` ships `run_ahead_enabled = "true"`, `run_ahead_frames = "1"`, `run_ahead_secondary_instance = "true"`, `core_info_savestate_bypass = "false"`, `menu_pause_libretro = "true"`, `pause_nonactive = "true"`. | Run-ahead must be switched off for hakuX (section 10). The menu stops calling `retro_run`. |
| `runemu.sh`, `setsettings.sh` and RetroArch patches 0014, 0018, 0019 and 0021 implement the launch policy and timed presents. | Route Xbox through them; no display policy in the core. |

Android is outside this work, as in the PortareOS handoff.

## 3. Scope

First playable core: load, run, unload and load again in one process; picture
through the Vulkan image handoff; 48 kHz stereo; port 0 playing; the menu's
pause; PortareOS's hotkeys; VRR and fixed-refresh checks.

Integration: controller ports 0-3, reset and title relaunch in process, the
data location settled, the package and the launcher entry.

Save states: a separate milestone (section 11). Until it passes, the core
reports serialization unsupported.

Out of scope: strict lockstep, run-ahead (never, for the Xbox), rewind,
netplay, exact frame advance, fast-forward beyond what the phase lock gives,
standalone timed presents, new accuracy work, Direct Video, software
rendering, other platforms. CHD images (`chdman createdvd`) are read in
place through libchdr (`native/core/chd.c`); the core never extracts discs
or calls xemu.

## 4. Ownership and layout

| Responsibility | Owner |
| --- | --- |
| Xbox CPU, devices, interrupts, guest time, the vblank clock | hakuX |
| The vblank's phase (locked to `retro_run`), pause, stop, reset and relaunch | hakuX shared core |
| Rendering, the title's gamma, guest video state | hakuX renderer |
| Scaling, aspect, shaders, swapchain, timed presents | RetroArch |
| Display mode, VRR policy, Mesa's environment, the DRM master | PortareOS's launch path |
| Refresh repeats | the display driver (kernel LFC) |
| PCM | MCPX APU |
| Audio device, latency and rate control | RetroArch |
| Controllers, remapping, hotkeys | RetroArch |

Suggested files (names final in the implementation):

```
native/libretro/core.c        the libretro ABI and lifecycle
native/libretro/video.c       Vulkan negotiation and the exported images
native/libretro/audio.c       the APU's libretro sink
native/libretro/input.c       RetroPad to NcPadState
native/cmake/libretro.cmake   the shared-library target
native/core/lifecycle.c       stop, join and destroy, below both front ends
tests/native/libretro/        a small frontend harness
```

Shared mechanisms go below the front ends. The Linux standalone `hakux` was
dropped in 0.4.0, as it wasn't tested; the core is the Linux front end.

## 5. Phase-locked frames

`retro_run`, on RetroArch's thread:

```
poll input; publish each port's NcPadState
nc_dev_nv2a_display_vsync(now): RetroArch's frame tick is the vsync
resume if the emulation was paused for a gap
drain the APU's ring into audio_batch_cb
take the newest front frame: nc_dev_nv2a_display_pickup(now), export it
video_cb exactly once (a new frame, or the last one again)
```

- The rate is the real Xbox's, exactly (owner's requirement): 60000/1001 Hz
  for the 525-line output, 50 Hz when that mode is set. The vblank thread
  carries the fraction of a nanosecond, the core reports exactly that rate,
  and the audio takes 48000 x 1001 / 60000 = 800.8 frames a call through an
  exact accumulator. RetroArch calls `retro_run` at the reported rate; with
  one tick per frame the vblank thread locks to it (k = 1) and places
  vblanks half a period from the pickups, as it does with Choreographer.
- The guest keeps its pipelining: the CPU, the render thread and the export
  overlap as they do today. `retro_run` never waits for the guest to finish
  a frame; a 30 fps title or a static screen shows its last frame again.
- A gap (the menu, a long stall): when no `retro_run` comes for two periods,
  `nc_pause_set(true)`; the next `retro_run` resumes. The guest's time leaves
  the pause out, as on Android. A pause that leaves the vCPU running is a bug.
- Frame advance gives about one period of emulation; it is not exact.
- The uncapped push-buffer mode (`HAKUX_HLE_UNCAP`) and refresh multipliers
  stay off in the core until AV info renegotiation is done for them.
- A mode change (50/60 Hz) updates `retro_system_av_info` through
  `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` before frames come at the new rate.
- A stalled guest is not hidden: no synthetic frames beyond repeating the
  last one, no extra vblanks. A 5 s watchdog without a new frame logs the
  CPU, renderer and vblank state; it does not kill anything.

## 6. Stop, unload and living in RetroArch's process

Add request-stop, join and destroy; make them idempotent and valid after
every partial boot. States: EMPTY, LOADING, RUNNING, STOPPING, EMPTY. A later
load in the same process works without `exec`.

Stop must reach: a CPU in a tight loop (the interrupt flag), a CPU idle in
the kernel, the vblank, AC'97, OHCI and kernel clock threads, the renderer's
queues and timeline waits, APU producers and voice workers, disc I/O and
diagnostic threads. Use FEX's own exit (`ExitOnHLT` or a narrow FEX change),
never `pthread_cancel` or a jump across FEX's C++ frames.

Teardown, in order: refuse new frames and request stop; join every thread
that can touch guest memory, output buffers or core code; finish GPU work and
retire the images RetroArch holds (section 7) without holding locks it needs;
flush and close the hard disk and disc; destroy core-owned Vulkan objects,
the FEX thread and context (`DestroyThread`, `~Context`, `Config::Shutdown`),
the guest reservation and memfd, registries and queues; clear per-content
globals.

No `_exit`, `exit`, `exec` or `abort` as an error or unload path; audit FEX's
assertion handlers. If a thread cannot stop, unload must not return: that
build fails the gate.

If FEX's context cannot be destroyed and created again reliably, one context
for the process's lifetime, reused across loads with no thread running, is
an acceptable fallback; record the evidence.

Signals: handle only faults positively identified as hakuX's (guest RAM
tracking, MMIO, the JIT); chain any other to the previous handler with its
flags and mask; install while the core's state is alive, restore only if
still ours. Test deliberate faults in child processes.

The core stops in RetroArch's process (M2). Unloading, a reset, a title's
relaunch and RetroArch letting its Vulkan context go (`context_destroy`)
stop the console, join its threads, free the renderer, the guest and FEX,
and give the signal handlers back; the next load, reset or relaunch boots in
the same process.
- Large buffers a boot keeps (the guest's page table, the NV2A's and the
  misc registers, the APU's state, the push-buffer decoder's arrays, the
  front end's ring) are mapped, not taken from the heap (`nc_big_alloc`,
  `hle_big_alloc`). glibc keeps a freed heap block in the arena it came
  from once its mmap threshold has risen, and each boot's threads get other
  arenas: every boot would leave a copy.
- glslang's process state (its built-in tables) goes with the renderer and
  is built again at the next boot.
- The heap is trimmed at the end (`malloc_trim`, glibc): a boot's threads
  free into arenas of their own, which keep the pages, and the next boot's
  threads get other arenas.
- The library stays mapped after RetroArch's `dlclose` (`RTLD_NODELETE`).
  FEX's allocator (rpmalloc) keeps its spans for the process, with its state
  in the library, and FEX never finalizes it: unloaded, each load left them
  behind, about 1 GiB of address space (measured). Kept, the next load
  reuses them and finds the statics as the stop reset them.
- A video driver rebuild boots the game again on RetroArch's new device,
  with a message saying so. The core doesn't let RetroArch cache the
  context (`cache_context`): a cached reinit kept the device but rebuilt
  RetroArch's driver, freeing the render interface and the queue lock's
  handle the core held, and RetroArch crashed (`VIDEO_REINIT` on the Nova).
- Known leftovers: FEX's `LookupCache` leaves its block-link buffers mapped
  (about 300 KiB a boot on Burnout 3: `named_monotonic_page_buffer_resource`
  has no destructor), and glibc and the Vulkan loader keep a few KiB a
  boot for the driver they load and unload.

The harness checks it (`tests/native/libretro/check.py`): 100 cycles of a
title that halts and 20 of one that spins in a tight loop, with and without
the renderer, on the harness's own Vulkan device and on a shared one, and
boots made to fail after each stage (`NC_BOOT_FAIL_AT`: the kernel, the
disc, the XBE, the CPU, the boot thread, the HLE GPU), each leaving one
thread, the same descriptors and signal handlers, and memory within a
margin.

PortareOS's build (`HAKUX_LR_END_PROCESS`, on in its package) may end
RetroArch's process, as every closed game did before M2:
- A stop that takes longer than 10 s ends it, rather than leave the
  handheld frozen; the log names what the stop was waiting for.
  `HAKUX_LR_STOP_TIMEOUT_MS` sets the time (the tests). Other builds wait,
  as above.
- `HAKUX_LR_STOP=0` in the environment brings back the way before M2, for
  one release: unloading, or RetroArch letting its Vulkan context go, ends
  RetroArch's process. A game relaunch reuses the original command line
  (#11); if it doesn't contain the game's path, the process ends instead.
  A dashboard relaunch always rebuilds the command line, even if it already
  named the core. It keeps only `-c/--config`, `-s/--save`, `--appendconfig`,
  `--log-file`, `-v/--verbose` and `-f/--fullscreen`, including joined
  `-cFILE`, `-sDIR` and `--opt=value` forms. It drops everything else
  (content, other `-L` options and `--menu`), then adds `-L <core>`.
  This also handles cores loaded through **Load Core → Start Core**.
  Other builds ignore `HAKUX_LR_STOP=0`.

`HAKUX_LR_END_PROCESS=0` or `1` in the environment overrides the build's
choice for one run (the tests).

## 7. Vulkan

Request `RETRO_HW_CONTEXT_VULKAN` and use the negotiation interface's
`create_device2`: hakuX creates the device on RetroArch's instance and
physical device with its required features and extensions, merged with
RetroArch's through the wrapper; optional features stay optional. A device
without the required features stops loading with a message.

The instance and the device belong to RetroArch once handed over; keep them
apart from core-owned objects in every destruction path. Use RetroArch's
queue, and its `lock_queue`/`unlock_queue` around every submission from the
renderer and the timeline code; never hold the lock while waiting on
RetroArch or calling it.

In the core build there is no window, surface, swapchain, presenter sleep or
`vkQueuePresentKHR`. Copy the front display slot into one core-owned export
image per sync index (a GPU pass), call `set_image`, then
`video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0)`. RetroArch times and
presents it; the core sets no present time and no Mesa environment.

Honour the header's contract: image usage, format and layout; barriers and
the readiness semaphore; queue family ownership; per-sync-index retention
(`wait_sync_index`) before an image is overwritten or destroyed; recreation
when the sync mask or context changes.

Apply the Xbox's gamma ramp in the export pass, as the presenter does in
present.c. Preserve the source size and aspect ratio; RetroArch handles
letterboxing, so do not pad the image to 1280 x 960.

With Crop Overscan enabled, remove 3/80 (3.75%) from each edge of a
standard-definition frame. This gives whole-pixel crops at 640 x 480 and
720 x 480 at every render scale: 640 x 480 becomes 592 x 444, exactly 4:3.
It also hides Burnout 3's 16 black lines at the top and bottom. Adjust the
reported aspect ratio for the crop to preserve pixel proportions when
rounding is needed, such as with 576-line PAL frames.

`context_destroy` stops the core before its resources are released (section
6). `context_reset` rebuilds them or stops the content with an error.

CPU readback (`hle_disp_read_front`) is for bring-up and diagnostics only.

## 8. Audio

A libretro sink for the APU's output contract (`apu_out_open`, `push`,
`queued`, `stats`, `nc_audio_set_paused`): no SDL, no device, no timed drain
thread. 16-bit stereo at 48000 Hz. `retro_run` drains what the ring holds
into `audio_batch_cb` (looping on partial acceptance), which paces the APU's
producer as a device's callback does now; RetroArch's rate control absorbs
the jitter. Start with at most two periods of ring; measure occupancy and
grow only with evidence. Paused: the ring keeps its contents and grows no
further. Reset clears it.

Player 1's Xbox Live Communicator (`hakux_communicator`, off by default) has
its headphone mixed into the same output, 3 dB down in each channel, after
the ring is read: the APU's pacing never sees it. Its microphone is
RetroArch's (`RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE`), read in
`retro_run` only while the title moves the Communicator's audio, and closed
when the title stops, the core stops, or the Communicator is gone; without
one the title hears silence.

## 9. Controls

Read RetroPad state once per `retro_run` and publish it per port.

| Xbox | RetroPad |
| --- | --- |
| A / B / X / Y | B / A / Y / X (south, east, west, north) |
| White / Black | L / R |
| Left / right trigger | L2 / R2: analog where RetroArch gives it, else 0/255 |
| D-pad | D-pad |
| Start / Back | Start / Select |
| Thumb clicks | L3 / R3 |
| Sticks | left and right analog, Y inverted, clamped |

Ports 0-3 are separate players, never merged into player 1. Each port's
types are "Xbox Controller" (`RETRO_DEVICE_JOYPAD`) and "None": a port has
a controller plugged in unless RetroArch sets it to None, so by default all
four are, as RetroArch puts a RetroPad on every port. RetroArch sets the
ports after the guest started to boot (`retro_set_controller_port_device`
after `context_reset`), and again when its menu changes one: before the
controllers are made it decides which are, after it the controller is
plugged into its root port or pulled out while the console runs, as on the
hardware, and the title enumerates it again. The ports stay so for a reset,
a relaunch and a video driver rebuild. Rumble goes through RetroArch's
rumble interface on its thread; it not working must not stop play (the
standalone's SDL rumble did not vibrate through InputPlumber's virtual pad).
Home + Start stays PortareOS's; the core adds no exit chord.

## 10. ABI, options, paths and the core info

| Entry point | Behaviour |
| --- | --- |
| `retro_get_system_info` | name, version; `need_fullpath = true`, `block_extract = true`; extensions `iso` and `chd` |
| `retro_load_game` | check the image, resolve paths, negotiate Vulkan, boot; false with full cleanup on failure |
| `retro_get_system_av_info` | 60000/1001 (the Xbox's, exactly), 48000 Hz, the source size and aspect |
| `retro_run` | section 5: one `video_cb`, at least one input poll |
| `retro_reset` | stop and boot again in process |
| `retro_unload_game` | stop, join, destroy; a later load works |
| `retro_deinit` | release the rest; `dlclose` is safe |
| serialization | size 0, false, until section 11 passes |
| `retro_cheat_set`, `retro_cheat_reset` | the codes of `patches/mods/README.md`'s cheats, or `AAAAAAAA VVVVVVVV`, joined by `+`, blanks or newlines; a set apart from the mods' cheats, from the next vblank, also before the guest boots; kept over a reset or relaunch, cleared on unload; a code the core can't read is logged once and stays off; an `xbe-patch-v1` code is an XBE patch instead (`native/core/xbe_patch.h`, `native/README.linux.md`): applied at the next boot to the disc XBE whose CRC-32 and size it names, all its runs or none, none when two match; a change shows a reminder to restart |
| memory, special content | unsupported |

Run-ahead stays off for hakuX, now and after section 11:
`hakux_libretro.info` declares `savestate = "false"` until states work, and
the Xbox system ships a RetroArch override with `run_ahead_enabled =
"false"`. RetroArch must never load a second hakuX instance into the process.

Options (`options.c`): only what the emulator backs.
- The render scale (1-4x), and the overscan crop (on by default; off shows
  the whole frame).
- The console's own settings, as its dashboard keeps them: screen format
  (normal, widescreen, letterbox), language (auto: RetroArch's) and video
  region (auto: the game's; NTSC-M, NTSC-J, PAL). PAL60 is always set, as
  RetroArch runs at 59.94 Hz; 480p, 720p and 1080i stay off.
- The network (`net.c`): off (no cable); LAN, using upstream's system link
  backend with broadcasts on the default route's interface and peers listed
  in `lan-peers.txt` in the save directory; or Xbox Live through Insignia,
  using upstream's NAT backend with Insignia's DNS server or the first
  address in `insignia-dns.txt` in the save directory. XLink Kai and the
  Xbox dashboard, needed to create or recover Insignia accounts, are not
  available yet.
- Frame pacing (`hakux_frame_pace`, rfandango/hakuX#14), off by default:
  after each swap the push-buffer decoder waits until the GPU has finished
  the swap two (On) or one (Lowest Latency) before it, so a game whose
  rendering falls behind its simulation slows to the render pace instead of
  queueing frames as input lag. The wait is on the renderer's timeline, not
  on `retro_run`: RetroArch's menu, a pause or a frame it never takes can't
  hold it, and the stop's kick ends it. RetroArch's pace already reaches the
  guest through the vblank lock (section 5) and the title's FLIP_STALL.
- The Xbox Live Communicator (off by default; experimental): a headset in
  expansion slot A of player 1's controller for voice chat, with its
  microphone's noise-gate sensitivity (medium, low, high, or off).

Insignia's NAT backend forwards the console's UDP 3074 through the device's
UDP 3074 and requests a router forward through UPnP for Open NAT. Without
UPnP, forward that port to the device manually. Native overrides
`natport=`, `upnp=` and `upnplease=` are documented under
[Insignia NAT](native/README.linux.md#insignia-nat).

All but the crop, rumble and frame pacing are read before the console
boots, so a change takes effect when the game starts next, and RetroArch
shows a message saying so. The crop changes from the next frame, frame
pacing at the decoder's next swap. No display, VRR, mapping or filter
options, and no latency option but frame pacing (off until it is measured
on the Nova): FXAA and the picture's shape are RetroArch's shaders and
aspect setting, and SGSR is not offered (owner, 2026-10-25).

Data: the console's hard disk and caches live in the save directory
RetroArch gives (`hdd/`, `cache/`); caches can be rebuilt, the disk cannot.
On PortareOS that is `/storage/saves/xbox`, beside `/storage/roms/xbox` and
`/storage/bios/xbox` (owner's layout, 2026-10-24): RetroArch's save
directory for the core is set to it. The old standalone's
`/storage/.config/hakux` (since portare-ch/portareos#752) moved there once. A lock keeps two sessions off the same disk. No `chdir`, no
`setenv` for paths after load: the renderer's mode and cache directory are
set through `nc_hle_set_mode` and `nc_hle_set_base_path`. A title's relaunch stays in process (with `HAKUX_LR_STOP=0`, in PortareOS's build RetroArch starts again in its place, #11; in others the game closes).

## 11. Save states

Unsupported in the first playable core. To enable them: a stream/buffer form
of capture and restore below the path-based one; capture from the pause
without an extra frame; restore by booting the state's XBE in process and
applying the checked sections; a self-contained buffer with every cache blob
it needs (the disc stays external, identified and checked); a size bound for
the session; checks before anything live changes; derived GPU and JIT state
rebuilt, queues cleared. Prove it in a fresh process with a fresh cache
folder. Run-ahead stays off (section 10).

## 12. Issue #8

| Issue #8 | Here |
| --- | --- |
| Vsync off, no beat between two clocks | No presenter in the core; the vblank locks to RetroArch's ticks, which 0018/0019 time at the core's rate. |
| `MESA_VK_WSI_DISPLAY_VRR` and its opt-outs | `runemu.sh` with the emulator `retroarch` and the Xbox core registered: its capability check, `vrr=0` and `display_mode`. |
| Fastest panel mode | `setsettings.sh`; check about 120.198 Hz on the Nova. |
| Timed presents | RetroArch and Mesa; no second clock in hakuX. |
| Integer repeats | The kernel's; check against the actual submissions. |
| Burnout 3 for minutes without a beat | 10 minutes through the launcher, with step, image, present and flip traces. |
| `vrr=0` | Fixed refresh, and no `VRR_ENABLED` left behind. |

0021 notes that the display offers FIFO only and keeps presents blocking
under Sync to Exact Content Framerate: the goal is no independent pacing
clock and no fixed-refresh beat, not a ban on FIFO. A 30 fps title gives one
`retro_run` per 59.94 Hz period with its image repeated, so the kernel shows
each submission twice; that is expected. The standalone's `start_hakux.sh`
part of #8 is superseded by this route, as the issue records.

Measured on the Nova on 2026-10-25 with 0.2.1, through `runemu.sh`, with
flip traces (PORTAREOS_HANDOFF.md, "Issue #8"):
- Burnout 3, 10 minutes on the 120.20 Hz mode with VRR: every frame after
  the first second was shown twice but one, at about 119.88 Hz. Its mean
  interval was 16.6830 ms against 16.6833 ms, and 59.9 new frames reached
  RetroArch each second.
- BLACK showed each of its 30 frames a second for four refreshes.
- `xbox.vrr=0` gave the 119.88 Hz mode with `VRR_ENABLED` 0, and every
  frame was shown twice.

## 13. Build and PortareOS

`tools/native/linux-build.sh` builds `hakux_libretro.so` with all linked
libraries as PIC. See the [Linux build and packaging instructions](native/README.linux.md).

The object exports only `retro_*`, needs no SDL, Android or build-tree
RUNPATH, and embeds no Vulkan loader. PortareOS packages it as `hakux-lr`
from a release: `/usr/lib/libretro/hakux_libretro.so` and its `.info`
(RetroArch loads them from `/tmp/cores`), the SM8550 dependency, and the Xbox
core registration; check the generated `es_systems.cfg`. The release tarball
holds only the core since 0.4.0.

## 14. Milestones

| | Deliverable | Gate |
| --- | --- | --- |
| M0 | The shared library builds; a harness loads it; an inventory of threads, mappings, handlers and FEX globals; a `create_device2` probe | It loads into a frontend in process and exports only `retro_*`. |
| M1 | First picture in RetroArch: phase-locked `retro_run`, the libretro audio sink, port 0, the menu's pause | BLACK and Burnout 3 play in RetroArch on the Nova (CPU readback allowed here). Until M2, unloading ends RetroArch's process; the owner chose to package it before M2 (2026-10-24), as quitting a game ends RetroArch on PortareOS anyway. Until then RetroArch letting its Vulkan context go ends the process too, and a title's relaunch starts RetroArch again in its place, on the same command line (#11). That is PortareOS's build (`HAKUX_LR_END_PROCESS`); other builds parked the emulation and kept RetroArch running, until M2. |
| M2 | A stoppable core | 100 load/run/unload cycles in one process; a CPU in a tight loop stops; failure at each boot stage cleans up; no threads, mappings, fds or handlers left. Status (2026-10-25): the cycles pass in the container's harness and on the Nova (Burnout 3, 100 and 200 cycles, renderer on), with the leftovers section 6 lists, and boots that fail at each stage clean up (the container). Android is still to do. |
| M3 | Vulkan handoff | `create_device2`, `set_image`, the queue lock, gamma; no readback; context reset works or fails cleanly. RetroArch only negotiates when the v1 `create_device` is set too (it then calls `create_device2`). |
| M4 | PortareOS integration and #8 | The package, the launcher entry, the data folder, ports 0-3, reset and relaunch, run-ahead off; the device checks below. |
| M5 | Save states | Section 11. |

Device checks: Burnout 3 for 10 minutes at full speed with about 119.88 Hz
doubled scanout; BLACK's 30 fps with its image reused and correct speed and
sound; `vrr=0` and a pinned mode; 100 pause/resume cycles and a 60 s pause
with no time jump or stale sound; exit, reset and reload from boot, play and a
static screen; another Vulkan core (ARMSX2) loaded after hakuX in the same
process; 30 minutes of sound without drift or growing queue; controls,
remapping and disconnects. Compare with `hakux` (standalone) on the same
image and segment: a repeatable regression over 5% in frame time is
investigated, and a full-speed segment that becomes slow blocks promotion.

## 15. Done

The playable core is done when RetroArch loads, plays, pauses, stops and loads
hakuX again in one process, with today's picture and sound, through
PortareOS's display path. Integration is done when the package, launcher,
data folder and #8's device checks pass. None of it counts with RetroArch
forced to exit, a detached core thread left running, the standalone's
presenter or audio clock still in use, or serialization claimed with
undisclosed local blobs.

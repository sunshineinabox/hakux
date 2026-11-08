# hakuX-lr

hakuX-lr is an original Xbox emulator for RetroArch on ARM64 (aarch64) and
AMD64 (x86-64) Linux, with an experimental build for RetroArch on Android. It
is based on [hakuX](https://github.com/rfandango/hakuX) and comes with
[PortareOS](https://github.com/portare-ch/portareos) on the Retroid Pocket Nova.

See the [game compatibility list](https://hakuxlr.portareos.org/) for per-title results.

You need your own game images. To play games, you do not need a BIOS,
MCPX ROM or Xbox hard disk image. hakuX creates its storage when you first
start a game.

## Install

<a id="play-on-portareos"></a>

### PortareOS

1. [Install PortareOS](https://github.com/portare-ch/portareos/blob/next/documentation/INSTALLATION.md).
   If it is already installed, use **Settings → About → Update** and restart.
2. Copy an Xbox `.iso` or DVD `.chd` to `/storage/roms/xbox/`.
   Extract ZIP or 7z archives first.
3. Open **Xbox** in the launcher and select a game. If the game does not
   appear, press **Home** to return to the console list, then open Xbox again.

To transfer games over Wi-Fi, enable **Settings → SSH** and connect with
an SFTP client. Use the IP address and password in **Settings → About**,
with username `root`.

<details>
<summary>Manually update the bundled core</summary>

Download `hakux-<version>-aarch64.tar.gz` from the
[Linux releases](https://github.com/portare-ch/hakuX-lr/releases) and extract
it. Copy `hakux_libretro.so` and `hakux_libretro.info` from `lib/libretro/`
to `/storage/cores/` on the device, creating the directory if needed, then
restart.

RetroArch will use these files instead of the bundled core, even after an
OS update. To return to the bundled version, remove both files and restart.
Back up your [saves](#saves) before changing versions.

</details>

<a id="play-on-other-aarch64-linux-systems"></a>

### RetroArch on other Linux systems

Install 64-bit RetroArch and zlib. Vulkan 1.4 is recommended; 1.3 is
also supported, and 1.2 works through a compatibility path that may be
slower. The Vulkan loader (`libvulkan.so.1`) must support 1.3 or newer.

The ARM64 core requires glibc 2.38+ and GCC 13's libstdc++. The AMD64 core
requires glibc 2.35+ and GCC 11's libstdc++ (Ubuntu 22.04, Debian 12 or
newer), plus the kernel and KVM requirements below.

Download the archive that matches your CPU:

| CPU | Release archive | Additional requirements |
| --- | --- | --- |
| ARM64 | `hakux-<version>-aarch64.tar.gz` | 64-bit ARM Linux |
| AMD64 | `hakux-<version>-x86_64.tar.gz` | Linux 5.16 or newer and [KVM setup](#play-on-amd64-x86-64-linux-systems) |

1. Download and extract the archive from the
   [Linux releases](https://github.com/portare-ch/hakuX-lr/releases).
2. In **Settings → Directory**, find RetroArch's **Cores** and **Core Info**
   directories. Copy `lib/libretro/hakux_libretro.so` into Cores and
   `lib/libretro/hakux_libretro.info` into Core Info.
3. Set **Settings → Drivers → Video** to **vulkan**. Choose
   **Main Menu → Configuration File → Save Current Configuration**, then
   restart RetroArch.
4. Set **Settings → Directory → Save Files** to a folder you can write to.
5. Choose **Load Core → Microsoft - Xbox (hakuX)**, then **Load Content**
   and select your `.iso` or `.chd` file.

Save states, rewind and run-ahead are not supported. Leave them off in
RetroArch.

<details>
<summary>Set up a test machine with one script</summary>

The setup script installs RetroArch, downloads the latest stable hakuX-lr
release for your CPU, and checks KVM access on AMD64. It supports pacman,
apt, dnf and zypper. It creates a separate RetroArch configuration for
hakuX, leaving your existing settings alone.

```sh
curl -fsSLO https://raw.githubusercontent.com/portare-ch/hakuX-lr/master/tools/linux/setup-hakux.sh
sh setup-hakux.sh
~/.local/bin/start-hakux.sh "burnout 3"
```

Put games in `~/roms/xbox`. The launcher accepts a file path or part of a
game's name. Run it without a game to open RetroArch's menu. It uses your
desktop session when available, or draws directly to the display through
KMS when no desktop is running.

Run the setup script again to update the core. Use `--version`, `--tarball`
or `--core` to install a specific release or local build; see
`sh setup-hakux.sh --help` for details. Settings and logs are kept in
`~/.local/share/hakux-lr/` by default. Put your RetroArch settings in
`user.cfg`; the script preserves that file when you update.

</details>

<a id="play-on-amd64-x86-64-linux-systems"></a>

#### AMD64: set up KVM first

The AMD64 build uses Linux KVM and requires kernel 5.16+, TSC scaling,
and UMIP. AMD Zen 2 and newer CPUs meet the CPU requirements; Intel KVM
can provide UMIP in software.

**Ubuntu 22.04:** Its glibc is compatible, but the original 5.15 kernel
is too old. Check with `uname -r` and use kernel 5.16 or newer. Ubuntu's
HWE kernel (6.8 on 22.04.5) is suitable. Install it with
`sudo apt install --install-recommends linux-generic-hwe-22.04`, then
reboot. Some Desktop installations already have HWE.

1. Enable **Intel VT-x** or **AMD SVM / AMD-V** in BIOS/UEFI.
2. Load the KVM driver: `sudo modprobe kvm_intel` for Intel, or
   `sudo modprobe kvm_amd` for AMD.
3. Run `ls -l /dev/kvm` to check its group. If the group is `kvm`, give
   your user access:

   ```sh
   sudo usermod -aG kvm "$USER"
   ```

   Log out and back in, then confirm this prints `KVM access OK`:

   ```sh
   test -r /dev/kvm && test -w /dev/kvm && echo 'KVM access OK'
   ```

Sandboxed RetroArch installations also need access to `/dev/kvm`. If a game
will not start, check the core's log for missing CPU or KVM features. Once
KVM is ready, follow the
[RetroArch installation steps above](#play-on-other-aarch64-linux-systems).

<a id="play-on-android"></a>

### RetroArch on Android (experimental)

The Android build is experimental and has seen little testing on devices.
It needs a 64-bit (arm64-v8a) device with Android 9 or newer, a GPU driver
with Vulkan 1.2 or newer (1.4 recommended, 1.3 supported), and RetroArch
from the Play Store
or [retroarch.com](https://www.retroarch.com/?page=platforms).

1. Download `hakux-<version>-android-arm64.zip` from the
   [releases](https://github.com/portare-ch/hakuX-lr/releases) and extract it
   on the device.
2. Set **Settings → Drivers → Video** to **vulkan**, then restart RetroArch.
3. Choose **Load Core → Install or Restore a Core** and select
   `hakux_libretro_android.so`.
4. Optional: copy `hakux_libretro.info` into RetroArch's **Core Info**
   directory (**Settings → Directory → Core Info**). RetroArch then shows the
   core as **Microsoft - Xbox (hakuX)** and knows which files it opens.
5. Choose **Load Core**, select the core, then **Load Content** and your
   `.iso` or `.chd` file.

Saves are kept in `hdd/` under RetroArch's **Save Files** directory. The
DSP JIT is Linux-only, so **Audio Processing** defaults to **Fast** (no Xbox
reverb or effects); **Full DSP** runs on a slower interpreter. Save states,
rewind and run-ahead are not supported. The core logs to Android's log
(`adb logcat`, tags starting with `hakuX`); include it when you report a
problem.

## Game formats

Supported formats are Xbox XISO images (`.iso`), full disc dumps (Redump
ISO), and DVD CHDs (`.chd`). **Convert full Redump ISOs to XISO before
creating a CHD.** Redump images include non-game partitions and padding;
compressing them directly can produce a CHD larger than a bare XISO.

The converter removes that overhead before CHD compression:

```sh
python3 tools/iso_to_chd.py "Game.iso"
# Optional: also keep Game.xiso.iso
python3 tools/iso_to_chd.py "Game.iso" --keep-xiso
```

Install [xdvdfs-cli](https://github.com/antangelo/xdvdfs) (the `xdvdfs`
command) and [chdman](https://docs.mamedev.org/tools/chdman.html) first.
The script runs `xdvdfs pack` followed by `chdman createdvd`; it keeps
the input ISO and removes the intermediate XISO unless `--keep-xiso` is
given. It refuses to overwrite existing files.

An XISO is not a bit-for-bit Redump archive; preserve your original dump
if you need one. hakuX reads the resulting CHD directly. CD CHDs made
with `createcd` and CHDs that depend on a parent image are not supported.

<a id="how-hakux-compares-with-xemu"></a>
<a id="how-this-differs-from-upstream-hakux-and-xemu"></a>

## About this fork

[Upstream hakuX](https://github.com/rfandango/hakuX) is an Xbox emulator for
ARM64 Android devices. This fork adds Linux builds, RetroArch support,
PortareOS packaging and an AMD64 CPU backend. For Android, use
[upstream's releases](https://github.com/rfandango/hakuX/releases).

Both projects use hakuX's native emulator core. Compared with
[xemu](https://xemu.app/docs/about/), the main differences are:

| | xemu | hakuX-lr |
| --- | --- | --- |
| Running Xbox code | Translates x86 code in software using QEMU's TCG by default. | Translates x86 to ARM64 using FEXCore, or runs it through KVM on AMD64. |
| Xbox kernel | Boots an Xbox kernel from a compatible BIOS. | Reimplements the Xbox kernel. |
| Graphics | Emulates the NV2A GPU with Vulkan or OpenGL. | Translates Xbox graphics commands to Vulkan. |
| System files | Needs a BIOS, MCPX ROM and hard disk image. | Creates storage automatically; no BIOS or MCPX ROM is needed. |

hakuX-lr uses its own [memory and device model](native/core/) rather than
QEMU's, while reusing components adapted from xemu, including the
[Xbox audio device](native/dev/apu/). For details about xemu, see its
[CPU setup](https://github.com/xemu-project/xemu/blob/master/system/vl.c),
[graphics backends](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/meson.build)
and [required files](https://xemu.app/docs/required-files/).

## Controls

RetroArch ports 1–4 correspond to Xbox players 1–4. Face buttons use the
Xbox layout. L1 maps to White, R1 to Black, and L2/R2 to the triggers.
To change the mapping, open **Quick Menu → Controls**. Vibration is off by
default; enable it under **Quick Menu → Core Options → Rumble**.

On PortareOS, **Home + Start** returns to the launcher.
**Home + the top face button** opens RetroArch's Quick Menu.

## Picture and audio

These settings are in **Quick Menu → Core Options**. Close and reopen the
game after changing them. **Crop Overscan**, **Rumble** and **Frame Pacing**
take effect immediately.

| Setting | When to change it |
| --- | --- |
| Render Scale | The default is **2x**. Try **1x** if a game runs slowly. Higher values sharpen the picture but need more GPU power. |
| Screen Format | Choose **Widescreen** for games that support it. Set RetroArch's aspect ratio to **Core provided**. |
| Crop Overscan | On by default to hide borders around the picture. Turn it off to show the full frame. |
| Combiner Precision | **Accurate** is the default on AMD64 Linux; **Fast** on ARM64 Linux and Android. Fast omits 9-bit combiner rounding to reduce GPU work. Restart the game after changing it. |
| Audio Processing | Defaults to **Full DSP (experimental)**, which runs Xbox audio effects. Try **Fast** if audio crackles or a game runs slowly. |
| Audio Resampler | Defaults to **Sinc**. Try **Linear** to reduce CPU use at the cost of some audio quality. |
| Frame Pacing | Off by default (experimental). If a game runs slowly and the controls feel delayed, **On** holds the game to the pace the GPU draws at, so it can't run ahead of the picture. **Lowest Latency** waits one frame less but may lower the frame rate. |

## Saves

Use the game's own save feature, as you would on an Xbox. Saves are kept
in `hdd/` inside RetroArch's save directory. On PortareOS, this is
`/storage/saves/xbox/hdd/`.

Back up this folder before reinstalling, updating the core or moving to
another device. RetroArch save states are not supported.

## Multiplayer

Choose a network mode under **Quick Menu → Core Options → Network**, then
close and reopen the game.

- **LAN (system link):** enable it on every device, then use the game's LAN
  mode. Discovery uses UDP port 3075. For another subnet or a VPN, add peers
  to `lan-peers.txt` in RetroArch's save directory
  (`/storage/saves/xbox/` on PortareOS), one `IP` or `IP:PORT` per line.
- **Xbox Live (Insignia):** register the console with
  [Insignia](https://insignia.live) using its Setup Assistant, then select this
  network mode. Account setup needs your own Xbox dashboard files;
  see the [dashboard setup](native/README.linux.md#the-libretro-core) and
  [NAT / port forwarding guide](native/README.linux.md#insignia-nat).
  Dashboard navigation and the complete Insignia setup process have not
  yet been tested on a device.
- **Xbox Live and system link:** both at once, as on a real console's home
  network. Each console gets its own address, so consoles on one network
  can play system link while signed in to Insignia.

XLink Kai is unavailable in RetroArch.

## Troubleshooting

| Problem | What to try |
| --- | --- |
| Game missing from the PortareOS launcher | Check `/storage/roms/xbox/`, extract archives, and return to the console list with Home. Update PortareOS if CHDs are missing. |
| Core missing in RetroArch | Check the Cores and Core Info directories, copy both release files, then restart. |
| Slow game or black picture | Select the **vulkan** video driver and restart RetroArch. Try **Render Scale: 1x** and **Audio Processing: Fast**. |
| AMD64 game will not start | Check [KVM setup](#play-on-amd64-x86-64-linux-systems) and the log for missing TSC scaling or UMIP. |
| Game asks for a controller | Set **Quick Menu → Controls → Port 1 Controls → Device Type** to **Xbox Controller**. None disconnects the port. |
| Game fails to start | Try the latest core and an XISO. For CHD, check that it was made with `createdvd`. |

When [reporting an issue](https://github.com/portare-ch/hakuX-lr/issues),
include the game and region, core version, device, operating system, and
steps to reproduce the problem. Collect the logs before starting another
game, as a new session may overwrite them:

- On PortareOS: `/var/log/exec.log` and `/var/log/retroarch/retroarch.log`.
- With the setup script: `hakux.log` and `retroarch.log` in
  `~/.local/share/hakux-lr/logs/` by default.
- With a manual RetroArch installation: capture the terminal output when
  starting the game, as described in the [testing guide](TESTING.md#logs-1).

## Development and license

[Build for Linux](native/README.linux.md) · [Testing](TESTING.md) ·
[Changelog](CHANGELOG.md)

The emulator core and Android app are the work of the upstream hakuX
contributors. This fork maintains the Linux and RetroArch integration.

Upstream hakuX code is licensed under [GPL-3.0-or-later](COPYING).
Original code from this fork marked `SPDX-License-Identifier: GPL-2.0-or-later`
is also available under [GPL-2.0-or-later](licenses/GPL-2.0.txt).
The combined hakuX-lr core is distributed under GPL-3.0-or-later.
Third-party code keeps its own licenses, listed in [LICENSE](LICENSE)
and [licenses/](licenses/).

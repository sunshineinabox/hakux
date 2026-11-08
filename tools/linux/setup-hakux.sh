#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# Sets up RetroArch with hakuX-lr for testing on an ARM64 or AMD64 Linux
# machine, then writes start-hakux.sh, which starts games with it.
#
#   sh setup-hakux.sh [options]
#
#   --version <v>     a release such as 0.17.1 or 0.18.0-exp.1
#                     (default: the newest stable release)
#   --tarball <file>  install a local release archive (linux-package.sh, CI)
#   --core <file.so>  install a locally built hakux_libretro.so; the .info
#                     next to it is used when present
#   --games <dir>     where the games are (default: ~/roms/xbox)
#   --prefix <dir>    where everything goes (default: ~/.local/share/hakux-lr)
#   --no-packages     don't install distribution packages
#
# It installs RetroArch, Vulkan and menu assets with the distribution's
# package manager (pacman, apt, dnf or zypper; asks for the sudo password),
# downloads the core and controller profiles, and gives hakuX its own
# RetroArch configuration in the prefix, so an existing RetroArch setup is
# left alone. On AMD64 it checks KVM (/dev/kvm, TSC scaling, UMIP). Run it
# again to update the core; your own settings go in <prefix>/user.cfg.
#
# start-hakux.sh runs on a desktop (Wayland, X11) or, without a display
# server, straight on KMS (Vulkan VK_KHR_display, udev input), as on
# PortareOS. To remove everything, delete the prefix and
# ~/.local/bin/start-hakux.sh.
set -eu

REPO=portare-ch/hakuX-lr
AUTOCONFIG_URL=https://codeload.github.com/libretro/retroarch-joypad-autoconfig/tar.gz/refs/heads/master

say() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    if [ -f "$0" ]; then sed -n '3,/^set -eu$/p' "$0" | sed '$d; s/^# \{0,1\}//'
    else echo "see the comment at the top of setup-hakux.sh"; fi
}

version=
tarball=
corefile=
games="$HOME/roms/xbox"
prefix="${XDG_DATA_HOME:-$HOME/.local/share}/hakux-lr"
packages=1
while [ $# -gt 0 ]; do
    case $1 in
        --version) [ $# -ge 2 ] || die "$1 needs a value"; version=${2#linux-v}; version=${version#v}; shift ;;
        --tarball) [ $# -ge 2 ] || die "$1 needs a value"; tarball=$2; shift ;;
        --core) [ $# -ge 2 ] || die "$1 needs a value"; corefile=$2; shift ;;
        --games) [ $# -ge 2 ] || die "$1 needs a value"; games=$2; shift ;;
        --prefix) [ $# -ge 2 ] || die "$1 needs a value"; prefix=$2; shift ;;
        --no-packages) packages=0 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done
[ -z "$tarball" ] || [ -f "$tarball" ] || die "no such file: $tarball"
[ -z "$corefile" ] || [ -f "$corefile" ] || die "no such file: $corefile"

case $(uname -m) in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) die "hakuX-lr runs on ARM64 and AMD64 Linux, not $(uname -m)" ;;
esac
[ "$(uname -s)" = Linux ] || die "hakuX-lr needs Linux"

if [ "$(id -u)" -eq 0 ]; then
    asroot=
elif have sudo; then
    asroot=sudo
elif have doas; then
    asroot=doas
else
    asroot=none
fi
root() {
    [ "$asroot" != none ] || die "run as root, or install sudo, for: $*"
    say "running as root: $*"
    $asroot "$@"
}

fetch() { # <url> <file>
    if have curl; then
        curl -fL --retry 3 --progress-bar -o "$2" "$1"
    elif have wget; then
        wget -q --show-progress -O "$2" "$1"
    else
        die "curl or wget is needed"
    fi
}
fetch_text() { # <url>
    if have curl; then curl -fsSL --retry 3 "$1"
    elif have wget; then wget -qO- "$1"
    else die "curl or wget is needed"; fi
}
sha256() {
    if have sha256sum; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# ---------------------------------------------------------------- packages

vulkan_icds() {
    for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d /usr/local/share/vulkan/icd.d; do
        [ -d "$d" ] && ls "$d"/*.json 2>/dev/null
    done | grep -v -e lvp_icd -e lavapipe || true
}
libvulkan() {
    for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/local/lib; do
        [ -e "$d/libvulkan.so.1" ] && { echo "$d/libvulkan.so.1"; return 0; }
    done
    return 1
}
have_libvulkan() { libvulkan >/dev/null; }
gpu_vendors() {
    cat /sys/class/drm/card[0-9]*/device/vendor 2>/dev/null | sort -u
}

install_packages() {
    want_ra=0 want_vk=0 want_loader=0
    have retroarch || want_ra=1
    [ -n "$(vulkan_icds)" ] || want_vk=1
    have_libvulkan || want_loader=1
    if [ $want_ra = 0 ] && [ $want_vk = 0 ] && [ $want_loader = 0 ]; then
        return 0
    fi
    vendors=$(gpu_vendors)
    if have pacman; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch retroarch-assets-ozone
        [ $want_loader = 0 ] || set -- "$@" vulkan-icd-loader
        if [ $want_vk = 1 ]; then
            case $vendors in *0x1002*) set -- "$@" vulkan-radeon ;; esac
            case $vendors in *0x8086*) set -- "$@" vulkan-intel ;; esac
        fi
        [ $# -eq 0 ] || root pacman -S --needed --noconfirm "$@" || die "installing $* failed"
    elif have apt-get; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch
        [ $want_loader = 0 ] || set -- "$@" libvulkan1
        [ $want_vk = 0 ] || set -- "$@" mesa-vulkan-drivers
        root apt-get update
        root apt-get install -y "$@" || die "installing $* failed"
    elif have dnf; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch
        [ $want_loader = 0 ] || set -- "$@" vulkan-loader
        [ $want_vk = 0 ] || set -- "$@" mesa-vulkan-drivers
        root dnf install -y "$@" || die "installing $* failed"
    elif have zypper; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch
        [ $want_loader = 0 ] || set -- "$@" libvulkan1
        if [ $want_vk = 1 ]; then
            case $vendors in *0x1002*) set -- "$@" libvulkan_radeon ;; esac
            case $vendors in *0x8086*) set -- "$@" libvulkan_intel ;; esac
        fi
        [ $# -eq 0 ] || root zypper --non-interactive install "$@" || die "installing $* failed"
    else
        [ $want_ra = 0 ] || die "install RetroArch with your package manager, then run this again"
    fi
}

if [ $packages = 1 ]; then
    install_packages
fi
have retroarch || die "RetroArch isn't installed (retroarch is not in PATH)"
retroarch=$(command -v retroarch)
if ! "$retroarch" --features 2>/dev/null | grep -i vulkan | grep -q -i yes; then
    warn "$retroarch reports no Vulkan support; hakuX-lr needs RetroArch's vulkan driver"
fi
if [ -z "$(vulkan_icds)" ]; then
    case $(gpu_vendors) in
        *0x10de*) warn "no Vulkan driver found: install NVIDIA's driver (nvidia-utils or your distribution's equivalent)" ;;
        *) warn "no hardware Vulkan driver found; hakuX-lr needs Vulkan 1.2 or newer (1.3 recommended)" ;;
    esac
fi
if loader=$(libvulkan); then
    # Read the version from the symlink target (libvulkan.so.1.3.275).
    lmin=$(readlink -f "$loader" | sed -n 's/.*libvulkan\.so\.1\.\([0-9][0-9]*\)\..*/\1/p')
    if [ -n "$lmin" ] && [ "$lmin" -lt 3 ]; then
        warn "the Vulkan loader is 1.$lmin; RetroArch refuses hakuX-lr on a loader older than 1.3, even with a 1.2 driver"
    fi
else
    warn "libvulkan.so.1 (the Vulkan loader) is missing"
fi

# ------------------------------------------------------------- permissions

user=$(id -un)
groups_added=
add_group() { # <group> <why>
    [ "$(id -u)" -ne 0 ] || return 0
    getent group "$1" >/dev/null 2>&1 || return 0
    id -nG "$user" | tr ' ' '\n' | grep -qx "$1" && return 0
    say "adding $user to the $1 group ($2)"
    root usermod -aG "$1" "$user"
    groups_added="$groups_added $1"
}
rw() { [ -r "$1" ] && [ -w "$1" ]; }

if [ "$arch" = x86_64 ]; then
    # The AMD64 core runs the game's code on the host CPU through KVM.
    if [ ! -e /dev/kvm ]; then
        if grep -qw svm /proc/cpuinfo; then kmod=kvm_amd
        elif grep -qw vmx /proc/cpuinfo; then kmod=kvm_intel
        else kmod=; fi
        if [ -n "$kmod" ]; then
            root modprobe "$kmod" || true
        fi
        [ -e /dev/kvm ] || warn "/dev/kvm is missing: enable AMD SVM / Intel VT-x in the BIOS/UEFI, then load kvm_amd or kvm_intel"
    fi
    if [ -e /dev/kvm ] && ! rw /dev/kvm; then
        add_group "$(stat -c %G /dev/kvm)" "access to /dev/kvm"
    fi
    kmaj=$(uname -r | cut -d. -f1) kmin=$(uname -r | cut -d. -f2 | tr -cd 0-9)
    if [ "$kmaj" -lt 5 ] || { [ "$kmaj" -eq 5 ] && [ "${kmin:-0}" -lt 16 ]; }; then
        warn "Linux $(uname -r) is older than 5.16; the KVM backend needs 5.16 or newer"
    fi
    grep -qw umip /proc/cpuinfo || warn "the CPU reports no UMIP; the KVM backend needs it"
    if [ -r /sys/module/kvm_amd/parameters/tsc_scaling ]; then
        case $(cat /sys/module/kvm_amd/parameters/tsc_scaling) in
            1|Y) ;;
            *) warn "kvm_amd has TSC scaling off (needs Zen 2 or newer)" ;;
        esac
    elif grep -q '^vmx flags' /proc/cpuinfo && ! grep '^vmx flags' /proc/cpuinfo | grep -qw tsc_scaling; then
        warn "the CPU reports no VMX TSC scaling; the KVM backend needs it"
    fi
fi

# Without a display server, RetroArch draws through KMS and reads the
# keyboard and controllers from /dev/input. A console login gets access to
# both from logind; over SSH they need the video and input groups.
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    card=$(ls /dev/dri/card[0-9]* 2>/dev/null | head -n1)
    if [ -n "$card" ] && ! rw "$card"; then
        add_group "$(stat -c %G "$card")" "drawing through KMS"
    fi
    readable=0
    for ev in /dev/input/event*; do
        [ -r "$ev" ] && { readable=1; break; }
    done
    if [ $readable = 0 ] && [ -e /dev/input/event0 ]; then
        add_group "$(stat -c %G /dev/input/event0)" "keyboard and controllers on KMS"
    fi
fi

# -------------------------------------------------------------------- core

mkdir -p "$prefix" "$prefix/cores" "$prefix/info" "$prefix/saves" \
    "$prefix/states" "$prefix/system" "$prefix/screenshots" "$prefix/logs" \
    "$prefix/config" "$prefix/playlists" "$prefix/cache" "$prefix/autoconfig" \
    "$games"
prefix=$(CDPATH='' cd -- "$prefix" && pwd)
games=$(CDPATH='' cd -- "$games" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hakux-setup.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT TERM

if [ -n "$corefile" ]; then
    say "installing $corefile"
    cp "$corefile" "$tmp/hakux_libretro.so"
    info=${corefile%.so}.info
    if [ -f "$info" ]; then
        cp "$info" "$tmp/hakux_libretro.info"
    elif [ -f "$prefix/info/hakux_libretro.info" ]; then
        cp "$prefix/info/hakux_libretro.info" "$tmp/hakux_libretro.info"
    else
        die "no hakux_libretro.info next to $corefile; install a release first"
    fi
    installed="local build ($corefile)"
else
    if [ -z "$tarball" ]; then
        if [ -z "$version" ]; then
            say "looking up the newest hakuX-lr release for $arch"
            url=$(fetch_text "https://api.github.com/repos/$REPO/releases?per_page=50" |
                grep -o "https://github.com/$REPO/releases/download/linux-v[0-9][0-9.]*/hakux-[0-9][0-9.]*-$arch\.tar\.gz\"" |
                head -n1 | tr -d '"')
            [ -n "$url" ] || die "no $arch release found at https://github.com/$REPO/releases"
        else
            url="https://github.com/$REPO/releases/download/linux-v$version/hakux-$version-$arch.tar.gz"
        fi
        tarball="$tmp/${url##*/}"
        say "downloading ${url##*/}"
        fetch "$url" "$tarball" || die "couldn't download $url"
        fetch "$url.sha256" "$tarball.sha256" 2>/dev/null || die "couldn't download $url.sha256"
        want=$(cut -d' ' -f1 "$tarball.sha256")
        got=$(sha256 "$tarball")
        [ "$want" = "$got" ] || die "checksum mismatch for ${url##*/}: $got, expected $want"
    fi
    mkdir "$tmp/x"
    tar -xzf "$tarball" -C "$tmp/x"
    so=$(find "$tmp/x" -path '*/lib/libretro/hakux_libretro.so' | head -n1)
    [ -n "$so" ] || die "$tarball holds no lib/libretro/hakux_libretro.so"
    cp "$so" "$tmp/hakux_libretro.so"
    cp "${so%.so}.info" "$tmp/hakux_libretro.info"
    rel=${so%/lib/libretro/hakux_libretro.so}
    built=$(sed -n 's/^architecture //p' "$rel/BUILD-INFO" 2>/dev/null)
    [ -z "$built" ] || [ "$built" = "$arch" ] || die "${tarball##*/} is built for $built, this machine is $arch"
    installed=$(head -n1 "$rel/BUILD-INFO" 2>/dev/null || true)
    installed=${installed:-hakuX ${rel##*/hakux-}}
    rm -rf "$prefix/licenses"
    if [ -d "$rel/share/licenses/hakux" ]; then
        cp -R "$rel/share/licenses/hakux" "$prefix/licenses"
        cp "$rel/BUILD-INFO" "$prefix/licenses/" 2>/dev/null || true
    fi
fi
# Check this core's glibc requirement; it varies by architecture and build.
glibc=$(ldd --version 2>/dev/null | sed -n '1s/.* \([0-9][0-9]*\.[0-9][0-9]*\)$/\1/p')
need=$(LC_ALL=C grep -ao 'GLIBC_2\.[0-9][0-9]*' "$tmp/hakux_libretro.so" | sort -u -t. -k2,2n | tail -n1)
need=${need#GLIBC_2.}
if [ -n "$glibc" ] && [ -n "$need" ]; then
    gmaj=${glibc%%.*} gmin=${glibc#*.}
    if [ "$gmaj" -lt 2 ] || { [ "$gmaj" -eq 2 ] && [ "$gmin" -lt "$need" ]; }; then
        warn "glibc $glibc is older than 2.$need, which this core needs; RetroArch won't load it"
    fi
fi

# Replace the files, never write into them: a running RetroArch keeps
# the old core mapped.
cp "$tmp/hakux_libretro.so" "$prefix/cores/.hakux_libretro.so.new"
mv -f "$prefix/cores/.hakux_libretro.so.new" "$prefix/cores/hakux_libretro.so"
cp "$tmp/hakux_libretro.info" "$prefix/info/hakux_libretro.info"
rm -f "$prefix/info/core_info.cache"
say "installed $installed"

# ------------------------------------------------------- controller profiles

say "downloading RetroArch's controller profiles"
if fetch "$AUTOCONFIG_URL" "$tmp/autoconfig.tar.gz" 2>/dev/null &&
    mkdir "$tmp/ac" && tar -xzf "$tmp/autoconfig.tar.gz" -C "$tmp/ac"; then
    udev=$(find "$tmp/ac" -maxdepth 2 -type d -name udev | head -n1)
    if [ -n "$udev" ]; then
        rm -rf "$prefix/autoconfig/udev"
        cp -R "$udev" "$prefix/autoconfig/udev"
    fi
else
    warn "couldn't download the controller profiles; map controllers in Settings → Input"
fi

# ------------------------------------------------------------ configuration

cfgval() { # <key> <file>: an uncommented value from a RetroArch config
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\{0,1\}\([^\"]*\)\"\{0,1\}[[:space:]]*$/\1/p" "$2" 2>/dev/null | tail -n1
}
# Menu assets: the distribution's, which RetroArch finds by itself only
# with its system configuration, which a --config file replaces.
assets=
for sys in /etc/retroarch.cfg /usr/local/etc/retroarch.cfg; do
    v=$(cfgval assets_directory "$sys")
    [ -n "$v" ] && [ -d "$v" ] && { assets=$v; break; }
done
if [ -z "$assets" ]; then
    for d in /usr/share/libretro/assets /usr/share/retroarch/assets \
        /usr/local/share/libretro/assets /usr/local/share/retroarch/assets \
        "${XDG_CONFIG_HOME:-$HOME/.config}/retroarch/assets"; do
        if [ -d "$d/ozone" ] || [ -d "$d/xmb" ] || [ -d "$d/glui" ]; then
            assets=$d
            break
        fi
    done
fi
menu=rgui
if [ -n "$assets" ]; then
    if [ -d "$assets/ozone" ]; then menu=ozone
    elif [ -d "$assets/xmb" ]; then menu=xmb
    fi
fi

q() { printf '%s' "$1" | sed 's/"/\\"/g'; }
{
    cat <<EOF
# hakuX-lr's RetroArch configuration, written by setup-hakux.sh: running it
# again rewrites this file. Put your own settings in user.cfg, which
# start-hakux.sh loads after this one.
config_save_on_exit = "false"

libretro_directory = "$(q "$prefix")/cores"
libretro_info_path = "$(q "$prefix")/info"
core_options_path = "$(q "$prefix")/retroarch-core-options.cfg"
rgui_config_directory = "$(q "$prefix")/config"
input_remapping_directory = "$(q "$prefix")/config/remaps"
savefile_directory = "$(q "$prefix")/saves"
savestate_directory = "$(q "$prefix")/states"
system_directory = "$(q "$prefix")/system"
screenshot_directory = "$(q "$prefix")/screenshots"
playlist_directory = "$(q "$prefix")/playlists"
cache_directory = "$(q "$prefix")/cache"
joypad_autoconfig_dir = "$(q "$prefix")/autoconfig"
log_dir = "$(q "$prefix")/logs"
rgui_browser_directory = "$(q "$games")"
EOF
    [ -z "$assets" ] || printf 'assets_directory = "%s"\n' "$(q "$assets")"
    for sys in /etc/retroarch.cfg /usr/local/etc/retroarch.cfg; do
        [ -f "$sys" ] || continue
        for k in audio_filter_dir video_filter_dir video_shader_dir overlay_directory osk_overlay_directory; do
            v=$(cfgval "$k" "$sys")
            [ -n "$v" ] && [ -d "$v" ] && printf '%s = "%s"\n' "$k" "$(q "$v")"
        done
        break
    done
    cat <<EOF

# RetroArch writes its log file only with verbose logging on
log_verbosity = "true"
log_to_file = "true"
log_to_file_timestamp = "false"

# hakuX draws with Vulkan; bilinear scaling as on PortareOS.
video_driver = "vulkan"
video_fullscreen = "true"
video_vsync = "true"
video_threaded = "false"
video_smooth = "true"
menu_driver = "$menu"
# RetroArch's Qt desktop window: of no use here, and without a display
# server Qt aborts RetroArch once a game has loaded.
desktop_menu_enable = "false"
ui_companion_enable = "false"
ui_companion_start_on_boot = "false"
ui_companion_toggle = "false"

input_joypad_driver = "udev"
input_autodetect_enable = "true"
# Menu: the controller's guide button where its profile maps one, or hold
# L1 + R1 + Start + Select. Esc twice quits.
input_menu_toggle_gamepad_combo = "3"
quit_press_twice = "true"

# hakuX supports no save states, so none of what needs them.
rewind_enable = "false"
run_ahead_enabled = "false"
preemptive_frames_enable = "false"
savestate_auto_save = "false"
savestate_auto_load = "false"
EOF
} > "$prefix/retroarch.cfg"

if [ ! -f "$prefix/user.cfg" ]; then
    cat > "$prefix/user.cfg" <<'EOF'
# Your RetroArch settings for hakuX-lr: start-hakux.sh loads this file last,
# and setup-hakux.sh never changes it. For example:
#   video_smooth = "false"
#   network_cmd_enable = "true"
EOF
fi

# ------------------------------------------------------------- the launcher

printf "RETROARCH='%s'\nGAMES='%s'\nARCH='%s'\n" \
    "$(printf '%s' "$retroarch" | sed "s/'/'\\\\''/g")" \
    "$(printf '%s' "$games" | sed "s/'/'\\\\''/g")" \
    "$arch" > "$prefix/hakux.env"

cat > "$prefix/start-hakux.sh" <<'LAUNCHER'
#!/bin/sh
# Copyright (C) 2026-present PortareOS
# Starts RetroArch with hakuX-lr as setup-hakux.sh set it up.
#
#   start-hakux.sh [--windowed] [--verbose] [<game> | <part of its name>]
#
# A game is an .iso or .chd file, or a file name in the games folder, whole
# or in part, with or without its extension ("burnout"). Without one,
# RetroArch's menu opens: Load Content browses the games folder. Without a
# display server (no WAYLAND_DISPLAY or DISPLAY) RetroArch draws straight
# through KMS. HAKUX_DISPLAY=kms or desktop overrides that. The core's
# diagnostics go to logs/hakux.log, RetroArch's to logs/retroarch.log.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
. "$here/hakux.env"

die() { printf 'start-hakux: %s\n' "$*" >&2; exit 1; }
warn() { printf 'start-hakux: %s\n' "$*" >&2; }
usage() { sed -n '3,/^set -eu$/p' "$(readlink -f -- "$0")" | sed '$d; s/^# \{0,1\}//'; }

windowed=0 verbose=0
while [ $# -gt 0 ]; do
    case $1 in
        -w|--windowed) windowed=1 ;;
        -v|--verbose) verbose=1 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1 (see --help)" ;;
        *) break ;;
    esac
    shift
done
[ $# -le 1 ] || die "one game at a time (see --help)"

game=
if [ $# -eq 1 ]; then
    if [ -f "$1" ]; then
        game=$(readlink -f -- "$1")
    else
        # a name in the games folder, whole or in part, its extension
        # optional; the name's own wildcards literal, a space or underscore
        # matching either ("burnout 3": Burnout_3_-_Takedown)
        name=$1
        case $name in
            *.[iI][sS][oO]|*.[cC][hH][dD]) name=${name%.*} ;;
        esac
        pat=$(printf '%s' "$name" | sed 's/[][*?\\]/\\&/g' | tr ' _' '??')
        matches=$(find "$GAMES" -maxdepth 2 -type f \( -iname "*$pat*.iso" -o -iname "*$pat*.chd" \) 2>/dev/null | sort)
        n=$(printf '%s' "$matches" | grep -c . || true)
        if [ "$n" -gt 1 ]; then
            # the one whose whole name it is ("halo": Halo.iso, not Halo 2.iso)
            want=$(printf '%s' "$name" | tr 'A-Z _' 'a-z__')
            exact=$(printf '%s\n' "$matches" | while IFS= read -r m; do
                b=${m##*/}
                [ "$(printf '%s' "${b%.*}" | tr 'A-Z _' 'a-z__')" != "$want" ] || printf '%s\n' "$m"
            done)
            if [ "$(printf '%s' "$exact" | grep -c . || true)" = 1 ]; then
                matches=$exact n=1
            fi
        fi
        case $n in
            0) die "no game matching \"$1\" in $GAMES" ;;
            1) game=$matches ;;
            *) printf '%s\n' "$matches" >&2; die "\"$1\" matches $n games; be more specific" ;;
        esac
    fi
fi

mode=${HAKUX_DISPLAY:-}
if [ -z "$mode" ]; then
    if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then mode=desktop; else mode=kms; fi
fi

if [ "$ARCH" = x86_64 ]; then
    [ -e /dev/kvm ] || die "/dev/kvm is missing: enable virtualization (SVM / VT-x) and load kvm_amd or kvm_intel"
    { [ -r /dev/kvm ] && [ -w /dev/kvm ]; } ||
        die "no access to /dev/kvm: log out and in again after setup-hakux.sh added you to its group"
fi

rt=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [ -S "$rt/pulse/native" ]; then audio=pulse
elif [ -S "$rt/pipewire-0" ]; then audio=pipewire
else audio=alsa; fi

session="$here/session.cfg"
{
    echo "# written by start-hakux.sh for each start"
    echo "audio_driver = \"$audio\""
    if [ "$mode" = kms ]; then
        echo 'video_context_driver = "khr_display"'
        echo 'input_driver = "udev"'
        echo 'video_fullscreen = "true"'
        # The connected display's preferred mode, listed first; without
        # it RetroArch takes the largest (4096x2160 on a 3840x2160 TV).
        for st in /sys/class/drm/card*-*/status; do
            [ "$(cat "$st" 2>/dev/null)" = connected ] || continue
            m=$(head -n1 "${st%/status}/modes" 2>/dev/null)
            w=${m%%x*} h=${m#*x} h=${h%%[!0-9]*}
            case $w$h in ''|*[!0-9]*) continue ;; esac
            echo "video_fullscreen_x = \"$w\""
            echo "video_fullscreen_y = \"$h\""
            break
        done
    elif [ $windowed = 1 ]; then
        echo 'video_fullscreen = "false"'
    fi
} > "$session"

if [ "$mode" = kms ]; then
    card=
    for c in /dev/dri/card[0-9]*; do
        [ -r "$c" ] && [ -w "$c" ] && { card=$c; break; }
    done
    [ -n "$card" ] || die "no access to /dev/dri/card*: start from a console login, or log in again after setup-hakux.sh added you to the video group"
    readable=0
    for ev in /dev/input/event*; do
        [ -r "$ev" ] && { readable=1; break; }
    done
    [ $readable = 1 ] || warn "no access to /dev/input: keyboard and controllers won't work (log in again, or start from a console login)"
    # Qt (RetroArch's desktop window, off in retroarch.cfg) aborts the
    # process when it finds no display server.
    export QT_QPA_PLATFORM=offscreen
fi

set -- --config "$here/retroarch.cfg" --appendconfig "$session|$here/user.cfg"
[ $verbose = 0 ] || set -- "$@" --verbose
[ -z "$game" ] || set -- "$@" -L "$here/cores/hakux_libretro.so" "$game"
# hakuX writes its diagnostics to stderr, which RetroArch's log file doesn't
# take: they get a file of their own (the last start's kept as hakux.log.1)
corelog="$here/logs/hakux.log"
mkdir -p "$here/logs"
[ ! -f "$corelog" ] || mv -f "$corelog" "$corelog.1"
echo "start-hakux: ${game:-RetroArch menu} ($mode, $audio)"
echo "start-hakux: logs: $corelog (the core), $here/logs/retroarch.log (RetroArch)"
exec "$RETROARCH" "$@" 2>"$corelog"
LAUNCHER
chmod +x "$prefix/start-hakux.sh"

mkdir -p "$HOME/.local/bin"
ln -sf "$prefix/start-hakux.sh" "$HOME/.local/bin/start-hakux.sh"

if [ -n "${XDG_CURRENT_DESKTOP:-}" ] || ls /usr/share/wayland-sessions/*.desktop /usr/share/xsessions/*.desktop >/dev/null 2>&1; then
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    mkdir -p "$apps"
    cat > "$apps/hakux-lr.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=hakuX (Xbox)
Comment=Original Xbox games in RetroArch
Exec="$prefix/start-hakux.sh" %f
Icon=retroarch
Terminal=false
Categories=Game;Emulator;
EOF
fi

# ----------------------------------------------------------------- summary

case ":$PATH:" in
    *":$HOME/.local/bin:"*) launcher=start-hakux.sh ;;
    *) launcher="$HOME/.local/bin/start-hakux.sh" ;;
esac
echo
ra_version=$("$retroarch" --version 2>&1 | sed -n 's/^Version: *\([0-9][0-9.]*\).*/\1/p' | head -n1)
say "done: $installed with RetroArch ${ra_version:-(version unknown)}"
cat <<EOF
    games:     $games (.iso or .chd)
    start:     $launcher "<game or part of its name>"
    menu:      $launcher
    settings:  $prefix/user.cfg
    saves:     $prefix/saves (the Xbox hard disk is hdd/)
    logs:      $prefix/logs/hakux.log (the core), retroarch.log (RetroArch)
EOF
if [ -n "$groups_added" ]; then
    echo
    echo "Log out and in again (or start a new SSH session) so the new groups apply:$groups_added"
fi

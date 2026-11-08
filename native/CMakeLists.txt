# hakuX native core (QEMU-free): FEXCore fork + reimplemented Xbox kernel.
#
# hakux_native is the core library; xboxrun runs a title from the command
# line on the device (adb shell) for the frontier loop. Cross-compiled with
# the NDK into build/native/core; the steps are in android/README.md.
cmake_minimum_required(VERSION 3.20)
project(hakux_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
add_library(nc_platform INTERFACE)
target_link_libraries(nc_platform INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
if(ANDROID)
  # aaudio: the communicator's microphone and speaker (dev/xblc.c), in every
  # Android build, the libretro core's too
  target_link_libraries(nc_platform INTERFACE android log aaudio)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|x86_64|AMD64)$")
  message(FATAL_ERROR "The native core requires Android arm64, or Linux on AArch64 or x86-64")
endif()
# The guest CPU (native/cpu): FEX translates on ARM64; on x86-64 Linux, KVM
# runs the guest's code on the host CPU (cpu_kvm.c, #56).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(NC_CPU_DEFAULT kvm)
else()
  set(NC_CPU_DEFAULT fex)
endif()
set(HAKUX_CPU "${NC_CPU_DEFAULT}" CACHE STRING "The guest CPU: fex (ARM64) or kvm (x86-64 Linux)")
set_property(CACHE HAKUX_CPU PROPERTY STRINGS fex kvm)
if(HAKUX_CPU STREQUAL "fex")
  if(NOT ANDROID AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "HAKUX_CPU=fex: FEX's JIT emits ARM64 code only")
  endif()
elseif(HAKUX_CPU STREQUAL "kvm")
  if(ANDROID OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "HAKUX_CPU=kvm: runs x86 guest code on an x86-64 Linux host only")
  endif()
else()
  message(FATAL_ERROR "HAKUX_CPU must be fex or kvm, not ${HAKUX_CPU}")
endif()
if(NOT ANDROID)
  target_compile_definitions(nc_platform INTERFACE _GNU_SOURCE)
endif()
option(HAKUX_BUILD_APP "Build the app: the Android native library (hakuxcore), or the Linux front end (hakux)" ${ANDROID})
option(HAKUX_BUILD_TESTS "Build native Linux smoke tests" ON)
option(HAKUX_BUILD_LIBRETRO "Build the libretro core (hakux_libretro.so; on Android hakux_libretro_android.so), everything as PIC" OFF)
option(HAKUX_LR_END_PROCESS "libretro, PortareOS's build: a stop that hangs ends RetroArch's process, and HAKUX_LR_STOP=0 brings back ending it when the game closes (a title's relaunch then starts it again)" OFF)
set(HAKUX_VERSION "dev" CACHE STRING "The version the libretro core reports")
if(HAKUX_BUILD_LIBRETRO)
  if(ANDROID AND HAKUX_BUILD_APP)
    message(FATAL_ERROR "On Android the libretro core and the app build in separate build directories: "
                        "add -DHAKUX_BUILD_APP=OFF")
  endif()
  if(ANDROID AND HAKUX_LR_END_PROCESS)
    message(FATAL_ERROR "HAKUX_LR_END_PROCESS ends RetroArch's process: PortareOS's build only")
  endif()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ThinLTO for Release builds of everything but FEX (its own ENABLE_LTO stays
# off, fex.cmake): the decoder and the renderer ~2% lighter (the RP5, DOA2).
# The cache keeps relinking after a change to the files changed.
option(HAKUX_LTO "ThinLTO in Release builds (FEX excepted)" ON)
if(HAKUX_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT nc_ipo LANGUAGES C CXX)
  if(nc_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    add_link_options("-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
  endif()
endif()

get_filename_component(NC_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(HAKUX_CPU STREQUAL "fex")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fex.cmake")
else()
  # FEX's source is still where xxhash comes from (state/cache.c, hle.cmake).
  set(HAKUX_FEX_DIR "${CMAKE_CURRENT_LIST_DIR}/../external/fex" CACHE PATH "Checkout of the FEX fork (branch hakuX)")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hle.cmake")

# libslirp (NC_DEPS/libslirp, 4.7.0, unmodified): the NAT backend's TCP/IP
# stack (net/nat.c), on the GLib subset plus net/slirp/glib.h. Its public
# header is copied beside the generated version header, so the core's
# include path gets only those two.
set(NC_SLIRP "${NC_DEPS}/libslirp/src")
set(SLIRP_MAJOR_VERSION 4)
set(SLIRP_MINOR_VERSION 7)
set(SLIRP_MICRO_VERSION 0)
set(SLIRP_VERSION_STRING "\"4.7.0\"")
configure_file("${NC_SLIRP}/libslirp-version.h.in" "${CMAKE_BINARY_DIR}/slirp/libslirp-version.h" @ONLY)
configure_file("${NC_SLIRP}/libslirp.h" "${CMAKE_BINARY_DIR}/slirp/libslirp.h" COPYONLY)
add_library(nc_slirp STATIC
  net/slirp/glib.c
  "${NC_SLIRP}/arp_table.c" "${NC_SLIRP}/bootp.c" "${NC_SLIRP}/cksum.c" "${NC_SLIRP}/dhcpv6.c"
  "${NC_SLIRP}/dnssearch.c" "${NC_SLIRP}/if.c" "${NC_SLIRP}/ip6_icmp.c" "${NC_SLIRP}/ip6_input.c"
  "${NC_SLIRP}/ip6_output.c" "${NC_SLIRP}/ip_icmp.c" "${NC_SLIRP}/ip_input.c" "${NC_SLIRP}/ip_output.c"
  "${NC_SLIRP}/mbuf.c" "${NC_SLIRP}/misc.c" "${NC_SLIRP}/ncsi.c" "${NC_SLIRP}/ndp_table.c"
  "${NC_SLIRP}/sbuf.c" "${NC_SLIRP}/slirp.c" "${NC_SLIRP}/socket.c" "${NC_SLIRP}/state.c"
  "${NC_SLIRP}/stream.c" "${NC_SLIRP}/tcp_input.c" "${NC_SLIRP}/tcp_output.c" "${NC_SLIRP}/tcp_subr.c"
  "${NC_SLIRP}/tcp_timer.c" "${NC_SLIRP}/tftp.c" "${NC_SLIRP}/udp.c" "${NC_SLIRP}/udp6.c"
  "${NC_SLIRP}/util.c" "${NC_SLIRP}/version.c" "${NC_SLIRP}/vmstate.c")
target_include_directories(nc_slirp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net/slirp" "${NC_REPO}")
target_include_directories(nc_slirp PUBLIC "${CMAKE_BINARY_DIR}/slirp")
target_link_libraries(nc_slirp PRIVATE nc_glib)
target_compile_options(nc_slirp PRIVATE -w)

add_library(hakux_native STATIC
  core/log.c
  core/jmp.c
  core/vm.c
  core/xdvdfs.c
  core/chd.c
  core/xbe_load.c
  core/mods.c
  core/online.c
  core/options.c
  core/cheats.c
  core/xbe_patch.c
  core/memscan.c
  core/core.c
  core/dash.c
  core/dash_blob.c
  core/mmio.c
  core/irq.c
  dev/nv2a.c
  dev/ports.c
  dev/usb.c
  dev/usb_hub.c
  dev/xid.c
  dev/xblc.c
  dev/ohci.c
  dev/aci.c
  dev/nvnet.c
  net/net.c
  net/lan.c
  net/all.c
  net/nat.c
  net/upnp.c
  kernel/kernel.c
  kernel/pool.c
  kernel/ob.c
  kernel/ps.c
  kernel/ke.c
  kernel/mm.c
  kernel/contig_run.c
  kernel/paging.c
  kernel/ex.c
  kernel/rtl.c
  kernel/rtlstr.c
  kernel/rtl_text.c
  kernel/seh.c
  kernel/fixups.c
  kernel/x86mmio.c
  kernel/timer.c
  kernel/intr.c
  kernel/apc.c
  kernel/io.c
  kernel/iomgr.c
  kernel/misc.c
  kernel/xc.c
  kernel/xc_crypto.c
  kernel/kstate.c
  state/state.c
  state/vm_state.c
  state/save.c
  state/cache.c
  "${NC_REPO}/hw/xbox/hle/core/xbe.c"
)
if(HAKUX_CPU STREQUAL "fex")
  target_sources(hakux_native PRIVATE cpu/cpu.cpp)
  target_link_libraries(hakux_native PUBLIC nc_fex)
else()
  target_sources(hakux_native PRIVATE cpu/cpu_kvm.c)
endif()
target_include_directories(hakux_native PUBLIC "${NC_REPO}")
# Rebuild the embedded dashboard when the pinned XBE changes.
set(NC_DASH_XBE "${NC_REPO}/native/dash/hakux-dash.xbe")
set_source_files_properties(core/dash_blob.c PROPERTIES
  COMPILE_DEFINITIONS "NC_DASH_XBE=\"${NC_DASH_XBE}\""
  OBJECT_DEPENDS "${NC_DASH_XBE}")
# xxhash (single header, FEX's copy): the savestates' blob store (state/cache.c)
target_include_directories(hakux_native PRIVATE "${HAKUX_FEX_DIR}/External/xxhash")
# The HLE layer's pure modules build without QEMU in this mode.
target_compile_definitions(hakux_native PRIVATE HLE_HOST_TEST)
target_link_libraries(hakux_native PUBLIC hakux_hle hakux_apu nc_slirp nc_platform z m)
target_compile_options(hakux_native PRIVATE -Wall -Wno-unused-parameter)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/chd.cmake")

# xboxrun shares the app's custom driver loader on Android.
add_executable(xboxrun run/xboxrun.c run/capture.c app/vkdriver.c)
target_link_libraries(xboxrun PRIVATE hakux_native hakux_hle z)
if(ANDROID)
  target_sources(xboxrun PRIVATE app/vklayer.c)
endif()
if(NC_APU_OUT_NULL)
  target_sources(xboxrun PRIVATE "${NC_APU_OUT_NULL}")
endif()
target_link_libraries(hakux_hle PUBLIC hakux_native)
if(NOT ANDROID)
  add_executable(vulkan-probe run/vulkan_probe.c "${NC_APU_OUT_NULL}")
  target_link_libraries(vulkan-probe PRIVATE hakux_native)
  add_executable(kms-grab run/kms_grab.c)
  target_link_libraries(kms-grab PRIVATE z)
endif()
target_link_libraries(nc_slirp PRIVATE hakux_native)   # nc_log

# The hakuX app's native core library (native/app, cmake/app.cmake); on
# Linux its front end (cmake/linux-app.cmake).
if(HAKUX_BUILD_APP AND ANDROID)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.cmake")
elseif(HAKUX_BUILD_APP)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux-app.cmake")
endif()

# RetroArch's core (cmake/libretro.cmake).
if(HAKUX_BUILD_LIBRETRO)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/libretro.cmake")
endif()

if(HAKUX_BUILD_TESTS AND NOT ANDROID)
  enable_testing()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux-tests.cmake")
endif()

# Copyright (C) 2026-present PortareOS
# Tests that run guest code run on x86-64 under KVM (/dev/kvm) too.
set(NC_GUEST_TESTS ON)
set(NC_LIBRETRO_GUEST_TESTS ON)
if(HAKUX_CPU STREQUAL "kvm")
  # The KVM backend's own checks (#56): privilege, exits, thread switches,
  # interrupts, the TSC, hooks.
  add_executable(kvm-smoke "${NC_REPO}/tests/native/kvm/kvm_smoke.c" "${NC_APU_OUT_NULL}")
  target_link_libraries(kvm-smoke PRIVATE hakux_native Threads::Threads)
  add_test(NAME kvm-smoke COMMAND kvm-smoke)
  set_tests_properties(kvm-smoke PROPERTIES TIMEOUT 30)
endif()
add_executable(linux-smoke "${NC_REPO}/tests/native/linux/smoke.c" "${NC_APU_OUT_NULL}")
target_link_libraries(linux-smoke PRIVATE hakux_native)
if(NC_GUEST_TESTS)
  add_test(NAME linux-smoke COMMAND linux-smoke)
  set_tests_properties(linux-smoke PROPERTIES TIMEOUT 15)
endif()
add_executable(audio-smoke "${NC_REPO}/tests/native/linux/audio.c" "${NC_APU_OUT_NULL}")
target_include_directories(audio-smoke PRIVATE "${NC_REPO}")
target_link_libraries(audio-smoke PRIVATE hakux_native)
add_test(NAME audio-smoke COMMAND audio-smoke)
set_tests_properties(audio-smoke PROPERTIES TIMEOUT 10)
# The VP's own mix through voice processing: ordinary 2D stereo by its sends,
# 3D, multipass and other sends as the monitor mixed them, the full-DSP
# monitor's mix bins, a reused voice's HRTF history, one worker and four.
add_executable(apu-stereo-smoke "${NC_REPO}/tests/native/linux/apu_stereo.c" "${NC_APU_OUT_NULL}")
target_include_directories(apu-stereo-smoke PRIVATE "${NC_REPO}")
target_link_libraries(apu-stereo-smoke PRIVATE hakux_native m)
add_test(NAME apu-stereo-smoke COMMAND apu-stereo-smoke)
set_tests_properties(apu-stereo-smoke PROPERTIES TIMEOUT 30)
if(HAKUX_DSP_JIT)
  add_executable(apu-jit-smoke "${NC_REPO}/tests/native/linux/apu_jit.c" "${NC_APU_OUT_NULL}")
  target_link_libraries(apu-jit-smoke PRIVATE hakux_native m)
  add_test(NAME apu-jit-smoke COMMAND apu-jit-smoke)
  set_tests_properties(apu-jit-smoke PROPERTIES TIMEOUT 30)
endif()
# The voices' resamplers, linear and sinc, through voice processing: levels,
# loops, pitch changes, a voice's end, reuse, a starved stream, ADPCM loops;
# `apu-resample-smoke bench` counts allocations (malloc wrapped).
add_executable(apu-resample-smoke "${NC_REPO}/tests/native/linux/apu_resample.c" "${NC_APU_OUT_NULL}")
target_include_directories(apu-resample-smoke PRIVATE "${NC_REPO}")
target_link_libraries(apu-resample-smoke PRIVATE hakux_native m)
target_link_options(apu-resample-smoke PRIVATE -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc)
add_test(NAME apu-resample-smoke COMMAND apu-resample-smoke)
set_tests_properties(apu-resample-smoke PROPERTIES TIMEOUT 60)
# Check ADPCM reserved bytes and step-index clamping.
add_executable(apu-adpcm-test "${NC_REPO}/tests/native/apu/adpcm_test.c")
target_include_directories(apu-adpcm-test PRIVATE "${NC_REPO}")
add_test(NAME apu-adpcm-test COMMAND apu-adpcm-test)
set_tests_properties(apu-adpcm-test PROPERTIES TIMEOUT 10)
# Check the kernel crypto against published vectors and the drive's signature.
add_executable(xc-test "${NC_REPO}/tests/native/xc/xc_test.c" "${NC_REPO}/native/kernel/xc_crypto.c")
target_include_directories(xc-test PRIVATE "${NC_REPO}")
add_test(NAME xc-test COMMAND xc-test)
set_tests_properties(xc-test PROPERTIES TIMEOUT 10)
# Check the runtime library's string and number conversions and its printf.
add_executable(rtl-text-test "${NC_REPO}/tests/native/rtl/text_test.c" "${NC_REPO}/native/kernel/rtl_text.c")
target_include_directories(rtl-text-test PRIVATE "${NC_REPO}")
add_test(NAME rtl-text-test COMMAND rtl-text-test)
set_tests_properties(rtl-text-test PROPERTIES TIMEOUT 10)
# Check disc directory lookup and listing on deep and looping trees.
add_executable(xdvdfs-test "${NC_REPO}/tests/native/linux/xdvdfs.c")
target_include_directories(xdvdfs-test PRIVATE "${NC_REPO}")
target_link_libraries(xdvdfs-test PRIVATE hakux_native Threads::Threads)
add_test(NAME xdvdfs-test COMMAND xdvdfs-test)
set_tests_properties(xdvdfs-test PROPERTIES TIMEOUT 10)
# Alias-table overflow must preserve page ownership and protection.
add_executable(vm-alias-test "${NC_REPO}/tests/native/linux/vm_alias.c")
target_include_directories(vm-alias-test PRIVATE "${NC_REPO}")
target_link_libraries(vm-alias-test PRIVATE hakux_native Threads::Threads)
add_test(NAME vm-alias-test COMMAND vm-alias-test)
set_tests_properties(vm-alias-test PROPERTIES TIMEOUT 10)
# Exercise NAT with console frames and a fake UPnP router.
add_executable(nat-smoke "${NC_REPO}/tests/native/linux/nat.c")
target_link_libraries(nat-smoke PRIVATE hakux_native Threads::Threads)
add_test(NAME nat-smoke COMMAND nat-smoke)
set_tests_properties(nat-smoke PROPERTIES TIMEOUT 60)
# The USB stack driven as the guest's would: enumeration, input, rumble, and
# controllers plugged in and pulled out.
add_executable(usb-smoke "${NC_REPO}/tests/native/usb/usb_test.c" "${NC_REPO}/native/dev/usb.c"
                         "${NC_REPO}/native/dev/usb_hub.c" "${NC_REPO}/native/dev/xid.c"
                         "${NC_REPO}/native/dev/xblc.c")
target_include_directories(usb-smoke PRIVATE "${NC_REPO}")
target_link_libraries(usb-smoke PRIVATE Threads::Threads m)
add_test(NAME usb-smoke COMMAND usb-smoke)
set_tests_properties(usb-smoke PROPERTIES TIMEOUT 10)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The .cht converter for XBE patches (its IPS and BPS cases with Floating IPS).
add_test(NAME xbe-to-cht COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tools/mods/test_xbe_to_cht.py")
set_tests_properties(xbe-to-cht PROPERTIES TIMEOUT 30)
if(NC_GUEST_TESTS)
  add_test(NAME linux-boundaries
           COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/linux/check.py" "$<TARGET_FILE_DIR:xboxrun>")
  set_tests_properties(linux-boundaries PROPERTIES TIMEOUT 60)
  # The kernel's queues, completion ports, IRPs and drivers driven by guest
  # code (assembled for i386 by the compiler; skipped without ld.lld).
  add_test(NAME kernel-io
           COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/linux/kernel_io.py" "$<TARGET_FILE_DIR:xboxrun>"
                   "${CMAKE_C_COMPILER}")
  set_tests_properties(kernel-io PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  # Failed commits must not leak physical pages or reservations.
  add_test(NAME kernel-mm
           COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/linux/kernel_mm.py" "$<TARGET_FILE_DIR:xboxrun>"
                   "${CMAKE_C_COMPILER}")
  set_tests_properties(kernel-mm PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  # Boot RAM reservations, page-table aliases, and stack padding.
  add_test(NAME kernel-boot-mem
           COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/linux/kernel_boot_mem.py" "$<TARGET_FILE_DIR:xboxrun>"
                   "${CMAKE_C_COMPILER}")
  set_tests_properties(kernel-boot-mem PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
if(TARGET hakux_libretro)
  # The APU made again in one process (a relaunch), its EP's samples through
  # the libretro output.
  add_executable(apu-reinit-smoke "${NC_REPO}/tests/native/linux/apu_reinit.c"
                                  "${NC_REPO}/native/libretro/audio.c")
  target_link_libraries(apu-reinit-smoke PRIVATE hakux_native)
  add_test(NAME apu-reinit-smoke COMMAND apu-reinit-smoke)
  set_tests_properties(apu-reinit-smoke PROPERTIES TIMEOUT 10)
  # The title's rumble through the real controller to RetroArch's motors,
  # with the core option on and off.
  add_executable(rumble-smoke "${NC_REPO}/tests/native/linux/rumble.c"
                              "${NC_REPO}/native/libretro/input.c" "${NC_APU_OUT_NULL}")
  target_link_libraries(rumble-smoke PRIVATE hakux_native)
  add_test(NAME rumble-smoke COMMAND rumble-smoke)
  set_tests_properties(rumble-smoke PROPERTIES TIMEOUT 10)
  # The Communicator's microphone and headphone through the libretro core's
  # (RetroArch's microphone stubbed), with the core option's device.
  add_executable(voice-smoke "${NC_REPO}/tests/native/linux/voice.c" "${NC_REPO}/native/libretro/mic.c"
                             "${NC_REPO}/native/libretro/audio.c")
  target_include_directories(voice-smoke PRIVATE "${NC_REPO}")
  target_link_libraries(voice-smoke PRIVATE hakux_native)
  add_test(NAME voice-smoke COMMAND voice-smoke)
  set_tests_properties(voice-smoke PROPERTIES TIMEOUT 20)
  # The core through a minimal frontend: exports, info and a synthetic title.
  add_executable(libretro-harness "${NC_REPO}/tests/native/libretro/harness.c"
                                  "${NC_REPO}/tests/native/libretro/vk.c")
  target_include_directories(libretro-harness PRIVATE "${NC_REPO}" "${NC_DEPS}/Vulkan-Headers/include"
                             "${NC_REPO}/native/libretro")
  target_link_libraries(libretro-harness PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  if(NC_LIBRETRO_GUEST_TESTS)
    add_test(NAME libretro-smoke
             COMMAND "${Python3_EXECUTABLE}" "${NC_REPO}/tests/native/libretro/check.py"
                     "$<TARGET_FILE:libretro-harness>" "$<TARGET_FILE:hakux_libretro>"
                     --audio-default "$<IF:$<BOOL:${HAKUX_DSP_JIT}>,dsp,fast>")
    set_tests_properties(libretro-smoke PROPERTIES TIMEOUT 180)
  endif()
endif()

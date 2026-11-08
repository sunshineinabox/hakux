cmake_minimum_required(VERSION 3.20)
project(hle_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NC_DEPS "D:/tools/native-deps" CACHE PATH "native core third-party sources")

add_compile_definitions(HLE_HOST_TEST=1)
include_directories(${REPO_ROOT} ${NC_DEPS}/root ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter
                      -Wno-unused-function)
endif()

enable_testing()

# Pure modules under test. P1a appends to this list.
set(HLE_PURE_SOURCES
  ${REPO_ROOT}/hw/xbox/hle/core/xbe.c
  ${REPO_ROOT}/hw/xbox/hle/tex/xdk_res.c
  ${REPO_ROOT}/hw/xbox/hle/front/ring.c
  ${REPO_ROOT}/hw/xbox/hle/front/arena.c
  ${REPO_ROOT}/hw/xbox/hle/mem/pageuse.c
  ${REPO_ROOT}/hw/xbox/hle/tex/xg_swizzle.c
  ${REPO_ROOT}/hw/xbox/hle/shader/ff_gen.c
  ${REPO_ROOT}/hw/xbox/hle/shader/ffhw.c
  ${REPO_ROOT}/hw/xbox/hle/shader/vsh.c
  ${REPO_ROOT}/hw/xbox/hle/shader/psh.c
  ${REPO_ROOT}/hw/xbox/hle/pb/pusher.c
  ${REPO_ROOT}/hw/xbox/hle/pb/objslot.c
  ${REPO_ROOT}/hw/xbox/hle/pb/wclip.c
  ${REPO_ROOT}/native/core/cheats.c
  ${NC_DEPS}/root/subprojects/nv2a_vsh_cpu/src/nv2a_vsh_disassembler.c
)
add_library(hle_pure STATIC ${HLE_PURE_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(hle_pure PUBLIC Threads::Threads)   # the frontend cheats' lock
if(UNIX)
  target_link_libraries(hle_pure PUBLIC m)
endif()

function(hle_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} hle_pure)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

hle_test(test_xbe)
hle_test(test_image)
hle_test(test_xdk_res)
hle_test(test_ring)
hle_test(test_arena)
hle_test(test_pageuse)
hle_test(test_pusher)
hle_test(test_objslot)
hle_test(test_wclip)
hle_test(test_pvideo)
hle_test(test_fill)
hle_test(test_xg_swizzle)
hle_test(test_ffgen)
hle_test(test_vsh)
hle_test(test_ffhw)
hle_test(test_psh)
hle_test(test_cheats)
hle_test(test_vkcompat)
hle_test(test_batch_wrap)
hle_test(test_pace)
# the back end's sources and the batch's wrappers, read at run time
file(GLOB HLE_VK_SOURCES ${REPO_ROOT}/hw/xbox/hle/vk/*.c)
list(JOIN HLE_VK_SOURCES "|" HLE_VK_SOURCES_STR)
target_compile_definitions(test_batch_wrap PRIVATE
  HLE_VK_SOURCES="${HLE_VK_SOURCES_STR}"
  HLE_BATCH_WRAP="${REPO_ROOT}/hw/xbox/hle/vk/batch_wrap.h")
# vk/compat_map.h: the Khronos headers alone (SDL3's copy)
target_include_directories(test_vkcompat PRIVATE ${NC_DEPS}/SDL3/src/video/khronos)
target_compile_definitions(test_vkcompat PRIVATE VK_NO_PROTOTYPES)

# The audio's pure parts, each with only its own sources (no hle_pure): the
# HRTF filter, the VP's stereo routing (header-only), and the libretro
# output's ring (native/libretro/audio.c, its frontend and log stood in).
function(audio_test name)
  add_executable(${name} ${name}.c ${ARGN})
  target_link_libraries(${name} Threads::Threads)
  if(UNIX)
    target_link_libraries(${name} m)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()
audio_test(test_apu_hrtf ${REPO_ROOT}/native/dev/apu/vp/simd.c)
audio_test(test_apu_stereo)
audio_test(test_lr_audio ${REPO_ROOT}/native/libretro/audio.c)
# The voices' resamplers, linear and sinc, at each vector level the host has
audio_test(test_apu_resample ${REPO_ROOT}/native/dev/apu/vp/resample.c
           ${REPO_ROOT}/native/dev/apu/vp/resample_sinc.c ${REPO_ROOT}/native/dev/apu/vp/simd.c)
# The VP's vector paths against scalar models of their arithmetic
audio_test(test_apu_simd ${REPO_ROOT}/native/dev/apu/vp/resample.c
           ${REPO_ROOT}/native/dev/apu/vp/resample_sinc.c ${REPO_ROOT}/native/dev/apu/vp/simd.c)

# The shader generators live in the local tools/ (not versioned).
if(EXISTS ${REPO_ROOT}/tools/hle/scan/hle_vshgen.c)
  add_executable(hle_vshgen ${REPO_ROOT}/tools/hle/scan/hle_vshgen.c)
  target_link_libraries(hle_vshgen hle_pure)
  add_executable(hle_pshgen ${REPO_ROOT}/tools/hle/scan/hle_pshgen.c)
  target_link_libraries(hle_pshgen hle_pure)
endif()

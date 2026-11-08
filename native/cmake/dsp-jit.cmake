# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# The pinned dsp56300 C FFI, compiled for the same Linux host (AArch64 or
# x86-64: Cranelift emits code for either, its ISA features the host's).
if(ANDROID OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|x86_64|AMD64)$")
  message(FATAL_ERROR "HAKUX_DSP_JIT requires AArch64 or x86-64 Linux; disable it for Android")
endif()

find_program(NC_CARGO cargo REQUIRED)
set(NC_DSP_JIT_SOURCE "${NC_DEPS}/dsp56300")
if(NOT EXISTS "${NC_DSP_JIT_SOURCE}/Cargo.lock")
  message(FATAL_ERROR "Missing pinned dsp56300 sources; run tools/native/linux-deps.py")
endif()
set(NC_DSP_JIT_BUILD "${CMAKE_BINARY_DIR}/dsp-jit")
set(NC_DSP_JIT_LIBRARY "${NC_DSP_JIT_BUILD}/release/libdsp56300_emu_ffi.a")
set(NC_DSP_JIT_HEADER "${NC_DSP_JIT_BUILD}/include/dsp56300.h")
set(NC_DSP_JIT_STAMP "${NC_DSP_JIT_BUILD}/built.stamp")
file(MAKE_DIRECTORY "${NC_DSP_JIT_BUILD}/include")
file(GLOB_RECURSE NC_DSP_JIT_INPUTS CONFIGURE_DEPENDS
     "${NC_DSP_JIT_SOURCE}/crates/*.rs"
     "${NC_DSP_JIT_SOURCE}/crates/*.toml"
     "${NC_DSP_JIT_SOURCE}/crates/*.c"
     "${NC_DSP_JIT_SOURCE}/crates/*.h")

add_custom_command(
  OUTPUT "${NC_DSP_JIT_STAMP}"
  BYPRODUCTS "${NC_DSP_JIT_LIBRARY}" "${NC_DSP_JIT_HEADER}"
  COMMAND "${CMAKE_COMMAND}" -E env "RUSTFLAGS=$ENV{RUSTFLAGS} -C relocation-model=pic"
          "${NC_CARGO}" build --release --locked -p dsp56300-emu-ffi
          --manifest-path "${NC_DSP_JIT_SOURCE}/Cargo.toml"
          --target-dir "${NC_DSP_JIT_BUILD}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${NC_DSP_JIT_SOURCE}/crates/emu-ffi/include/dsp56300.h" "${NC_DSP_JIT_HEADER}"
  COMMAND python3 "${NC_REPO}/tools/native/dsp-jit-licenses.py"
          "${NC_DSP_JIT_SOURCE}" "${NC_DSP_JIT_BUILD}/licenses" --cargo "${NC_CARGO}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${NC_DSP_JIT_STAMP}"
  DEPENDS "${NC_DSP_JIT_SOURCE}/Cargo.toml" "${NC_DSP_JIT_SOURCE}/Cargo.lock"
          "${NC_DSP_JIT_SOURCE}/rust-toolchain.toml" ${NC_DSP_JIT_INPUTS}
          "${NC_REPO}/tools/native/dsp-jit-licenses.py"
  COMMENT "Building the pinned DSP56300 JIT"
  VERBATIM
)
add_custom_target(nc_dsp_jit_build DEPENDS "${NC_DSP_JIT_STAMP}")
add_library(nc_dsp_jit STATIC IMPORTED GLOBAL)
set_target_properties(nc_dsp_jit PROPERTIES
  IMPORTED_LOCATION "${NC_DSP_JIT_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${NC_DSP_JIT_BUILD}/include"
)
add_dependencies(nc_dsp_jit nc_dsp_jit_build)
target_link_libraries(nc_dsp_jit INTERFACE ${CMAKE_DL_LIBS} Threads::Threads m)

# The Linux front end (native/app/linux_main.c): hakux, the game in a
# fullscreen SDL3 window through the HLE GPU's swapchain, its sound through
# SDL audio.
#
# SDL3 comes from NC_DEPS/SDL3 and is built only to link against: hakux loads
# the system's libSDL3.so.0, with its video, audio and input backends, at run
# time.
set(NC_SDL3 "${NC_DEPS}/SDL3")
if(NOT EXISTS "${NC_SDL3}/CMakeLists.txt")
  message(FATAL_ERROR "The Linux front end needs SDL3 at ${NC_SDL3} (tools/native/linux-deps.py)")
endif()

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
# No backends: whatever headers the build host has, the link needs none.
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
foreach(backend X11 WAYLAND KMSDRM OPENGL OPENGLES ALSA JACK PIPEWIRE PULSEAUDIO SNDIO IBUS DBUS HIDAPI_LIBUSB)
  set(SDL_${backend} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${NC_SDL3}" sdl3 EXCLUDE_FROM_ALL)

add_executable(hakux
  "${CMAKE_CURRENT_SOURCE_DIR}/app/linux_main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/present.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/gamepad.c"
  "${NC_REPO}/hw/xbox/hle/vk/swapchain.c"
  "${NC_APU_OUT_SDL}"
)
# the presenter is the HLE GPU's (hle.cmake's warnings), built here with SDL
set_source_files_properties("${NC_REPO}/hw/xbox/hle/vk/swapchain.c" PROPERTIES
  COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-unused-function;-Wno-sign-compare")
target_link_libraries(hakux PRIVATE hakux_native hakux_hle hakux_apu SDL3::SDL3-shared)
target_compile_definitions(hakux PRIVATE "HAKUX_VERSION=\"${HAKUX_VERSION}\"")
# no RUNPATH to this tree's SDL: hakux runs against the system's libSDL3.so.0
set_target_properties(hakux PROPERTIES SKIP_BUILD_RPATH ON)
target_compile_options(hakux PRIVATE -Wall -Wno-unused-parameter)

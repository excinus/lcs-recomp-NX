# Nintendo Switch (Atmosphere homebrew) build of LCSNative.
#
# Configure with devkitPro's toolchain file:
#   cmake -S . -B out/lcs-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
#
# Uses the Switch portlibs: SDL2 (window, GLES presenter, audio, controllers),
# FFmpeg (cutscene videos), Mesa EGL/GL. The GE runs the software rasterizer,
# or the OpenGL backend when LCS_SWITCH_OPENGL is ON.

set(LCS_SWITCH_DIR ${CMAKE_CURRENT_LIST_DIR})
set(LCS_SWITCH_TITLE "LCS Recomp" CACHE STRING "Title shown in the homebrew menu")
set(LCS_SWITCH_AUTHOR "elmasas, Switch port" CACHE STRING "Author shown in the homebrew menu")
set(LCS_SWITCH_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown in the homebrew menu")
option(LCS_SWITCH_OPENGL "Build the OpenGL GE backend (Mesa) for the Switch" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(NX_FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswresample libswscale)
pkg_check_modules(NX_SDL2 REQUIRED IMPORTED_TARGET sdl2)

target_sources(lcs_host PRIVATE
    ${LCS_SWITCH_DIR}/switch_platform.cpp)
if(LCS_SWITCH_OPENGL AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/host/ge_gpu_backend_gl.cpp)
    # GL entry points come through SDL_GL_GetProcAddress; SDL2's own link line
    # already brings Mesa (-lEGL -lglapi -ldrm_nouveau). Mesa's .pc files here
    # carry a literal $(DEVKITPRO), which CMake rejects, so they are not used.
    target_compile_definitions(lcs_host PRIVATE LCS_GL_GE_BACKEND=1)
endif()

target_link_libraries(lcs_host PUBLIC PkgConfig::NX_FFMPEG PkgConfig::NX_SDL2)

# --wrap=main: the game runs on its own thread with a 64 MiB stack (as on
#   Windows, /STACK:67108864), pinned to core 0.
# --wrap=pthread_create: every other thread is spread over cores 1 and 2, with
#   at least 8 MiB of stack.
#   Horizon starts a new thread on the process's default core (0) and never
#   moves it, so without this the GE and raster workers would share the game's
#   core.
target_link_options(LCSNative PRIVATE
    -Wl,--wrap=main
    -Wl,--wrap=pthread_create
    -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/LCSNative.map)

set_target_properties(LCSNative PROPERTIES SUFFIX ".elf")

nx_generate_nacp(LCSNative.nacp
    NAME "${LCS_SWITCH_TITLE}"
    AUTHOR "${LCS_SWITCH_AUTHOR}"
    VERSION "${LCS_SWITCH_VERSION}")

nx_create_nro(LCSNative
    ICON ${LCS_SWITCH_DIR}/icon.jpg
    NACP ${CMAKE_CURRENT_BINARY_DIR}/LCSNative.nacp)

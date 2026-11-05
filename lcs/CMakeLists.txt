set(FFMPEG_LIBS avcodec avformat avutil swresample swscale)
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg)

file(GLOB HOST_SOURCES CONFIGURE_DEPENDS host/*.cpp)
list(REMOVE_ITEM HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/host/pack_font5x7.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host/main.cpp)
# Share the real HLE implementations with the save-protection integration test.
add_library(lcs_host OBJECT ${HOST_SOURCES})
add_executable(LCSNative host/main.cpp)
target_link_libraries(LCSNative PRIVATE lcs_host)
target_include_directories(lcs_host PUBLIC host)
if(WIN32)
    target_include_directories(lcs_host PRIVATE ${FFMPEG_DIR}/include)
endif()
target_link_libraries(lcs_host PUBLIC psprecomp_core)

if(MSVC)
    target_compile_options(lcs_host PUBLIC /W4 /permissive- /EHsc)
    target_link_options(LCSNative PRIVATE /STACK:67108864)
endif()

if(WIN32)
    target_sources(LCSNative PRIVATE resources/LCSNative.rc)
    target_link_libraries(lcs_host PUBLIC gdi32 user32 winmm d3d12 dxgi d3dcompiler dxguid)
    foreach(lib ${FFMPEG_LIBS})
        target_link_libraries(lcs_host PUBLIC ${FFMPEG_DIR}/lib/${lib}.lib)
    endforeach()
    add_custom_command(TARGET LCSNative POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${FFMPEG_DIR}/bin $<TARGET_FILE_DIR:LCSNative>)

    find_program(POWERSHELL NAMES pwsh powershell)
    if(POWERSHELL)
        add_custom_command(TARGET LCSNative POST_BUILD
            COMMAND ${POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/stamp_outputs.ps1
                -Executable $<TARGET_FILE:LCSNative>)
    endif()
elseif(NINTENDO_SWITCH)
    # Nintendo Switch (Atmosphere homebrew). See switch/README.md.
    include(${CMAKE_CURRENT_SOURCE_DIR}/switch/switch.cmake)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswresample libswscale)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
    find_package(Threads REQUIRED)
    # LCS_GE_OPENGL builds the OpenGL GE (the Switch's) instead of Vulkan, to
    # check that backend on a desktop. Off for normal builds.
    option(LCS_GE_OPENGL "Use the OpenGL GE backend instead of Vulkan" OFF)
    if(LCS_GE_OPENGL)
        target_compile_definitions(lcs_host PRIVATE LCS_GL_GE_BACKEND=1)
    else()
        target_compile_definitions(lcs_host PRIVATE LCS_VULKAN_GE_BACKEND=1)
    endif()
    target_link_libraries(lcs_host PUBLIC
        PkgConfig::FFMPEG PkgConfig::SDL2 PkgConfig::VULKAN Threads::Threads)
    # ge_spv.hpp is produced here so an edit to a shader is what the binary runs.
    find_program(LCS_GLSLANG NAMES glslangValidator glslang REQUIRED)
    set(LCS_GE_SPV_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan/ge_spv.hpp)
    add_custom_command(
        OUTPUT ${LCS_GE_SPV_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DGLSLANG=${LCS_GLSLANG}
            -DVERT=${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.vert
            -DFRAG=${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.frag
            -DPRESENT_VERT=${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/present.vert
            -DPRESENT_FRAG=${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/present.frag
            -DOUT=${LCS_GE_SPV_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/embed_spirv.cmake
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.vert
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/ge.frag
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/present.vert
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/present.frag
            ${CMAKE_CURRENT_SOURCE_DIR}/host/vulkan/embed_spirv.cmake
        COMMENT "Compiling Vulkan GE shaders"
        VERBATIM)
    add_custom_target(lcs_ge_spirv DEPENDS ${LCS_GE_SPV_HEADER})
    add_dependencies(lcs_host lcs_ge_spirv)
    # pack_font5x7 turns host/font5x7.txt into the F10 menu font. Windows skips this.
    add_executable(lcs_pack_font5x7 host/pack_font5x7.cpp)
    set_target_properties(lcs_pack_font5x7 PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set(LCS_FONT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/host_font_5x7.hpp)
    add_custom_command(
        OUTPUT ${LCS_FONT_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND lcs_pack_font5x7 ${CMAKE_CURRENT_SOURCE_DIR}/host/font5x7.txt ${LCS_FONT_HEADER}
        DEPENDS lcs_pack_font5x7 ${CMAKE_CURRENT_SOURCE_DIR}/host/font5x7.txt
        COMMENT "Packing host menu font"
        VERBATIM)
    add_custom_target(lcs_host_font DEPENDS ${LCS_FONT_HEADER})
    add_dependencies(lcs_host lcs_host_font)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/host/display_window_sdl.cpp
        PROPERTIES OBJECT_DEPENDS ${LCS_FONT_HEADER})
    target_include_directories(lcs_host BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/host/ge_gpu_backend_vulkan.cpp
        PROPERTIES OBJECT_DEPENDS ${LCS_GE_SPV_HEADER})
    # Some linkers do not resolve the shared libraries FFmpeg itself depends on.
    # Probe a small link, and only then pass FFmpeg's own libdir as -rpath-link.
    include(CheckCXXSourceCompiles)
    set(_lcs_ffmpeg_probe [=[
extern "C" int avformat_version();
int main() { return avformat_version() == 0; }
]=])
    set(CMAKE_REQUIRED_LIBRARIES ${FFMPEG_LINK_LIBRARIES})
    check_cxx_source_compiles("${_lcs_ffmpeg_probe}" LCS_FFMPEG_LINKS)
    if(NOT LCS_FFMPEG_LINKS)
        pkg_get_variable(LCS_FFMPEG_LIBDIR libavcodec libdir)
        if(LCS_FFMPEG_LIBDIR)
            set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,-rpath-link,${LCS_FFMPEG_LIBDIR}")
            check_cxx_source_compiles("${_lcs_ffmpeg_probe}" LCS_FFMPEG_LINKS_WITH_LIBDIR)
            unset(CMAKE_REQUIRED_LINK_OPTIONS)
            if(LCS_FFMPEG_LINKS_WITH_LIBDIR)
                target_link_options(lcs_host PUBLIC "-Wl,-rpath-link,${LCS_FFMPEG_LIBDIR}")
            endif()
        endif()
    endif()
    unset(CMAKE_REQUIRED_LIBRARIES)
endif()

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS generated/*.cpp)
if(GENERATED_SOURCES)
    add_library(lcs_generated STATIC ${GENERATED_SOURCES})
    target_include_directories(lcs_generated PRIVATE host)
    target_link_libraries(lcs_generated PUBLIC psprecomp_core)
    if(MSVC)
        target_compile_options(lcs_generated PRIVATE /bigobj ${PSPRECOMP_MSVC_MP_FLAG}
            $<$<CONFIG:Release,RelWithDebInfo>:/O2>)
    else()
        target_compile_options(lcs_generated PRIVATE -O2)
    endif()
    if(NINTENDO_SWITCH)
        # devkitPro's toolchain adds -g. Debug info for the recompiled units
        # made them 2.5x slower to build and 12x bigger; symbols are enough.
        target_compile_options(lcs_generated PRIVATE -g0)
    endif()
    target_link_libraries(LCSNative PRIVATE lcs_generated)
else()
    target_compile_definitions(LCSNative PRIVATE LCS_NO_GENERATED)
endif()

if(NINTENDO_SWITCH)
    # Everything links into one static PIE. Libraries default to -fPIC, which
    # reaches globals through the GOT and treats every global function as
    # interposable (no inlining across them); -fPIE does neither.
    foreach(_lcs_target psprecomp_core lcs_host lcs_generated)
        if(TARGET ${_lcs_target})
            set_target_properties(${_lcs_target} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
            target_compile_options(${_lcs_target} PRIVATE -fPIE)
        endif()
    endforeach()
endif()

if(NOT NINTENDO_SWITCH)
add_executable(lcs_host_menu_test
    ${CMAKE_SOURCE_DIR}/tests/lcs_host_menu_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host/lcs_display_menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host/lcs_save_picker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host/lcs_savedata_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/host/lcs_render_config.cpp)
target_include_directories(lcs_host_menu_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(lcs_host_menu_test PRIVATE user32)
else()
    target_link_libraries(lcs_host_menu_test PRIVATE PkgConfig::SDL2 Threads::Threads)
endif()
if(PSPRECOMP_BUILD_TESTS)
    foreach(_mode menu present f10 bindings fps persist triggers saves crypto)
        add_test(NAME lcs_host_menu_${_mode} COMMAND lcs_host_menu_test ${_mode})
    endforeach()
endif()
endif()

if(PSPRECOMP_BUILD_TESTS)
    add_executable(lcs_world_detail_test ${CMAKE_SOURCE_DIR}/tests/lcs_world_detail_test.cpp)
    target_link_libraries(lcs_world_detail_test PRIVATE lcs_host)
    add_test(NAME lcs_world_detail_test COMMAND lcs_world_detail_test)
    add_executable(lcs_ge_transfer_test ${CMAKE_SOURCE_DIR}/tests/lcs_ge_transfer_test.cpp)
    target_link_libraries(lcs_ge_transfer_test PRIVATE lcs_host)
    add_test(NAME lcs_ge_transfer_test COMMAND lcs_ge_transfer_test)
    add_executable(lcs_stress_controller_test
        ${CMAKE_SOURCE_DIR}/tests/lcs_stress_controller_test.cpp
        host/lcs_stress_controller.cpp)
    target_include_directories(lcs_stress_controller_test PRIVATE host ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME lcs_stress_controller_test COMMAND lcs_stress_controller_test)
    if(LCS_GE_OPENGL)
        # The Switch's OpenGL GE, checked on a desktop (needs an OpenGL display;
        # xvfb-run with Mesa is enough). Exit code 77 means skipped.
        add_executable(lcs_ge_gl_backend_test
            ${CMAKE_SOURCE_DIR}/tests/lcs_ge_gl_backend_test.cpp
            host/ge_gpu_backend_gl.cpp
            host/lcs_prompt_textures.cpp
            host/lcs_render_config.cpp
            host/lcs_display_menu.cpp
            host/lcs_save_picker.cpp
            host/lcs_savedata_crypto.cpp
            host/lcs_runtime_log.cpp)
        target_compile_definitions(lcs_ge_gl_backend_test PRIVATE LCS_GL_GE_BACKEND=1)
        target_include_directories(lcs_ge_gl_backend_test PRIVATE host ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(lcs_ge_gl_backend_test PRIVATE PkgConfig::SDL2 Threads::Threads)
        add_test(NAME lcs_ge_gl_backend_test COMMAND lcs_ge_gl_backend_test)
        set_tests_properties(lcs_ge_gl_backend_test PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    if(NOT WIN32)
        # The Switch build's allocator (lcs/switch/switch_malloc.cpp) over a fake heap.
        add_executable(lcs_switch_malloc_test ${CMAKE_SOURCE_DIR}/tests/switch_malloc_test.cpp)
        target_link_libraries(lcs_switch_malloc_test PRIVATE Threads::Threads)
        add_test(NAME lcs_switch_malloc_test COMMAND lcs_switch_malloc_test)
    endif()
    add_executable(lcs_stress_safety_test ${CMAKE_SOURCE_DIR}/tests/lcs_stress_safety_test.cpp)
    target_link_libraries(lcs_stress_safety_test PRIVATE lcs_host)
    add_test(NAME lcs_stress_safety_test COMMAND lcs_stress_safety_test)
endif()


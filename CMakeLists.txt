cmake_minimum_required(VERSION 3.25)

project(glint
    VERSION 0.1.0
    DESCRIPTION "Wallpaper engine for Wayland"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# M0: the hello world needs nothing but the standard library. The platform libraries
# (Wayland, Vulkan, PipeWire, FFmpeg, glslang) come in with M1 and are listed in
# cmake/Dependencies.cmake, pulled in once GLINT_PLATFORM is on.
option(GLINT_PLATFORM "Build the daemon with Wayland and Vulkan (Linux only)" OFF)
option(GLINT_SANITIZE "Address and undefined behaviour sanitizers" OFF)
option(GLINT_WERROR "Treat warnings as errors" ON)

if(GLINT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

configure_file(src/core/version.hpp.in ${CMAKE_BINARY_DIR}/generated/core/version.hpp @ONLY)

add_executable(glint
    src/main.cpp
    src/core/log.cpp
)

target_include_directories(glint PRIVATE src ${CMAKE_BINARY_DIR}/generated)

set(GLINT_WARNINGS
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wno-missing-field-initializers>
    $<$<AND:$<BOOL:${GLINT_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
)
target_compile_options(glint PRIVATE ${GLINT_WARNINGS})

# The audio spectrum (FFT and the 64 bands for scenes) needs only the standard library,
# so it and its tests build everywhere, the hello preset included.
add_library(glint_audio STATIC
    src/audio/fft.cpp
    src/audio/spectrum.cpp
)
target_include_directories(glint_audio PUBLIC src)
target_compile_options(glint_audio PRIVATE ${GLINT_WARNINGS})

enable_testing()
add_executable(glint-spectrum-tests tests/spectrum.cpp)
target_link_libraries(glint-spectrum-tests PRIVATE glint_audio)
target_compile_options(glint-spectrum-tests PRIVATE ${GLINT_WARNINGS})
add_test(NAME spectrum COMMAND glint-spectrum-tests)

if(GLINT_PLATFORM)
    include(cmake/Dependencies.cmake)
    target_sources(glint PRIVATE
        src/daemon.cpp
        src/cli/cli.cpp
        src/core/loop.cpp
        src/core/paths.cpp
        src/core/state.cpp
        src/core/worker.cpp
        src/gpu/blit.cpp
        src/gpu/buffer.cpp
        src/gpu/context.cpp
        src/gpu/pictures.cpp
        src/gpu/staging.cpp
        src/gpu/surface.cpp
        src/gpu/texture.cpp
        src/gpu/volk.cpp
        src/ipc/server.cpp
        src/wallpaper/wallpaper.cpp
        src/wallpaper/image/decode.cpp
        src/wallpaper/image/image.cpp
        src/wallpaper/image/textures.cpp
        src/wallpaper/scene/assets.cpp
        src/wallpaper/scene/renderer.cpp
        src/wallpaper/scene/scene_wallpaper.cpp
        src/wayland/display.cpp
    )
    target_link_libraries(glint PRIVATE glint::platform glint_scene)
    add_dependencies(glint glint_shaders)

    # The Wallpaper Engine readers, shared by the daemon, we-inspect and the tests.
    add_library(glint_scene STATIC
        src/wallpaper/scene/mdl.cpp
        src/wallpaper/scene/plan.cpp
        src/wallpaper/scene/project.cpp
        src/wallpaper/scene/scene.cpp
        src/wallpaper/scene/shader.cpp
        src/wallpaper/scene/tex.cpp
        src/wallpaper/scene/vfs.cpp
    )
    target_include_directories(glint_scene PUBLIC src)
    target_link_libraries(glint_scene PUBLIC nlohmann_json::nlohmann_json PkgConfig::LZ4
        glslang::glslang glslang::glslang-default-resource-limits)
    target_compile_options(glint_scene PRIVATE ${GLINT_WARNINGS})
    if(GLINT_SANITIZE)
        # glslang is built without RTTI, which UBSan's vptr check needs.
        set_source_files_properties(src/wallpaper/scene/shader.cpp PROPERTIES
            COMPILE_OPTIONS -fno-sanitize=vptr)
    endif()

    add_executable(we-inspect tools/we-inspect.cpp)
    target_link_libraries(we-inspect PRIVATE glint_scene)
    target_compile_options(we-inspect PRIVATE ${GLINT_WARNINGS})

    add_executable(glint-tests tests/readers.cpp)
    target_link_libraries(glint-tests PRIVATE glint_scene)
    target_compile_options(glint-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME readers COMMAND glint-tests)
endif()

install(TARGETS glint RUNTIME DESTINATION bin)

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

configure_file(src/core/version.hpp.in ${CMAKE_BINARY_DIR}/generated/core/version.hpp @ONLY)

add_executable(glint
    src/main.cpp
    src/core/log.cpp
)

target_include_directories(glint PRIVATE src ${CMAKE_BINARY_DIR}/generated)

target_compile_options(glint PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wno-missing-field-initializers>
    $<$<AND:$<BOOL:${GLINT_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
)

if(GLINT_SANITIZE)
    target_compile_options(glint PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(glint PRIVATE -fsanitize=address,undefined)
endif()

if(GLINT_PLATFORM)
    include(cmake/Dependencies.cmake)
    target_sources(glint PRIVATE
        src/daemon.cpp
        src/cli/cli.cpp
        src/core/loop.cpp
        src/core/paths.cpp
        src/core/state.cpp
        src/core/worker.cpp
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
        src/wayland/display.cpp
    )
    target_link_libraries(glint PRIVATE glint::platform)
    add_dependencies(glint glint_shaders)
endif()

install(TARGETS glint RUNTIME DESTINATION bin)

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

# Logging, used by the daemon, the readers and the tools alike.
add_library(glint_core STATIC src/core/log.cpp)
target_include_directories(glint_core PUBLIC src)

add_executable(glint
    src/main.cpp
)
target_link_libraries(glint PRIVATE glint_core)

target_include_directories(glint PRIVATE src ${CMAKE_BINARY_DIR}/generated)

set(GLINT_WARNINGS
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion
        -Wno-missing-field-initializers>
    $<$<AND:$<BOOL:${GLINT_WERROR}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
)
target_compile_options(glint PRIVATE ${GLINT_WARNINGS})

enable_testing()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(glint-worker-tests tests/worker.cpp src/core/worker.cpp src/core/loop.cpp)
    target_link_libraries(glint-worker-tests PRIVATE glint_core Threads::Threads)
    target_compile_options(glint-worker-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME worker COMMAND glint-worker-tests)
    set_tests_properties(worker PROPERTIES TIMEOUT 10)
endif()

# The audio spectrum (FFT and the 64 bands for scenes) needs only the standard library,
# so it and its tests build everywhere, the hello preset included.
add_library(glint_audio STATIC
    src/audio/fft.cpp
    src/audio/spectrum.cpp
)
target_include_directories(glint_audio PUBLIC src)
target_compile_options(glint_audio PRIVATE ${GLINT_WARNINGS})

add_executable(glint-spectrum-tests tests/spectrum.cpp)
target_link_libraries(glint-spectrum-tests PRIVATE glint_audio)
target_compile_options(glint-spectrum-tests PRIVATE ${GLINT_WARNINGS})
add_test(NAME spectrum COMMAND glint-spectrum-tests)

# Window rules and the power policy are plain logic and build everywhere, with their tests.
add_library(glint_policy STATIC
    src/policy/power.cpp
    src/policy/rules.cpp
)
target_include_directories(glint_policy PUBLIC src)
target_compile_options(glint_policy PRIVATE ${GLINT_WARNINGS})

add_executable(glint-policy-tests tests/policy.cpp)
target_link_libraries(glint-policy-tests PRIVATE glint_policy)
target_compile_options(glint-policy-tests PRIVATE ${GLINT_WARNINGS})
add_test(NAME policy COMMAND glint-policy-tests)

if(GLINT_PLATFORM)
    include(cmake/Dependencies.cmake)
    pkg_check_modules(TUI REQUIRED IMPORTED_TARGET ncursesw)
    target_sources(glint PRIVATE
        src/daemon.cpp
        src/audio/capture.cpp
        src/audio/player.cpp
        src/input/hyprland.cpp
        src/cli/cli.cpp
        src/cli/tui.cpp
        src/cli/tui_model.cpp
        src/cli/workshop.cpp
        src/cli/configure.cpp
        src/cli/library.cpp
        src/core/loop.cpp
        src/core/state.cpp
        src/core/worker.cpp
        src/gpu/blit.cpp
        src/gpu/buffer.cpp
        src/gpu/context.cpp
        src/gpu/dmabuf.cpp
        src/gpu/pictures.cpp
        src/gpu/staging.cpp
        src/gpu/surface.cpp
        src/gpu/texture.cpp
        src/gpu/video_pass.cpp
        src/gpu/volk.cpp
        src/ipc/server.cpp
        src/ipc/library.cpp
        src/media/mpris.cpp
        src/wallpaper/wallpaper.cpp
        src/wallpaper/image/decode.cpp
        src/wallpaper/image/image.cpp
        src/wallpaper/image/textures.cpp
        src/wallpaper/scene/assets.cpp
        src/wallpaper/scene/renderer.cpp
        src/wallpaper/scene/scene_wallpaper.cpp
        src/wallpaper/video/decoder.cpp
        src/wallpaper/video/source.cpp
        src/wallpaper/video/video.cpp
        src/wayland/display.cpp
    )
    target_link_libraries(glint PRIVATE glint::platform glint_scene glint_audio PkgConfig::SDBUS PkgConfig::TUI)
    add_dependencies(glint glint_shaders)

    # The Wallpaper Engine readers, shared by the daemon, we-inspect and the tests.
    add_library(glint_scene STATIC
        src/core/paths.cpp
        src/wallpaper/scene/mdl.cpp
        src/wallpaper/scene/particles.cpp
        src/wallpaper/scene/plan.cpp
        src/wallpaper/scene/project.cpp
        src/wallpaper/scene/scene.cpp
        src/wallpaper/scene/script.cpp
        src/wallpaper/scene/script_prelude.cpp
        src/wallpaper/scene/shader.cpp
        src/wallpaper/scene/tex.cpp
        src/wallpaper/scene/text.cpp
        src/wallpaper/scene/vfs.cpp
    )
    target_include_directories(glint_scene PUBLIC src)
    target_link_libraries(glint_scene PUBLIC glint_core nlohmann_json::nlohmann_json PkgConfig::LZ4 qjs
        PkgConfig::TEXT glslang::glslang glslang::glslang-default-resource-limits)
    target_compile_options(glint_scene PRIVATE ${GLINT_WARNINGS})
    if(GLINT_SANITIZE)
        # glslang is built without RTTI, which UBSan's vptr check needs.
        set_source_files_properties(src/wallpaper/scene/shader.cpp PROPERTIES
            COMPILE_OPTIONS -fno-sanitize=vptr)
    endif()

    add_executable(we-inspect tools/we-inspect.cpp)
    target_link_libraries(we-inspect PRIVATE glint_scene)
    target_compile_options(we-inspect PRIVATE ${GLINT_WARNINGS})

    # Headless renders of a scene into a PNG: the renderer with the GPU pieces, no Wayland.
    add_executable(glint-render
        tools/glint-render.cpp
        src/gpu/blit.cpp
        src/gpu/buffer.cpp
        src/gpu/context.cpp
        src/gpu/staging.cpp
        src/gpu/texture.cpp
        src/gpu/volk.cpp
        src/gpu/video_pass.cpp
        src/wallpaper/image/decode.cpp
        src/wallpaper/scene/assets.cpp
        src/wallpaper/scene/renderer.cpp
        src/wallpaper/video/decoder.cpp
    )
    target_include_directories(glint-render PRIVATE src ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(glint-render PRIVATE glint::platform glint_scene)
    target_compile_options(glint-render PRIVATE ${GLINT_WARNINGS})
    add_dependencies(glint-render glint_shaders)

    add_executable(glint-tests tests/readers.cpp)
    target_link_libraries(glint-tests PRIVATE glint_scene)
    target_compile_options(glint-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME readers COMMAND glint-tests)
    add_executable(glint-script-tests tests/scripts.cpp)
    target_link_libraries(glint-script-tests PRIVATE glint_scene)
    target_compile_options(glint-script-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME scripts COMMAND glint-script-tests)
    set_tests_properties(scripts PROPERTIES TIMEOUT 10)
    add_executable(glint-library-tests tests/library.cpp src/cli/library.cpp src/ipc/library.cpp)
    target_link_libraries(glint-library-tests PRIVATE glint_scene)
    target_compile_options(glint-library-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME library COMMAND glint-library-tests)
    add_executable(glint-configure-tests tests/configure.cpp src/cli/configure.cpp)
    target_include_directories(glint-configure-tests PRIVATE src)
    target_link_libraries(glint-configure-tests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_options(glint-configure-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME configure COMMAND glint-configure-tests)
    add_executable(glint-tui-tests tests/tui.cpp src/cli/tui_model.cpp)
    target_include_directories(glint-tui-tests PRIVATE src)
    target_link_libraries(glint-tui-tests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_options(glint-tui-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME tui COMMAND glint-tui-tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_executable(glint-tui-driver tests/tui_driver.cpp src/cli/tui.cpp src/cli/tui_model.cpp src/cli/library.cpp src/cli/workshop.cpp)
        target_link_libraries(glint-tui-driver PRIVATE glint_scene PkgConfig::TUI)
        target_compile_options(glint-tui-driver PRIVATE ${GLINT_WARNINGS})
        add_test(NAME tui-terminal COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/tui_terminal.py $<TARGET_FILE:glint-tui-driver>)
        set_tests_properties(tui-terminal PROPERTIES TIMEOUT 25)
    endif()
    add_executable(glint-media-tests tests/media.cpp)
    target_link_libraries(glint-media-tests PRIVATE glint_scene)
    target_compile_options(glint-media-tests PRIVATE ${GLINT_WARNINGS})
    add_test(NAME media COMMAND glint-media-tests)
endif()

include(GNUInstallDirs)
install(TARGETS glint RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(GLINT_PLATFORM)
    install(FILES brand/icon.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME glint.svg)
    configure_file(cmake/InstallDesktop.cmake.in ${PROJECT_BINARY_DIR}/InstallDesktop.cmake @ONLY)
    install(SCRIPT ${PROJECT_BINARY_DIR}/InstallDesktop.cmake)
endif()

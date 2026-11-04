# Platform libraries for the daemon. Enabled with -DGLINT_PLATFORM=ON (M1 onward).
# Every entry is a distro package except VMA, which has no Arch package and is the single
# header in third_party/vma. Arch package names are in docs/PLAN.md.

find_package(PkgConfig REQUIRED)
find_package(Vulkan 1.3 REQUIRED)                  # vulkan-headers, vulkan-icd-loader
find_package(volk REQUIRED CONFIG)                 # volk
find_package(glslang REQUIRED CONFIG)              # glslang
find_package(nlohmann_json REQUIRED CONFIG)        # nlohmann-json
find_package(Threads REQUIRED)

pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)

# VMA: one translation unit holds the implementation. It loads Vulkan through volk, so no
# static or loader-provided function pointers.
add_library(glint_vma STATIC ${PROJECT_SOURCE_DIR}/src/gpu/vma.cpp)
target_include_directories(glint_vma SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/vma)
target_compile_definitions(glint_vma PUBLIC VMA_STATIC_VULKAN_FUNCTIONS=0
                                            VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
target_link_libraries(glint_vma PUBLIC volk::volk_headers)
target_compile_definitions(glint_vma PUBLIC VK_NO_PROTOTYPES)

# Wayland protocols. The xml comes from the packages; wayland-scanner writes a client header
# and the interface code into ${CMAKE_BINARY_DIR}/generated/protocols at build time.
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)   # wayland-protocols
pkg_get_variable(WLR_PROTOCOLS_DIR wlr-protocols pkgdatadir)           # wlr-protocols
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)       # wayland

set(GLINT_PROTOCOLS
    ${WLR_PROTOCOLS_DIR}/unstable/wlr-layer-shell-unstable-v1.xml
    ${WLR_PROTOCOLS_DIR}/unstable/wlr-foreign-toplevel-management-unstable-v1.xml
    ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
    ${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml
    ${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml
    ${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml
)

set(GLINT_PROTOCOL_DIR ${CMAKE_BINARY_DIR}/generated/protocols)
file(MAKE_DIRECTORY ${GLINT_PROTOCOL_DIR})
set(GLINT_PROTOCOL_SOURCES)
foreach(xml IN LISTS GLINT_PROTOCOLS)
    if(NOT EXISTS ${xml})
        message(FATAL_ERROR "Wayland protocol not found: ${xml}")
    endif()
    get_filename_component(name ${xml} NAME_WE)
    set(header ${GLINT_PROTOCOL_DIR}/${name}.h)
    set(code ${GLINT_PROTOCOL_DIR}/${name}.c)
    add_custom_command(
        OUTPUT ${header} ${code}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml}
        COMMENT "wayland-scanner ${name}")
    list(APPEND GLINT_PROTOCOL_SOURCES ${header} ${code})
endforeach()

add_library(glint_protocols STATIC ${GLINT_PROTOCOL_SOURCES})
target_include_directories(glint_protocols SYSTEM PUBLIC ${GLINT_PROTOCOL_DIR})
target_link_libraries(glint_protocols PUBLIC PkgConfig::WAYLAND)

add_library(glint::platform INTERFACE IMPORTED)
target_link_libraries(glint::platform INTERFACE
    volk::volk_headers glint_vma glint_protocols
    glslang::glslang nlohmann_json::nlohmann_json Threads::Threads
    PkgConfig::WAYLAND PkgConfig::PIPEWIRE PkgConfig::FFMPEG PkgConfig::LZ4)
target_compile_definitions(glint::platform INTERFACE GLINT_PLATFORM=1 VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WAYLAND_KHR)
# Libraries a milestone does not call yet (PipeWire and lz4 in M1) are not loaded.
target_link_options(glint::platform INTERFACE -Wl,--as-needed)

# Built-in shaders (the picture pass; later the composition passes) are compiled at build time
# to SPIR-V and embedded as arrays. Wallpaper Engine shaders are compiled at runtime instead.
if(NOT Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    message(FATAL_ERROR "glslangValidator not found (glslang package)")
endif()
set(GLINT_SHADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)
file(MAKE_DIRECTORY ${GLINT_SHADER_DIR})
set(GLINT_SHADER_HEADERS)
foreach(shader fullscreen.vert picture.frag)
    string(REPLACE "." "_" symbol ${shader})
    set(header ${GLINT_SHADER_DIR}/${shader}.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 --quiet
                --vn ${symbol} -o ${header} ${PROJECT_SOURCE_DIR}/src/shaders/${shader}
        DEPENDS ${PROJECT_SOURCE_DIR}/src/shaders/${shader}
        COMMENT "glslang ${shader}")
    list(APPEND GLINT_SHADER_HEADERS ${header})
endforeach()
add_custom_target(glint_shaders DEPENDS ${GLINT_SHADER_HEADERS})

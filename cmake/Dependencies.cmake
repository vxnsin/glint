# Platform libraries for the daemon. Enabled with -DGLINT_PLATFORM=ON (M1 onward).
# Every entry is a distro package; nothing is fetched or vendored. Names are the pkg-config
# or CMake package names, Arch package names in docs/PLAN.md.

find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)                      # vulkan-headers, vulkan-icd-loader
find_package(volk REQUIRED)                        # volk
find_package(VulkanMemoryAllocator REQUIRED)       # vulkan-memory-allocator
find_package(glslang REQUIRED)                     # glslang
find_package(nlohmann_json REQUIRED)               # nlohmann-json

pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)

# Protocol xml files come from the packages, generated with wayland-scanner at build time.
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)   # wayland-protocols
pkg_get_variable(WLR_PROTOCOLS_DIR wlr-protocols pkgdatadir)           # wlr-protocols
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)

add_library(glint::platform INTERFACE IMPORTED)
target_link_libraries(glint::platform INTERFACE
    Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator
    glslang::glslang glslang::SPIRV nlohmann_json::nlohmann_json
    PkgConfig::WAYLAND PkgConfig::PIPEWIRE PkgConfig::FFMPEG PkgConfig::LZ4)

# Protocols glint speaks. Generated into ${CMAKE_BINARY_DIR}/generated/protocols by M1.
set(GLINT_PROTOCOLS
    ${WLR_PROTOCOLS_DIR}/unstable/wlr-layer-shell-unstable-v1.xml
    ${WLR_PROTOCOLS_DIR}/unstable/wlr-foreign-toplevel-management-unstable-v1.xml
    ${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml
    ${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml
    ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml
)

#pragma once

// Vulkan through volk, with the Wayland surface functions. Include this, never vulkan.h.
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <volk.h>

#include <string>

namespace glint::gpu {

const char* result_name(VkResult result);

// "vkCreateDevice: VK_ERROR_..." for error messages.
inline std::string failed(const char* what, VkResult result) {
    return std::string(what) + ": " + result_name(result);
}

}  // namespace glint::gpu

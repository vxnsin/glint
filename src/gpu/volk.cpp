// volk's implementation, compiled here with the Wayland platform defined. The volk package's
// own static library is built without it and lacks vkCreateWaylandSurfaceKHR.

#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#define VOLK_IMPLEMENTATION
#include <volk.h>

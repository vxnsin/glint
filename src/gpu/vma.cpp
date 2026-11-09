// The one translation unit that holds the VMA implementation (third_party/vma).
// Function pointers come from volk at allocator creation, see gpu/device.cpp.

#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

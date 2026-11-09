# third_party

Everything glint links comes from distro packages (docs/PLAN.md, Dependencies), except the
pieces here: single files with no Arch package, each in its own folder with its licence.

- `vma/vk_mem_alloc.h`: Vulkan Memory Allocator 3.5.0, MIT, from
  GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator at commit
  0f1a757da579831283413295cc7f3b47eb521c77. Not in the Arch repositories, only in the AUR.
  The implementation is compiled once in `src/gpu/vma.cpp`.

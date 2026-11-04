#include "gpu/context.hpp"

#include <sys/sysmacros.h>
#include <wayland-client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/version.hpp"

// After volk: VMA picks the function pointers up from it.
#include <vk_mem_alloc.h>

namespace glint::gpu {

const char* result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        default: return "VK_ERROR_(other)";
    }
}

namespace {

constexpr std::uint32_t api_version = VK_API_VERSION_1_3;

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::ranges::any_of(list, [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice physical) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, list.data());
    return list;
}

std::filesystem::path pipeline_cache_path() {
    return paths::cache_dir() / "pipelines.bin";
}

}  // namespace

std::unique_ptr<Context> Context::create(wl_display* display, std::optional<dev_t> main_device,
                                         std::string& error) {
    std::unique_ptr<Context> context(new Context());
    error = context->init(display, main_device);
    if (!error.empty()) return nullptr;
    return context;
}

std::string Context::init(wl_display* display, std::optional<dev_t> main_device) {
    if (VkResult r = volkInitialize(); r != VK_SUCCESS) return failed("volkInitialize", r);
    if (volkGetInstanceVersion() < api_version) return "the Vulkan loader is older than 1.3";

    VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "glint",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "glint",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = api_version,
    };
    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    if (const char* v = std::getenv("GLINT_VALIDATION"); v != nullptr && v[0] == '1') {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
    VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    if (VkResult r = vkCreateInstance(&instance_info, nullptr, &instance_); r != VK_SUCCESS) {
        return failed("vkCreateInstance", r);
    }
    volkLoadInstanceOnly(instance_);

    // Device pick.
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance_, &count, physicals.data());

    struct Candidate {
        VkPhysicalDevice physical;
        std::uint32_t family;
        int score;
        std::string name;
    };
    std::vector<Candidate> candidates;
    for (VkPhysicalDevice physical : physicals) {
        auto extensions = device_extensions(physical);
        if (!has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
            !has_extension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
            continue;
        }
        VkPhysicalDeviceDrmPropertiesEXT drm{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        bool has_drm = has_extension(extensions, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        if (has_drm) props.pNext = &drm;
        vkGetPhysicalDeviceProperties2(physical, &props);
        if (props.properties.apiVersion < api_version) continue;

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        std::optional<std::uint32_t> family;
        for (std::uint32_t i = 0; i < family_count; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical, i, display)) {
                family = i;
                break;
            }
        }
        if (!family) continue;

        int score = 0;
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 10;
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 5;
        if (has_drm && main_device) {
            auto node = [](std::int64_t maj, std::int64_t min) {
                return makedev(static_cast<unsigned>(maj), static_cast<unsigned>(min));
            };
            if ((drm.hasPrimary && node(drm.primaryMajor, drm.primaryMinor) == *main_device) ||
                (drm.hasRender && node(drm.renderMajor, drm.renderMinor) == *main_device)) {
                score += 100;
            }
        }
        candidates.push_back({physical, *family, score, props.properties.deviceName});
    }
    if (candidates.empty()) return "no Vulkan 1.3 device can present to this Wayland display";
    auto best = std::ranges::max_element(candidates, {}, &Candidate::score);
    physical_ = best->physical;
    queue_family_ = best->family;
    device_name_ = best->name;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    max_image_dimension_ = props.limits.maxImageDimension2D;
    {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
        if (families[queue_family_].timestampValidBits > 0) {
            timestamp_period_ = props.limits.timestampPeriod;
        }
    }

    // Device.
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features13,
        .hostQueryReset = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &features12,
    };
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = extensions,
    };
    if (VkResult r = vkCreateDevice(physical_, &device_info, nullptr, &device_); r != VK_SUCCESS) {
        return failed("vkCreateDevice", r);
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Allocator.
    VmaAllocatorCreateInfo allocator_info{
        .physicalDevice = physical_,
        .device = device_,
        // Small blocks: everything but the pictures (uniforms, the staging ring) is small.
        .preferredLargeHeapBlockSize = 4u << 20,
        .instance = instance_,
        .vulkanApiVersion = api_version,
    };
    VmaVulkanFunctions functions{};
    vmaImportVulkanFunctionsFromVolk(&allocator_info, &functions);
    allocator_info.pVulkanFunctions = &functions;
    if (VkResult r = vmaCreateAllocator(&allocator_info, &allocator_); r != VK_SUCCESS) {
        return failed("vmaCreateAllocator", r);
    }

    VkSemaphoreTypeCreateInfo timeline_type{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_type,
    };
    if (VkResult r = vkCreateSemaphore(device_, &timeline_info, nullptr, &timeline_);
        r != VK_SUCCESS) {
        return failed("vkCreateSemaphore", r);
    }

    VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_,
    };
    if (VkResult r = vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
        r != VK_SUCCESS) {
        return failed("vkCreateCommandPool", r);
    }

    load_pipeline_cache();
    log::info("vulkan: {} (score {}), queue family {}, timestamps {}", device_name_, best->score,
              queue_family_, timestamp_period_ > 0 ? "yes" : "no");
    return {};
}

Context::~Context() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        drain();
        save_pipeline_cache();
        if (pipeline_cache_ != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
        if (timeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, timeline_, nullptr);
        if (allocator_ != VK_NULL_HANDLE) vmaDestroyAllocator(allocator_);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

std::uint64_t Context::submit(VkCommandBuffer cmd, std::span<const Wait> waits,
                              std::span<const VkSemaphore> signals) {
    std::vector<VkSemaphoreSubmitInfo> wait_infos;
    for (const Wait& wait : waits) {
        wait_infos.push_back({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = wait.semaphore,
            .stageMask = wait.stage,
        });
    }
    std::uint64_t value = submitted_ + 1;
    std::vector<VkSemaphoreSubmitInfo> signal_infos;
    signal_infos.push_back({
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = timeline_,
        .value = value,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    });
    for (VkSemaphore semaphore : signals) {
        signal_infos.push_back({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        });
    }
    VkCommandBufferSubmitInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = static_cast<std::uint32_t>(wait_infos.size()),
        .pWaitSemaphoreInfos = wait_infos.data(),
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmd_info,
        .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signal_infos.size()),
        .pSignalSemaphoreInfos = signal_infos.data(),
    };
    if (VkResult r = vkQueueSubmit2(queue_, 1, &submit_info, VK_NULL_HANDLE); r != VK_SUCCESS) {
        log::error("vkQueueSubmit2: {}", result_name(r));
        return submitted_;
    }
    submitted_ = value;
    return value;
}

std::uint64_t Context::completed() const {
    std::uint64_t value = 0;
    vkGetSemaphoreCounterValue(device_, timeline_, &value);
    return value;
}

void Context::wait(std::uint64_t value) const {
    if (value == 0) return;
    VkSemaphoreWaitInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &timeline_,
        .pValues = &value,
    };
    vkWaitSemaphores(device_, &info, UINT64_MAX);
}

void Context::defer(std::function<void()> destroy) {
    deferred_.push_back({submitted_, std::move(destroy)});
}

bool Context::collect() {
    if (deferred_.empty()) return false;
    std::uint64_t done = completed();
    std::vector<Deferred> due;
    std::erase_if(deferred_, [&](Deferred& d) {
        if (d.value > done) return false;
        due.push_back(std::move(d));
        return true;
    });
    for (auto& d : due) d.destroy();
    return !deferred_.empty();
}

void Context::drain() {
    if (deferred_.empty()) return;
    wait(submitted_);
    collect();
}

VkCommandBuffer Context::allocate_command_buffer() const {
    VkCommandBufferAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &info, &cmd);
    return cmd;
}

void Context::free_command_buffer(VkCommandBuffer cmd) const {
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void Context::load_pipeline_cache() {
    std::vector<char> data;
    {
        std::ifstream in(pipeline_cache_path(), std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), {});
    }
    // The driver checks the header (vendor, device, cache UUID) and ignores data that does
    // not match, so a cache from another driver version is harmless.
    VkPipelineCacheCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data.size(),
        .pInitialData = data.empty() ? nullptr : data.data(),
    };
    if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_);
        data.clear();
    }
    pipeline_cache_size_ = data.size();
    log::debug("pipeline cache: {} bytes from disk", data.size());
}

void Context::save_pipeline_cache() {
    if (pipeline_cache_ == VK_NULL_HANDLE) return;
    std::size_t size = 0;
    vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr);
    if (size == 0 || size == pipeline_cache_size_) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(paths::cache_dir(), ec);
    auto path = pipeline_cache_path();
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(size));
        if (!out) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (!ec) {
        pipeline_cache_size_ = size;
        log::debug("pipeline cache: {} bytes written", size);
    }
}

}  // namespace glint::gpu

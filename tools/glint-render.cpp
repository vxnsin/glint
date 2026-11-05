// glint-render: draws a Wallpaper Engine scene headless into a PNG, no compositor needed.
// For looking at a scene while no output is free, and for regression pictures.
//
//   glint-render <item> <out.png> [--size WxH] [--frames N] [--fps F] [--pointer X,Y]
//                [--prop name=value ...] [--media "title|artist|album|cover.png"]
//
// N frames are drawn at 1/F seconds each (scripts, timelines and particles run on that
// clock); the last one is written. The pointer is a share of the output, 0 to 1, y down.
// --media plays a track for the scene: its media events and the cover in its thumbnail
// slot, as the daemon would give them from MPRIS.

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/log.hpp"
#include "core/paths.hpp"
#include "gpu/blit.hpp"
#include "gpu/buffer.hpp"
#include "gpu/context.hpp"
#include "gpu/staging.hpp"
#include "gpu/surface.hpp"
#include "gpu/texture.hpp"
#include "gpu/vulkan.hpp"
#include "wallpaper/image/decode.hpp"
#include "wallpaper/scene/assets.hpp"
#include "wallpaper/scene/renderer.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace {

using namespace glint;

int usage() {
    std::fprintf(stderr,
                 "usage: glint-render <item> <out.png> [--size WxH] [--frames N] [--fps F] "
                 "[--pointer X,Y] [--prop name=value ...]\n");
    return 2;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::filesystem::path item = argv[1];
    std::filesystem::path out = argv[2];
    std::uint32_t width = 1920, height = 1080;
    int frames = 1;
    double fps = 60;
    float pointer[2] = {0.5f, 0.5f};
    std::vector<std::pair<std::string, std::string>> props;
    std::string media;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (arg != name) return nullptr;
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (const char* v = value("--size")) {
            if (std::sscanf(v, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
                return usage();
            }
        } else if (const char* v2 = value("--frames")) {
            frames = std::max(1, std::atoi(v2));
        } else if (const char* v3 = value("--fps")) {
            fps = std::max(1.0, std::atof(v3));
        } else if (const char* v4 = value("--pointer")) {
            if (std::sscanf(v4, "%f,%f", &pointer[0], &pointer[1]) != 2) return usage();
        } else if (const char* v5 = value("--prop")) {
            std::string text = v5;
            auto eq = text.find('=');
            if (eq == std::string::npos) return usage();
            props.emplace_back(text.substr(0, eq), text.substr(eq + 1));
        } else if (const char* v6 = value("--media")) {
            media = v6;
        } else {
            return usage();
        }
    }

    if (const char* level = std::getenv("GLINT_LOG"); level != nullptr && level[0] == 'd') {
        log::set_level(log::Level::debug);
    }
    std::string error;
    auto gpu = gpu::Context::create(nullptr, std::nullopt, error);
    if (!gpu) {
        std::fprintf(stderr, "vulkan: %s\n", error.c_str());
        return 1;
    }
    {
        gpu::StagingRing staging(*gpu, 64u << 20);
        gpu::Blit blit(*gpu);
        auto assets = scene::SceneAssets::load(item, scene::default_assets(),
                                               paths::cache_dir() / "spirv", *gpu, staging, error);
        if (!assets) {
            std::fprintf(stderr, "%s: %s\n", item.c_str(), error.c_str());
            return 1;
        }
        assets->finish();
        for (const auto& w : assets->warnings()) std::fprintf(stderr, "warning: %s\n", w.c_str());
        scene::SceneRenderer renderer(*gpu, blit, assets);
        if (!props.empty()) {
            scene::Values values = renderer.values();
            for (const auto& [name, text] : props) {
                nlohmann::json v = nlohmann::json::parse(text, nullptr, false);
                values[name] = v.is_discarded() ? nlohmann::json(text) : v;
            }
            renderer.set_values(values);
        }

        if (!media.empty()) {
            // "title|artist|album|cover": a playing track with its cover.
            MediaState state;
            std::vector<std::string> parts;
            std::size_t start = 0;
            for (;;) {
                auto bar = media.find('|', start);
                parts.push_back(media.substr(
                    start, bar == std::string::npos ? std::string::npos : bar - start));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            state.available = true;
            state.playback = 1;
            state.title = parts.size() > 0 ? parts[0] : "";
            state.artist = parts.size() > 1 ? parts[1] : "";
            state.album = parts.size() > 2 ? parts[2] : "";
            state.album_artist = state.artist;
            state.content_type = "music";
            state.duration = 240;
            state.position = 42;
            if (parts.size() > 3 && !parts[3].empty()) {
                image::Decoded cover;
                std::string e = image::decode(parts[3], 512, staging, cover);
                if (!e.empty()) {
                    std::fprintf(stderr, "cover %s: %s\n", parts[3].c_str(), e.c_str());
                } else {
                    state.thumbnail = gpu::Texture::upload_rgba(*gpu, staging, cover.pixels,
                                                                cover.width, cover.height);
                    state.has_thumbnail = state.thumbnail != nullptr;
                    state.thumbnail_width = cover.width;
                    state.thumbnail_height = cover.height;
                    float primary[3] = {0.2f, 0.3f, 0.8f}, white[3] = {1, 1, 1};
                    std::copy_n(primary, 3, state.primary);
                    std::copy_n(primary, 3, state.secondary);
                    std::copy_n(primary, 3, state.tertiary);
                    std::copy_n(white, 3, state.text_color);
                    std::copy_n(white, 3, state.high_contrast);
                }
            }
            state.properties_version = state.playback_version = state.thumbnail_version = 1;
            renderer.set_media(state);
        }

        // The picture: a colour image drawn into like a swapchain image, read back once.
        VkExtent2D extent{width, height};
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VmaAllocationCreateInfo alloc_info{.usage = VMA_MEMORY_USAGE_AUTO};
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        if (vmaCreateImage(gpu->allocator(), &image_info, &alloc_info, &image, &allocation,
                           nullptr) != VK_SUCCESS) {
            std::fprintf(stderr, "cannot create the target image\n");
            return 1;
        }
        VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(gpu->device(), &view_info, nullptr, &view);
        gpu::Buffer uniforms(gpu->allocator(), sizeof(gpu::FrameUniforms),
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
        gpu::Buffer readback(gpu->allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);

        double delta = 1.0 / fps;
        double time = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        for (int frame = 0; frame < frames; ++frame) {
            scene::FrameInputs inputs;
            inputs.time = time;
            inputs.delta = frame == 0 ? 0 : delta;
            inputs.pointer[0] = pointer[0];
            inputs.pointer[1] = pointer[1];
            inputs.pointer_last[0] = pointer[0];
            inputs.pointer_last[1] = pointer[1];
            inputs.parallax[0] = pointer[0];
            inputs.parallax[1] = pointer[1];

            VkCommandBuffer cmd = gpu->allocate_command_buffer();
            VkCommandBufferBeginInfo begin{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            vkBeginCommandBuffer(cmd, &begin);
            renderer.record(cmd, extent, inputs);

            gpu::FrameUniforms fu{};
            fu.resolution[0] = static_cast<float>(width);
            fu.resolution[1] = static_cast<float>(height);
            fu.time = static_cast<float>(time);
            fu.delta = static_cast<float>(inputs.delta);
            fu.pointer[0] = pointer[0] * static_cast<float>(width);
            fu.pointer[1] = pointer[1] * static_cast<float>(height);
            std::memcpy(uniforms.mapped(), &fu, sizeof(fu));
            uniforms.flush(0, VK_WHOLE_SIZE);

            barrier(cmd, image, layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkRenderingAttachmentInfo color{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = view,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            };
            color.clearValue.color = {{0, 0, 0, 1}};
            VkRenderingInfo info{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea = {{0, 0}, extent},
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &color,
            };
            vkCmdBeginRendering(cmd, &info);
            VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            gpu::Target target{cmd, extent, format, {uniforms.get(), 0, sizeof(fu)}, true};
            renderer.composite(target, 1.0f);
            vkCmdEndRendering(cmd);

            if (frame + 1 == frames) {
                barrier(cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                VkBufferImageCopy copy{
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .imageExtent = {width, height, 1},
                };
                vkCmdCopyImageToBuffer(cmd, image, layout, readback.get(), 1, &copy);
            }
            vkEndCommandBuffer(cmd);
            std::uint64_t value = gpu->submit(cmd, {}, {});
            gpu->wait(value);
            gpu->free_command_buffer(cmd);
            time += delta;
        }
        readback.invalidate(0, VK_WHOLE_SIZE);
        std::string written = image::write_png(
            out, std::span<const std::byte>(readback.mapped(), static_cast<std::size_t>(bytes)),
            width, height, false);
        if (!written.empty()) std::fprintf(stderr, "%s: %s\n", out.c_str(), written.c_str());
        for (const auto& w : renderer.warnings()) std::fprintf(stderr, "warning: %s\n", w.c_str());
        std::printf("%s: %ux%u, %d frame%s, %zu draws\n", out.c_str(), width, height, frames,
                    frames == 1 ? "" : "s", renderer.draw_count());

        gpu->drain();
        vkDestroyImageView(gpu->device(), view, nullptr);
        vmaDestroyImage(gpu->allocator(), image, allocation);
        if (!written.empty()) return 1;
    }
    gpu->drain();
    return 0;
}

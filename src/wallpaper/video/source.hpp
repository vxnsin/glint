#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/player.hpp"
#include "gpu/buffer.hpp"
#include "gpu/dmabuf.hpp"
#include "gpu/vulkan.hpp"
#include "wallpaper/video/decoder.hpp"
#include "wallpaper/wallpaper.hpp"

VK_DEFINE_HANDLE(VmaAllocation)

namespace glint::video {

// One playing video: the decoder, the frame on screen (uploaded as three planes, or with
// VA-API imported as dma-bufs without a copy) and the sound track. Outputs showing the same
// file at the same speed and volume share one, so it is decoded and heard once and the
// screens stay on the same frame. Main thread.
class Source {
public:
    Source(std::filesystem::path path, const Options& options, Services& services);
    ~Source();
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    // Calls back on the main thread, at once when already loaded.
    void load(std::function<void(std::string error)> done);
    bool ready() const {
        return decoder_ != nullptr && (prime_ || planes_[0].image != VK_NULL_HANDLE);
    }

    // Shows the frame due on the source's clock, which runs while an output runs (the first
    // caller of a frame records the upload or the ownership transfer into its command
    // buffer; the queue orders the other outputs' frames after it).
    void prepare(VkCommandBuffer cmd);
    bool render(const gpu::Target& target, const Options& options, float opacity);

    // Decoding and sound run while at least one output runs.
    void add_runner(int delta);
    std::uint64_t memory() const;
    const Info* info() const { return decoder_ ? &decoder_->info() : nullptr; }

private:
    struct Plane {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0;
        VkDeviceSize memory = 0;
        bool sampled = false;  // in SHADER_READ_ONLY layout
    };
    void make_planes(std::uint32_t width, std::uint32_t height);
    void present(Picture& picture, VkCommandBuffer cmd);
    void upload(const Picture& picture, VkCommandBuffer cmd);
    void show(Picture& picture, VkCommandBuffer cmd);  // a dma-buf frame
    void start_sound();

    std::filesystem::path path_;
    Options options_;
    Services& services_;
    std::unique_ptr<Decoder> decoder_;
    std::vector<std::function<void(std::string)>> waiting_;
    std::string error_;
    bool loading_ = false;
    std::array<Plane, 3> planes_{};
    std::array<gpu::Buffer, 2> staging_;
    std::uint32_t slot_ = 0;
    std::optional<Picture> first_;  // taken on the worker, shown with the first frame
    int runners_ = 0;
    double clock_ = 0;        // seconds run before since_
    std::int64_t since_ = 0;  // when the clock last started, while running
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // dma-buf frames: the decoder's surfaces imported once each, by buffer.
    struct Imported {
        std::unique_ptr<gpu::DmabufImage> y, uv;
    };
    bool prime_ = false;
    std::unordered_map<std::uint64_t, Imported> imports_;
    const Imported* shown_ = nullptr;
    std::shared_ptr<void> shown_hold_;  // keeps the decoder off the surface on screen

    // The file mapped for the sound track.
    const std::byte* mapped_ = nullptr;
    std::size_t mapped_size_ = 0;
    std::unique_ptr<audio::Player> player_;
};

// The sources in use, by file and the options that change what is decoded or heard.
class Sources {
public:
    std::shared_ptr<Source> get(const std::filesystem::path& path, const Options& options,
                                Services& services);

private:
    std::map<std::string, std::weak_ptr<Source>> sources_;
};

}  // namespace glint::video

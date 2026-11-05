#pragma once

#include <array>
#include <memory>

#include "audio/player.hpp"
#include "gpu/buffer.hpp"
#include "gpu/vulkan.hpp"
#include "wallpaper/video/decoder.hpp"
#include "wallpaper/wallpaper.hpp"

VK_DEFINE_HANDLE(VmaAllocation)

namespace glint::video {

// A video wallpaper: frames decoded on the decoder's thread, the frame due at the wallpaper's
// clock uploaded as three planes, drawn like a picture. Loops; speed and pause; the sound
// track plays only when a volume is set.
class Video final : public Wallpaper {
public:
    Video(std::filesystem::path path, const Options& options, Services& services);
    ~Video() override;

    Kind kind() const override { return Kind::video; }
    const std::filesystem::path& path() const override { return path_; }
    const Options& options() const override { return options_; }

    void load(std::function<void(std::string error)> done) override;
    bool ready() const override {
        return decoder_ != nullptr && planes_[0].image != VK_NULL_HANDLE;
    }
    void prepare(const Frame& frame, VkCommandBuffer cmd, VkExtent2D extent) override;
    bool render(const Frame& frame, const gpu::Target& target, float opacity) override;
    bool wants_frames() const override { return !paused_; }
    void pause() override;
    void resume() override;
    std::uint64_t memory() const override;

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
    void upload(const Picture& picture, VkCommandBuffer cmd);
    void start_sound();

    std::filesystem::path path_;
    Options options_;
    Services& services_;
    std::unique_ptr<Decoder> decoder_;
    std::array<Plane, 3> planes_{};
    std::array<gpu::Buffer, 2> staging_;
    std::uint32_t slot_ = 0;
    std::optional<Picture> first_;  // taken on the worker, uploaded with the first frame
    bool paused_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    // The file mapped for the sound track.
    const std::byte* mapped_ = nullptr;
    std::size_t mapped_size_ = 0;
    std::unique_ptr<audio::Player> player_;
};

}  // namespace glint::video

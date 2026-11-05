#pragma once

#include <memory>

#include "wallpaper/video/source.hpp"
#include "wallpaper/wallpaper.hpp"

namespace glint::video {

// A video wallpaper on one output: a Source shared with the other outputs that show the same
// file, drawn with this output's scaling. Loops; speed and pause; the sound track plays only
// when a volume is set.
class Video final : public Wallpaper {
public:
    Video(std::filesystem::path path, const Options& options, Services& services);
    ~Video() override;

    Kind kind() const override { return Kind::video; }
    const std::filesystem::path& path() const override { return path_; }
    const Options& options() const override { return options_; }

    void load(std::function<void(std::string error)> done) override;
    bool ready() const override { return loaded_ && source_->ready(); }
    void prepare(const Frame& frame, VkCommandBuffer cmd, VkExtent2D extent) override;
    bool render(const Frame& frame, const gpu::Target& target, float opacity) override;
    bool wants_frames() const override { return !paused_; }
    void pause() override;
    void resume() override;
    std::uint64_t memory() const override { return source_->memory(); }

    const Info* info() const { return source_->info(); }

private:
    std::filesystem::path path_;
    Options options_;
    std::shared_ptr<Source> source_;
    bool loaded_ = false;
    bool paused_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace glint::video

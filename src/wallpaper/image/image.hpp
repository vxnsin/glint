#pragma once

#include <memory>

#include "wallpaper/wallpaper.hpp"

namespace glint::gpu {
class Texture;
}

namespace glint::image {

// A still picture. Loading decodes on a worker and uploads with GPU mipmaps; after that it
// draws only when asked and never wants another frame.
class Image final : public Wallpaper {
public:
    Image(std::filesystem::path path, const Options& options, Services& services);

    Kind kind() const override { return Kind::image; }
    const std::filesystem::path& path() const override { return path_; }
    const Options& options() const override { return options_; }

    void load(std::function<void(std::string error)> done) override;
    bool ready() const override { return texture_ != nullptr; }
    bool render(const Frame& frame, const gpu::Target& target, float opacity) override;
    bool wants_frames() const override { return false; }
    std::uint64_t memory() const override;

    std::uint32_t width() const;
    std::uint32_t height() const;

private:
    std::filesystem::path path_;
    Options options_;
    Services& services_;
    std::shared_ptr<gpu::Texture> texture_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace glint::image

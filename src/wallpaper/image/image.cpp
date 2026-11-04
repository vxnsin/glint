#include "wallpaper/image/image.hpp"

#include "gpu/texture.hpp"
#include "wallpaper/image/textures.hpp"

namespace glint::image {

Image::Image(std::filesystem::path path, const Options& options, Services& services)
    : path_(std::move(path)), options_(options), services_(services) {}

void Image::load(std::function<void(std::string error)> done) {
    std::weak_ptr<bool> alive = alive_;
    services_.textures.request(
        path_, [this, alive, done = std::move(done)](const Textures::Result& result) {
            if (alive.expired()) return;
            texture_ = result.texture;
            done(result.error);
        });
}

bool Image::render(const Frame&, const gpu::Target& target, float opacity) {
    if (!texture_) return false;
    services_.pictures.draw(target, *texture_, options_.scaling, options_.linear, opacity);
    return true;
}

std::uint64_t Image::memory() const {
    return texture_ ? texture_->memory() : 0;
}

std::uint32_t Image::width() const {
    return texture_ ? texture_->width() : 0;
}

std::uint32_t Image::height() const {
    return texture_ ? texture_->height() : 0;
}

}  // namespace glint::image

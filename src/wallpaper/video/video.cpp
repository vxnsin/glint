#include "wallpaper/video/video.hpp"

namespace glint::video {

Video::Video(std::filesystem::path path, const Options& options, Services& services)
    : path_(std::move(path)),
      options_(options),
      source_(services.videos.get(path_, options_, services)) {
    source_->add_runner(1);
}

Video::~Video() {
    if (!paused_) source_->add_runner(-1);
}

void Video::load(std::function<void(std::string error)> done) {
    std::weak_ptr<bool> alive = alive_;
    source_->load([this, alive, done = std::move(done)](std::string error) {
        if (alive.expired()) return;
        loaded_ = error.empty();
        done(std::move(error));
    });
}

void Video::prepare(const Frame&, VkCommandBuffer cmd, VkExtent2D) {
    if (loaded_) source_->prepare(cmd);
}

bool Video::render(const Frame&, const gpu::Target& target, float opacity) {
    return loaded_ && source_->render(target, options_, opacity);
}

void Video::pause() {
    if (paused_) return;
    paused_ = true;
    source_->add_runner(-1);
}

void Video::resume() {
    if (!paused_) return;
    paused_ = false;
    source_->add_runner(1);
}

}  // namespace glint::video

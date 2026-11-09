#include "wallpaper/image/textures.hpp"

#include <sys/stat.h>

#include "core/log.hpp"
#include "core/worker.hpp"
#include "gpu/context.hpp"
#include "gpu/texture.hpp"
#include "wallpaper/image/decode.hpp"

namespace glint::image {

Textures::Textures(gpu::Context& gpu, gpu::StagingRing& staging, Workers& workers)
    : gpu_(gpu), staging_(staging), workers_(workers) {}

void Textures::request(const std::filesystem::path& path, Callback callback) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        callback({.error = "no such file"});
        return;
    }
    std::string key = path.string() + '\n' + std::to_string(info.st_size) + '\n' +
                      std::to_string(info.st_mtim.tv_sec) + '.' +
                      std::to_string(info.st_mtim.tv_nsec);

    if (auto it = ready_.find(key); it != ready_.end()) {
        if (auto texture = it->second.lock()) {
            callback({.texture = std::move(texture), .cached = true});
            return;
        }
        ready_.erase(it);
    }
    // Pictures no longer shown leave dead entries behind; a slideshow would collect them.
    if (ready_.size() > 64) std::erase_if(ready_, [](const auto& e) { return e.second.expired(); });
    auto& waiters = loading_[key];
    waiters.push_back(std::move(callback));
    if (waiters.size() > 1) return;  // already on its way

    auto decoded = std::make_shared<Decoded>();
    auto error = std::make_shared<std::string>();
    std::uint32_t max_dimension = gpu_.max_image_dimension();
    gpu::StagingRing& staging = staging_;
    workers_.post([path, max_dimension, &staging, decoded,
                   error] { *error = decode(path, max_dimension, staging, *decoded); },
                  [this, key, decoded, error] {
                      Result result;
                      result.error = *error;
                      result.decode_ms = decoded->decode_ms;
                      if (result.error.empty()) {
                          result.texture = gpu::Texture::upload_rgba(
                              gpu_, staging_, decoded->pixels, decoded->width, decoded->height);
                          if (!result.texture) {
                              result.error = "out of GPU memory";
                          } else {
                              ready_[key] = result.texture;
                          }
                      }
                      // Take the waiters first: a callback may request again.
                      auto node = loading_.extract(key);
                      if (node.empty()) return;
                      for (auto& waiter : node.mapped()) waiter(result);
                  });
}

}  // namespace glint::image

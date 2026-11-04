#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace glint {
class Workers;
namespace gpu {
class Context;
class StagingRing;
class Texture;
}  // namespace gpu
}  // namespace glint

namespace glint::image {

// Pictures by file (path, size, modification time): the same picture on several outputs is
// decoded and uploaded once. Entries live as long as some wallpaper holds the texture.
class Textures {
public:
    struct Result {
        std::shared_ptr<gpu::Texture> texture;
        std::string error;
        double decode_ms = 0;  // on the worker
        bool cached = false;
    };
    using Callback = std::function<void(const Result&)>;

    Textures(gpu::Context& gpu, gpu::StagingRing& staging, Workers& workers);

    // Calls back on the main thread, at once if the texture is already there.
    void request(const std::filesystem::path& path, Callback callback);

private:
    gpu::Context& gpu_;
    gpu::StagingRing& staging_;
    Workers& workers_;
    std::map<std::string, std::weak_ptr<gpu::Texture>> ready_;
    std::map<std::string, std::vector<Callback>> loading_;
};

}  // namespace glint::image

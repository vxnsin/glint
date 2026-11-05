#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/player.hpp"
#include "wallpaper/scene/renderer.hpp"
#include "wallpaper/wallpaper.hpp"

namespace glint::scene {

// Scene assets by item folder: two outputs showing one scene load it once.
class SceneCache {
public:
    struct Result {
        std::shared_ptr<SceneAssets> assets;
        std::string error;
    };
    using Callback = std::function<void(const Result&)>;

    SceneCache(gpu::Context& gpu, gpu::StagingRing& staging, Workers& workers);
    // Calls back on the main thread, at once when the scene is already loaded.
    void request(const std::filesystem::path& item, Callback callback);

private:
    gpu::Context& gpu_;
    gpu::StagingRing& staging_;
    Workers& workers_;
    std::map<std::string, std::weak_ptr<SceneAssets>> ready_;
    std::map<std::string, std::vector<Callback>> loading_;
};

// A Wallpaper Engine scene on one output.
class Scene final : public Wallpaper {
public:
    Scene(std::filesystem::path path, const Options& options, Services& services);
    ~Scene() override;

    Kind kind() const override { return Kind::scene; }
    const std::filesystem::path& path() const override { return path_; }
    const Options& options() const override { return options_; }

    void load(std::function<void(std::string error)> done) override;
    bool ready() const override { return renderer_ != nullptr; }
    void prepare(const Frame& frame, VkCommandBuffer cmd, VkExtent2D extent) override;
    bool render(const Frame& frame, const gpu::Target& target, float opacity) override;
    bool wants_frames() const override;
    std::uint64_t memory() const override;

    std::vector<Property> properties() const override;
    std::vector<std::string> set_property(std::string_view name, std::string_view json) override;
    std::vector<std::string> reset_properties() override;
    Values property_values() const;

    bool wants_audio() const override;
    void audio_spectrum(const float* left, const float* right) override;

    void pause() override;
    void resume() override;

    bool wants_cursor() const override;
    void cursor(const Cursor& cursor) override;

    const SceneAssets* assets() const { return assets_.get(); }
    std::size_t draw_count() const { return renderer_ ? renderer_->draw_count() : 0; }

private:
    std::filesystem::path path_;
    Options options_;
    Services& services_;
    std::shared_ptr<SceneAssets> assets_;
    std::unique_ptr<SceneRenderer> renderer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    VkExtent2D extent_{1, 1};
    float pointer_[2] = {0.5f, 0.5f};
    float pointer_last_[2] = {0.5f, 0.5f};
    float parallax_[2] = {0.5f, 0.5f};
    double last_time_ = 0;
    std::array<float, 64> audio_left_{}, audio_right_{};
    void start_sound();
    std::vector<File> sound_files_;  // what the player reads from, kept alive
    std::unique_ptr<audio::Player> player_;
    bool audio_ = false;
};

}  // namespace glint::scene

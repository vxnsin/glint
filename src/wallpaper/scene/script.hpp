#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/math.hpp"
#include "wallpaper/scene/scene.hpp"

// SceneScript (docs/FORMATS.md, 11): the scripts bound to a scene's fields, run on
// quickjs-ng. One host per scene on one output, driven from the render thread: scripts
// write into ScriptCells the renderer reads through Values, so the shared scene description
// is never touched.
namespace glint::scene {

// What the scripts see of the frame.
struct ScriptFrame {
    double time = 0;   // seconds the scene has run
    double delta = 0;  // since the last frame
    double daytime = 0;
    float canvas[2] = {1920, 1080};    // canvas units
    float screen[2] = {1920, 1080};    // output pixels
    float pointer_world[2] = {0, 0};   // canvas units, y up
    float pointer_screen[2] = {0, 0};  // output pixels, y down
    bool left_down = false;
    const float* audio_left = nullptr;  // 64 bands, or null
    const float* audio_right = nullptr;
};

// Playback state of a puppet's animation layer under a script.
struct AnimationLayerState {
    bool playing = true;
    double start = 0;
    double held = 0;
    bool held_set = false;
};

class ScriptHost {
public:
    ScriptHost(const SceneDesc& scene, const Project& project, std::string label,
               std::string storage_id);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    bool empty() const;
    std::size_t count() const;
    const std::vector<ScriptCell>& cells() const { return cells_; }

    // The object sizes scripts read (`thisLayer.size`): by image index.
    void set_size_callback(std::function<Vec2(int)> callback) { size_of_ = std::move(callback); }

    // init() for every script, then applyUserProperties with every value; once.
    void start(const Values& values, const ScriptFrame& frame);
    // Timers, update() for every script.
    void frame(const Values& values, const ScriptFrame& frame);
    // applyUserProperties with what differs.
    void values_changed(const Values& before, const Values& after);
    // Cursor events for the layers under the pointer (solid ones): enter, leave, move,
    // down, up, click. world in canvas units, y up.
    void cursor(const Values& values, const ScriptFrame& frame, bool down_edge, bool up_edge);

    // Bumped when a script changed something the plan depends on (visibility).
    std::uint64_t structure_version() const { return structure_; }
    // Text objects (by object id) whose string a script changed since the last call.
    std::vector<int> take_changed_texts();
    bool uses_pointer() const { return uses_pointer_; }
    bool uses_audio() const { return uses_audio_; }

    // The puppet animation layers as scripts left them: by image index; null when untouched.
    const std::vector<AnimationLayer>* animation_layers(int image) const;
    // The time an animation layer plays at, from the scene time (play/pause/rate).
    double layer_time(int image, int layer, double time) const;

    const std::vector<std::string>& warnings() const { return warnings_; }

    // The bridge's side (script.cpp); not for callers.
    struct Impl;
    struct Script;

private:
    std::unique_ptr<Impl> impl_;
    std::vector<ScriptCell> cells_;
    std::vector<std::string> warnings_;
    std::function<Vec2(int)> size_of_;
    std::uint64_t structure_ = 1;
    std::set<int> changed_texts_;
    bool uses_pointer_ = false;
    bool uses_audio_ = false;
    std::vector<Script> scripts_;
    friend struct Impl;
};

}  // namespace glint::scene

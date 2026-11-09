#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/math.hpp"
#include "media/state.hpp"
#include "wallpaper/scene/mdl.hpp"
#include "wallpaper/scene/scene.hpp"
#include "wallpaper/wallpaper.hpp"

// SceneScript (docs/FORMATS.md, 11): the scripts bound to a scene's fields, run on
// quickjs-ng. One host per scene on one output, driven from the render thread: scripts
// write into ScriptCells the renderer reads through Values, so the shared scene description
// is never touched.
namespace glint::scene {

class Vfs;

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

// A sound object under a script: whether it plays, its volume, and how often it was
// taken back to its start.
struct SoundState {
    bool playing = true;
    float volume = 1;
    std::uint64_t restarts = 0;
    std::uint64_t version = 0;
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
    // vfs: the item's files, for layers scripts create (null: createLayer is refused).
    ScriptHost(const SceneDesc& scene, const Project& project, std::string label,
               std::string storage_id, const Vfs* vfs = nullptr,
               std::function<const Mesh*(const std::string&)> mesh_lookup = {});
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    bool empty() const;
    std::size_t count() const;
    const std::vector<ScriptCell>& cells() const { return cells_; }
    // The scene as the scripts left it: its own copy, with the layers they created.
    const SceneDesc& scene() const { return scene_; }

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

    // The media events, as far as they are due (the renderer tracks the versions).
    using MediaEvents = ::glint::media::Updates;
    void media(const Values& values, const MediaState& state, const MediaEvents& events);
    bool uses_media() const { return uses_media_; }

    // Bumped when a script changed something the plan depends on (visibility).
    std::uint64_t structure_version() const { return structure_; }
    // Text objects (by object id) whose string a script changed since the last call.
    std::vector<int> take_changed_texts();
    bool uses_pointer() const { return uses_pointer_; }
    bool uses_audio() const { return uses_audio_; }

    // The sound objects as scripts left them, by sound index.
    const std::vector<SoundState>& sounds() const { return sounds_; }

    // The puppet animation layers as scripts left them: by image index; null when untouched.
    const std::vector<AnimationLayer>* animation_layers(int image) const;
    // Clip time in seconds, with play/pause/rate already applied.
    double layer_time(int image, int layer, double time) const;

    const std::vector<std::string>& warnings() const { return warnings_; }

    // The bridge's side (script.cpp); not for callers.
    struct Impl;
    struct Script;

private:
    SceneDesc scene_;
    std::unique_ptr<Impl> impl_;
    std::vector<ScriptCell> cells_;
    std::vector<std::string> warnings_;
    std::function<Vec2(int)> size_of_;
    std::uint64_t structure_ = 1;
    std::vector<SoundState> sounds_;
    std::set<int> changed_texts_;
    bool uses_pointer_ = false;
    bool uses_audio_ = false;
    bool uses_media_ = false;
    std::vector<Script> scripts_;
    friend struct Impl;
};

}  // namespace glint::scene

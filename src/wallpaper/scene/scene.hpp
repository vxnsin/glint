#pragma once

#include <array>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wallpaper/scene/project.hpp"

// The parsed description of a scene: scene.json with its image objects, and for each the
// model, material and effect files it points to (docs/FORMATS.md, 4 to 7). Nothing here
// touches the GPU. Parsing is lenient: a field that does not read degrades to its default
// and lands in warnings, it does not fail the scene (#20).
namespace glint::scene {

class Vfs;

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

// "1 2 3" or a number; missing components stay as in fallback.
Vec3 parse_vec3(const nlohmann::json& json, Vec3 fallback);
Vec2 parse_vec2(const nlohmann::json& json, Vec2 fallback);

// A timeline (the "animation" value source): one channel of keyframes per component, a cubic
// bezier between neighbouring keyframes. Handle x is a share of the segment (0.5 is half way),
// handle y an offset in value.
struct Keyframe {
    float frame = 0, value = 0;
    float back_x = 0, back_y = 0, front_x = 0, front_y = 0;
    bool back = false, front = false;  // handle enabled; otherwise the segment is straight
};

struct Animation {
    enum class Mode { single, loop, mirror };
    std::vector<std::vector<Keyframe>> channels;  // c0, c1, ...
    float fps = 30;
    float length = 0;  // frames
    Mode mode = Mode::single;

    // The components at a time in seconds since the scene started.
    std::vector<float> at(double seconds) const;
};

// A field that may be bound to a project property:
// plain value, {"value": v, "user": "name"} or {"value": v, "user": {"name", "condition"}}.
struct Bound {
    nlohmann::json value;
    std::string user;                      // property name, empty if not bound
    std::string condition;                 // with a condition: true when the property equals it
    bool script = false;                   // a SceneScript value (M5); the static value is used
    std::shared_ptr<Animation> animation;  // a timeline; the static value when null

    bool animated() const { return animation != nullptr; }
    // The value at a time: the timeline if there is one, else resolve(values).
    nlohmann::json at(const Values& values, double seconds) const;

    // The value with the current property values applied.
    nlohmann::json resolve(const Values& values) const;
    bool resolve_bool(const Values& values, bool fallback) const;
    float resolve_float(const Values& values, float fallback) const;
};
Bound parse_bound(const nlohmann::json& json);

struct MaterialPass {
    std::string shader;    // "genericimage2", "effects/iris"
    std::string blending;  // "translucent", "normal", "additive", ...
    std::string cullmode;
    bool depth_test = false;
    bool depth_write = false;
    std::vector<std::optional<std::string>> textures;  // g_TextureN; null = the input
    std::map<std::string, int> combos;
    std::map<std::string, nlohmann::json> constants;    // constantshadervalues
    std::map<std::string, std::string> user_constants;  // usershadervalues: uniform -> prop
};

struct Material {
    std::string path;
    std::vector<MaterialPass> passes;
};

struct EffectBind {
    std::string name;  // "previous" or an fbo name
    int index = 0;     // g_TextureN slot
};

struct EffectPass {
    std::string material_path;
    Material material;
    std::string target;  // fbo name, empty = the next ping-pong target
    std::vector<EffectBind> binds;
    std::string command;  // "", "copy" or "swap"
    std::string source;   // for commands
};

struct EffectFbo {
    std::string name;
    float scale = 1;                      // the fbo is the layer size divided by this
    std::uint32_t width = 0, height = 0;  // a fixed size instead, when set
    std::string format;
    bool repeat = false;  // "uvs": "repeat"
};

// One effect on one image object: the effect file plus the object's overrides per pass.
struct Effect {
    std::string file;
    int id = 0;
    std::string name;
    Bound visible;
    std::vector<EffectPass> passes;
    std::vector<EffectFbo> fbos;
    struct Override {
        std::map<std::string, int> combos;
        std::map<std::string, Bound> constants;
        std::vector<std::optional<std::string>> textures;
    };
    std::vector<Override> overrides;  // by pass index
};

// A puppet's animation layer: which animation of its .mdl plays, how fast, how strongly.
struct AnimationLayer {
    int animation = 0;
    float rate = 1;
    float blend = 1;
    bool visible = true;
    bool additive = false;
};

struct ImageObject {
    int id = 0;
    std::string name;
    int parent = -1;
    std::string attachment;  // a named point on the parent puppet's skeleton
    Bound origin, scale, angles, size;
    Bound visible, alpha, color, brightness;
    int color_blend_mode = 0;
    Vec2 parallax_depth{1, 1};
    bool copy_background = false;
    bool perspective = false;

    // From the model json.
    std::string model_path;
    bool autosize = false;
    bool fullscreen = false;
    bool passthrough = false;
    bool puppet = false;      // drawn as its mesh, posed by its animation layers
    std::string puppet_path;  // the .mdl
    std::vector<AnimationLayer> animation_layers;
    Vec2 model_size{0, 0};

    Material material;
    std::vector<Effect> effects;
};

struct ParticleDesc;

// A particle object: a system (particles/x.json) placed on the canvas.
struct ParticleObject {
    int id = 0;
    std::string name;
    std::string file;
    int parent = -1;  // the id of an image or particle object it moves with
    Bound origin, scale, angles;
    Bound visible;
    Vec2 parallax_depth{1, 1};
    nlohmann::json instance_override;    // rate, count, size, speed, lifetime, alpha, color...
    std::shared_ptr<ParticleDesc> desc;  // null when the file did not read
};

struct SoundObject {
    int id = 0;
    std::vector<std::string> files;
    std::string mode = "loop";  // loop, random, single
    Bound volume;
    Bound visible;
    float min_time = 1, max_time = 5;
    bool start_silent = false;  // plays only when a script starts it (M5)
};

struct SceneGeneral {
    Vec2 ortho{1920, 1080};  // orthogonalprojection, the canvas
    Bound clear_color;
    bool clear_enabled = true;
    bool camera_parallax = false;
    float parallax_amount = 0.5f;
    float parallax_delay = 0.1f;
    float parallax_mouse_influence = 0.5f;
    Bound bloom;               // the scene-wide bloom pass, M5
    bool perspective = false;  // no orthogonalprojection: a 3D scene
};

// The objects in drawing order, across kinds.
struct ObjectRef {
    enum class Kind { image, particle } kind;
    int index;  // into images or particles
};

struct SceneDesc {
    SceneGeneral general;
    std::vector<ImageObject> images;  // in drawing order among images
    std::vector<ParticleObject> particles;
    std::vector<ObjectRef> order;  // images and particles as scene.json lists them
    std::vector<SoundObject> sounds;
    // Objects of kinds a later milestone draws: text, light, model.
    std::map<std::string, int> skipped;
    std::vector<std::string> warnings;
};

// Reads scene.json and everything its image objects reference.
// Returns an error message only when there is no usable scene at all.
std::string load_scene(const Vfs& vfs, std::string_view file, SceneDesc& out);

// Material and effect JSON on their own, for tests and we-inspect.
bool parse_material(const Vfs& vfs, std::string_view path, Material& out,
                    std::vector<std::string>& warnings);

}  // namespace glint::scene

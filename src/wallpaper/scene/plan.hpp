#pragma once

#include <array>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "wallpaper/scene/scene.hpp"
#include "wallpaper/scene/shader.hpp"

// A scene turned into a flat list of draws, the way it is rendered every frame: no strings
// looked up, no JSON walked in the frame loop. Built at load and again when a property
// changes what is visible. CPU only.
//
// An image object without visible effects is one draw per material pass, straight into the
// scene target. With effects it gets a layer: the picture is copied into a layer target, the
// effect passes ping-pong between two layer targets (or write their named fbos), and the
// material passes draw the result into the scene with the object's transform.
namespace glint::scene {

struct PlanTexture {
    enum class Kind { none, file, target, framebuffer };
    Kind kind = Kind::none;
    std::string file;  // as the material names it, e.g. "masks/x"; materials/<file>.tex
    int target = -1;   // index into Plan::targets
};

struct PlanTarget {
    int object = -1;  // index into SceneDesc::images
    Vec2 size{0, 0};  // canvas units, scaled to the output later; pixels when fixed
    bool fixed = false;
    bool repeat = false;
    std::string name;         // fbo name, or "layer a"/"layer b"
    bool persistent = false;  // its own image, kept across frames (an fbo marked unique)
};

enum class Geometry {
    object,  // the object's quad in canvas units, through the scene's projection
    clip,    // a quad over the whole target, in clip space
};

struct PlanDraw {
    int object = -1;        // index into SceneDesc::images, -1 for a particle draw
    int particle = -1;      // index into SceneDesc::particles: one draw per system and child
    int child = -1;         // -1: the system itself; else the index of the child, depth first
    int effect = -1;        // index into the object's effects, -1 otherwise
    ShaderRequest request;  // empty name: glint's own copy pass
    std::string blending = "normal";
    Geometry geometry = Geometry::clip;
    int target = -1;  // -1: the scene target
    bool clear = false;
    bool crop = false;  // texture 0 is a file: use only its picture, not the padding
    std::array<PlanTexture, max_texture_slots> textures{};
    std::map<std::string, nlohmann::json> constants;  // by material key
    std::map<std::string, Bound> bound;               // by material key, effect overrides
    std::map<std::string, std::string> user;          // usershadervalues: uniform -> prop
};

struct Plan {
    std::vector<PlanDraw> draws;
    std::vector<PlanTarget> targets;
    // _rt_imageLayerComposite_<id>_a and _b: an object's picture in a target of its own,
    // drawn first in every frame, by name.
    std::map<std::string, int> composites;
    bool reads_framebuffer = false;
    std::vector<std::string> warnings;
};

// The picture size of a texture by its material name, when known.
using TextureSize = std::function<std::optional<Vec2>(const std::string& name)>;
// What the plan needs to know about a texture file beyond its size: the sprite-sheet frame
// count (0 when it is not one) and the FORMAT_ code the shaders switch on (0 for RGBA8).
struct PlanTextureInfo {
    std::uint32_t frames = 0;
    int format = 0;
};
using TextureInfoFn = std::function<PlanTextureInfo(const std::string& name)>;

// Object size: "size", else the model's, else the picture's; the canvas when fullscreen.
Vec2 object_size(const ImageObject& image, const SceneGeneral& general, const Values& values,
                 const TextureSize& texture_size);

// everything: plan hidden objects and effects too (to compile every shader at load).
Plan build_plan(const SceneDesc& scene, const Values& values, const TextureSize& texture_size,
                bool everything = false, const TextureInfoFn& texture_info = {});

// Whether an object is shown: its own visible, and its ancestors' (groups and images)
// unless an ancestor has disablepropagation. id is the object's own id.
bool visible_with_parents(const SceneDesc& scene, int id, const Values& values);

// A stable key for a shader request, for caches.
std::string request_key(const ShaderRequest& request);

}  // namespace glint::scene

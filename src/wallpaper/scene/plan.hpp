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
    std::string name;  // fbo name, or "layer a"/"layer b"
};

enum class Geometry {
    object,  // the object's quad in canvas units, through the scene's projection
    clip,    // a quad over the whole target, in clip space
};

struct PlanDraw {
    int object = -1;
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
    bool reads_framebuffer = false;
    std::vector<std::string> warnings;
};

// The picture size of a texture by its material name, when known.
using TextureSize = std::function<std::optional<Vec2>(const std::string& name)>;

// Object size: "size", else the model's, else the picture's; the canvas when fullscreen.
Vec2 object_size(const ImageObject& image, const SceneGeneral& general, const Values& values,
                 const TextureSize& texture_size);

// everything: plan hidden objects and effects too (to compile every shader at load).
Plan build_plan(const SceneDesc& scene, const Values& values, const TextureSize& texture_size,
                bool everything = false);

// A stable key for a shader request, for caches.
std::string request_key(const ShaderRequest& request);

}  // namespace glint::scene

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/math.hpp"

// The mesh of a Wallpaper Engine puppet (.mdl, docs/FORMATS.md, 10): its vertices with the
// bones that move them, the skeleton's rest pose and the animations. The rest pose is the
// atlas the picture is cut from; an animation's keyframes put the parts together.
namespace glint::scene {

struct Mesh {
    std::vector<float> vertices;         // per vertex: x, y, z, u, v (object pixels, texture uv)
    std::vector<std::uint16_t> indices;  // triangles
    std::string material;

    // Per vertex, the four bones that move it and their weights; empty without a skeleton.
    std::vector<std::array<std::uint32_t, 4>> bone_indices;
    std::vector<std::array<float, 4>> bone_weights;

    // The skeleton's rest pose: per bone its parent and its matrix relative to the parent
    // (column-major), and named attachment points on bones that other objects hang from.
    struct Bone {
        std::string name;  // often empty
        int parent = -1;
        float matrix[16];
    };
    std::vector<Bone> bones;
    struct Attachment {
        std::string name;
        int bone = 0;
        float matrix[16];
    };
    std::vector<Attachment> attachments;

    // An animation: per bone a track of one keyframe per frame, each the bone's transform
    // relative to its parent (translation, rotation in radians about x, y, z, scale).
    struct Keyframe {
        float translation[3];
        float rotation[3];
        float scale[3];
    };
    struct Animation {
        int id = 0;
        std::string name;
        enum class Mode { single, loop, mirror } mode = Mode::loop;
        float fps = 30;
        std::uint32_t length = 0;                   // frames; a track holds length + 1 keyframes
        std::vector<std::vector<Keyframe>> tracks;  // by bone
    };
    std::vector<Animation> animations;
    const Animation* animation(int id) const;

    // The frame of an animation at a time in seconds, by its mode (looping, mirroring or
    // holding the last frame), scaled by rate.
    static double frame_at(const Animation& animation, double seconds, double rate);

    // The bones' local transforms at a frame, interpolated between keyframes; a bone the
    // animation has no track for keeps its rest transform.
    void pose(const Animation& animation, double frame, std::vector<math::Mat4>& locals) const;
    // The matrices that take a rest-pose vertex to its place under a pose: global(pose) *
    // inverse(global(rest)) per bone. locals as pose() gives them.
    void skin_matrices(const std::vector<math::Mat4>& locals, std::vector<math::Mat4>& out) const;
    // The rest pose's local transforms.
    void rest_pose(std::vector<math::Mat4>& locals) const;

    // Blend a layer into a pose initialized with rest_pose(), including the first layer.
    static void blend_pose(std::vector<math::Mat4>& locals, const std::vector<math::Mat4>& layer,
                           const std::vector<math::Mat4>& rest, float weight, bool additive);

    // Where an attachment sits in the mesh's space under a pose (locals as pose() gives
    // them; the rest pose when empty): the bone's global transform times the attachment's.
    // False if there is no such attachment.
    bool attachment_matrix(const std::string& name, const std::vector<math::Mat4>& locals,
                           math::Mat4& out) const;
};

// Returns an error message or empty.
std::string parse_mdl(std::span<const std::byte> data, Mesh& out);

}  // namespace glint::scene

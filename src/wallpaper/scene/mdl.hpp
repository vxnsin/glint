#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The mesh of a Wallpaper Engine puppet (.mdl, docs/FORMATS.md, 10), in its rest pose: what
// a puppet looks like before its bones move it. Bones and animations (MDLS, MDLA) are M5.
namespace glint::scene {

struct Mesh {
    std::vector<float> vertices;         // per vertex: x, y, z, u, v (object pixels, texture uv)
    std::vector<std::uint16_t> indices;  // triangles
    std::string material;

    // The skeleton's rest pose: per bone its parent and its matrix relative to the parent
    // (column-major), and named attachment points on bones that other objects hang from.
    struct Bone {
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

    // Where an attachment sits in the mesh's space in the rest pose (x, y); false if unknown.
    bool attachment_position(const std::string& name, float& x, float& y) const;
};

// Returns an error message or empty.
std::string parse_mdl(std::span<const std::byte> data, Mesh& out);

}  // namespace glint::scene

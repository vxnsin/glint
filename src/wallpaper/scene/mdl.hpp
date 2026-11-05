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
};

// Returns an error message or empty.
std::string parse_mdl(std::span<const std::byte> data, Mesh& out);

}  // namespace glint::scene

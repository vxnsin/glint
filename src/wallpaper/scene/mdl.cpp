#include "wallpaper/scene/mdl.hpp"

#include <cstring>

namespace glint::scene {

namespace {

// MDLV0023, read from the three puppets of the corpus: after the magic, three u32, the
// material path (NUL-terminated), seven u32 of zero, a u32 of flags (0x0180000f), the byte
// size of the vertices, the vertices at 80 bytes each (position 3, normal 3, tangent 4, bone
// indices 4 x u32, weights 4, uv 2), the byte size of the indices, u16 triangles.
constexpr std::size_t vertex_bytes = 80;

template <typename T>
bool read(std::span<const std::byte> data, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > data.size()) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

}  // namespace

std::string parse_mdl(std::span<const std::byte> data, Mesh& out) {
    if (data.size() < 9 || std::memcmp(data.data(), "MDLV", 4) != 0) return "not an .mdl file";
    std::size_t offset = 9 + 12;
    std::size_t end = offset;
    while (end < data.size() && data[end] != std::byte{0}) ++end;
    if (end >= data.size()) return "no material path";
    out.material.assign(reinterpret_cast<const char*>(data.data() + offset), end - offset);
    offset = end + 1 + 28;
    std::uint32_t flags = 0, vertex_size = 0, index_size = 0;
    if (!read(data, offset, flags) || !read(data, offset, vertex_size)) return "truncated";
    if (vertex_size % vertex_bytes != 0 || offset + vertex_size > data.size()) {
        return "unexpected vertex layout (flags " + std::to_string(flags) + ")";
    }
    std::size_t count = vertex_size / vertex_bytes;
    out.vertices.resize(count * 5);
    for (std::size_t i = 0; i < count; ++i) {
        const std::byte* v = data.data() + offset + i * vertex_bytes;
        float f[20];
        std::memcpy(f, v, sizeof(f));
        out.vertices[i * 5 + 0] = f[0];
        out.vertices[i * 5 + 1] = f[1];
        out.vertices[i * 5 + 2] = f[2];
        out.vertices[i * 5 + 3] = f[18];
        out.vertices[i * 5 + 4] = f[19];
    }
    offset += vertex_size;
    if (!read(data, offset, index_size) || index_size % 6 != 0 ||
        offset + index_size > data.size()) {
        return "truncated indices";
    }
    out.indices.resize(index_size / 2);
    std::memcpy(out.indices.data(), data.data() + offset, index_size);
    for (std::uint16_t i : out.indices) {
        if (i >= count) return "index out of range";
    }
    return {};
}

}  // namespace glint::scene

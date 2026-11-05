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

    // MDLS: u32 end offset, u16 bone count, per bone four bytes, u32 parent (-1 for the root),
    // u32 64, the 4x4 rest matrix relative to the parent, a NUL-terminated JSON string.
    // MDAT: u32 end offset, u16 count, per attachment u16 bone, the name NUL-terminated, a
    // 4x4 matrix relative to the bone. Read from the rigs of 2857144643 and 3426865175.
    auto find = [&](const char* magic) -> std::size_t {
        for (std::size_t i = offset; i + 8 <= data.size(); ++i) {
            if (std::memcmp(data.data() + i, magic, 4) == 0 && data[i + 8] == std::byte{0}) {
                return i;
            }
        }
        return data.size();
    };
    std::size_t skeleton = find("MDLS");
    if (skeleton < data.size()) {
        std::size_t at = skeleton + 9 + 4;
        std::uint16_t bones = 0;
        if (read(data, at, bones)) {
            for (std::uint16_t b = 0; b < bones; ++b) {
                Mesh::Bone bone;
                std::int32_t parent = -1;
                std::uint32_t size = 0;
                at += 4;
                if (!read(data, at, parent) || !read(data, at, size) || size != 64 ||
                    at + 64 > data.size()) {
                    out.bones.clear();
                    break;
                }
                std::memcpy(bone.matrix, data.data() + at, 64);
                at += 64;
                while (at < data.size() && data[at] != std::byte{0}) ++at;
                ++at;
                bone.parent = parent;
                out.bones.push_back(bone);
            }
        }
    }
    std::size_t attach = find("MDAT");
    if (attach < data.size()) {
        std::size_t at = attach + 9 + 4;
        std::uint16_t n = 0;
        if (read(data, at, n)) {
            for (std::uint16_t a = 0; a < n; ++a) {
                Mesh::Attachment attachment;
                std::uint16_t bone = 0;
                if (!read(data, at, bone)) break;
                std::size_t name = at;
                while (at < data.size() && data[at] != std::byte{0}) ++at;
                attachment.name.assign(reinterpret_cast<const char*>(data.data() + name),
                                       at - name);
                ++at;
                if (at + 64 > data.size()) break;
                std::memcpy(attachment.matrix, data.data() + at, 64);
                at += 64;
                attachment.bone = bone;
                out.attachments.push_back(std::move(attachment));
            }
        }
    }
    return {};
}

bool Mesh::attachment_position(const std::string& name, float& x, float& y) const {
    for (const Attachment& a : attachments) {
        if (a.name != name) continue;
        // Rest pose: the bone chain's translations plus the attachment's (the rigs seen have
        // no rotation in the rest pose).
        x = a.matrix[12];
        y = a.matrix[13];
        int bone = a.bone;
        for (int guard = 0; bone >= 0 && bone < static_cast<int>(bones.size()) && guard < 256;
             ++guard) {
            x += bones[static_cast<std::size_t>(bone)].matrix[12];
            y += bones[static_cast<std::size_t>(bone)].matrix[13];
            bone = bones[static_cast<std::size_t>(bone)].parent;
        }
        return true;
    }
    return false;
}

}  // namespace glint::scene

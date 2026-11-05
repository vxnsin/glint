#include "wallpaper/scene/mdl.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace glint::scene {

namespace {

// After the magic, three u32, the material path (NUL-terminated), seven u32 of zero, a u32
// of flags (0x0180000f), the byte size of the vertices, the vertices at 80 bytes each
// (position 3, normal 3, tangent 4, bone indices 4 x u32, weights 4, uv 2), the byte size of
// the indices, u16 triangles. Seen from MDLV0013 to MDLV0023.
constexpr std::size_t vertex_bytes = 80;

template <typename T>
bool read(std::span<const std::byte> data, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > data.size()) return false;
    std::memcpy(&out, data.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool read_string(std::span<const std::byte> data, std::size_t& offset, std::string& out) {
    std::size_t end = offset;
    while (end < data.size() && data[end] != std::byte{0}) ++end;
    if (end >= data.size()) return false;
    out.assign(reinterpret_cast<const char*>(data.data() + offset), end - offset);
    offset = end + 1;
    return true;
}

// A block "MDLx000n\0" at or after from; data.size() if there is none.
std::size_t find_block(std::span<const std::byte> data, std::size_t from, const char* magic) {
    for (std::size_t i = from; i + 9 <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, magic, 4) == 0 && data[i + 8] == std::byte{0}) return i;
    }
    return data.size();
}

int block_version(std::span<const std::byte> data, std::size_t at) {
    return std::atoi(std::string(reinterpret_cast<const char*>(data.data() + at + 4), 4).c_str());
}

math::Mat4 matrix_of(const float m[16]) {
    math::Mat4 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

math::Mat4 transform_of(const Mesh::Keyframe& k) {
    return math::Mat4::translate(k.translation[0], k.translation[1], k.translation[2]) *
           math::Mat4::rotate_z(k.rotation[2]) * math::Mat4::rotate_y(k.rotation[1]) *
           math::Mat4::rotate_x(k.rotation[0]) *
           math::Mat4::scale(k.scale[0], k.scale[1], k.scale[2]);
}

}  // namespace

std::string parse_mdl(std::span<const std::byte> data, Mesh& out) {
    if (data.size() < 9 || std::memcmp(data.data(), "MDLV", 4) != 0) return "not an .mdl file";
    std::size_t offset = 9 + 12;
    if (!read_string(data, offset, out.material)) return "no material path";
    offset += 28;
    std::uint32_t flags = 0, vertex_size = 0, index_size = 0;
    if (!read(data, offset, flags) || !read(data, offset, vertex_size)) return "truncated";
    if (vertex_size % vertex_bytes != 0 || offset + vertex_size > data.size()) {
        return "unexpected vertex layout (flags " + std::to_string(flags) + ")";
    }
    std::size_t count = vertex_size / vertex_bytes;
    out.vertices.resize(count * 5);
    out.bone_indices.resize(count);
    out.bone_weights.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::byte* v = data.data() + offset + i * vertex_bytes;
        float f[20];
        std::memcpy(f, v, sizeof(f));
        out.vertices[i * 5 + 0] = f[0];
        out.vertices[i * 5 + 1] = f[1];
        out.vertices[i * 5 + 2] = f[2];
        out.vertices[i * 5 + 3] = f[18];
        out.vertices[i * 5 + 4] = f[19];
        std::uint32_t bones[4];
        std::memcpy(bones, v + 40, sizeof(bones));
        for (int k = 0; k < 4; ++k) {
            out.bone_indices[i][static_cast<std::size_t>(k)] =
                static_cast<std::uint8_t>(std::min<std::uint32_t>(bones[k], 255));
            out.bone_weights[i][static_cast<std::size_t>(k)] = f[14 + k];
        }
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
    offset += index_size;

    // MDLS: u32 end offset, u32 bone count, per bone its name NUL-terminated (often empty),
    // u32 (1), i32 parent (-1 for the root), u32 64, the 4x4 rest matrix relative to the
    // parent, a NUL-terminated JSON string with the physics settings. More per-bone data
    // follows up to the end offset, not read.
    std::size_t skeleton = find_block(data, offset, "MDLS");
    if (skeleton < data.size()) {
        std::size_t at = skeleton + 9;
        std::uint32_t end = 0, bones = 0;
        if (read(data, at, end) && read(data, at, bones) && bones < 1024) {
            for (std::uint32_t b = 0; b < bones; ++b) {
                Mesh::Bone bone;
                std::int32_t parent = -1;
                std::uint32_t size = 0, flag = 0;
                std::string json;
                if (!read_string(data, at, bone.name) || !read(data, at, flag) ||
                    !read(data, at, parent) || !read(data, at, size) || size != 64 ||
                    at + 64 > data.size()) {
                    out.bones.clear();
                    break;
                }
                std::memcpy(bone.matrix, data.data() + at, 64);
                at += 64;
                if (!read_string(data, at, json)) {
                    out.bones.clear();
                    break;
                }
                bone.parent = parent < static_cast<std::int32_t>(bones) ? parent : -1;
                out.bones.push_back(bone);
            }
        }
    }
    std::size_t attach = find_block(data, offset, "MDAT");
    if (attach < data.size()) {
        std::size_t at = attach + 9 + 4;
        std::uint16_t n = 0;
        if (read(data, at, n)) {
            for (std::uint16_t a = 0; a < n; ++a) {
                Mesh::Attachment attachment;
                std::uint16_t bone = 0;
                if (!read(data, at, bone) || !read_string(data, at, attachment.name)) break;
                if (at + 64 > data.size()) break;
                std::memcpy(attachment.matrix, data.data() + at, 64);
                at += 64;
                attachment.bone = bone;
                out.attachments.push_back(std::move(attachment));
            }
        }
    }

    // MDLA: u32 end offset, u32 count; per animation u32 id, u32 zero, the name, the mode
    // ("loop", "mirror", "single"), f32 fps, u32 length in frames, u32 zero, u32 track count,
    // per track u32 zero, u32 byte size, then length + 1 keyframes of nine floats; then a few
    // zero bytes (by version) and a u32 count of events, each f32 time and a JSON string.
    std::size_t anim = find_block(data, offset, "MDLA");
    if (anim < data.size() && !out.bones.empty()) {
        int version = block_version(data, anim);
        std::size_t trailer = version <= 2   ? 0
                              : version == 3 ? 5
                              : version == 4 ? 6
                              : version == 5 ? 30
                                             : 31;
        std::size_t at = anim + 9;
        std::uint32_t end = 0, total = 0;
        if (read(data, at, end) && read(data, at, total) && total < 256) {
            for (std::uint32_t a = 0; a < total; ++a) {
                Mesh::Animation animation;
                std::uint32_t id = 0, zero = 0, length = 0, tracks = 0;
                std::string mode;
                float fps = 30;
                if (!read(data, at, id) || !read(data, at, zero) ||
                    !read_string(data, at, animation.name) || !read_string(data, at, mode) ||
                    !read(data, at, fps) || !read(data, at, length) || !read(data, at, zero) ||
                    !read(data, at, tracks) || tracks > 1024 || length > 100000) {
                    break;
                }
                animation.id = static_cast<int>(id);
                animation.mode = mode == "mirror"   ? Mesh::Animation::Mode::mirror
                                 : mode == "single" ? Mesh::Animation::Mode::single
                                                    : Mesh::Animation::Mode::loop;
                animation.fps = fps > 0 ? fps : 30;
                animation.length = length;
                bool ok = true;
                for (std::uint32_t t = 0; t < tracks && ok; ++t) {
                    std::uint32_t size = 0;
                    if (!read(data, at, zero) || !read(data, at, size) ||
                        size != (length + 1) * sizeof(Mesh::Keyframe) || at + size > data.size()) {
                        ok = false;
                        break;
                    }
                    std::vector<Mesh::Keyframe> keys(length + 1);
                    std::memcpy(keys.data(), data.data() + at, size);
                    at += size;
                    animation.tracks.push_back(std::move(keys));
                }
                if (!ok) break;
                at += trailer;
                std::uint32_t events = 0;
                if (!read(data, at, events) || events > 1024) break;
                for (std::uint32_t e = 0; e < events; ++e) {
                    float time = 0;
                    std::string json;
                    if (!read(data, at, time) || !read_string(data, at, json)) break;
                }
                out.animations.push_back(std::move(animation));
            }
        }
    }
    return {};
}

const Mesh::Animation* Mesh::animation(int id) const {
    for (const Animation& a : animations) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

double Mesh::frame_at(const Animation& animation, double seconds, double rate) {
    double length = animation.length;
    if (length <= 0) return 0;
    double f = seconds * rate * animation.fps;
    switch (animation.mode) {
        case Animation::Mode::single: return std::clamp(f, 0.0, length);
        case Animation::Mode::loop: return std::fmod(std::max(f, 0.0), length);
        case Animation::Mode::mirror: {
            double p = std::fmod(std::max(f, 0.0), 2 * length);
            return p > length ? 2 * length - p : p;
        }
    }
    return 0;
}

void Mesh::rest_pose(std::vector<math::Mat4>& locals) const {
    locals.resize(bones.size());
    for (std::size_t b = 0; b < bones.size(); ++b) locals[b] = matrix_of(bones[b].matrix);
}

void Mesh::pose(const Animation& animation, double frame, std::vector<math::Mat4>& locals) const {
    rest_pose(locals);
    auto f0 = static_cast<std::size_t>(std::floor(frame));
    auto t = static_cast<float>(frame - static_cast<double>(f0));
    for (std::size_t b = 0; b < bones.size() && b < animation.tracks.size(); ++b) {
        const auto& track = animation.tracks[b];
        if (track.empty()) continue;
        std::size_t i0 = std::min(f0, track.size() - 1), i1 = std::min(f0 + 1, track.size() - 1);
        Keyframe k = track[i0];
        const Keyframe& n = track[i1];
        for (int c = 0; c < 3; ++c) {
            k.translation[c] += (n.translation[c] - k.translation[c]) * t;
            k.rotation[c] += (n.rotation[c] - k.rotation[c]) * t;
            k.scale[c] += (n.scale[c] - k.scale[c]) * t;
        }
        locals[b] = transform_of(k);
    }
}

void Mesh::skin_matrices(const std::vector<math::Mat4>& locals,
                         std::vector<math::Mat4>& out) const {
    std::size_t n = std::min(locals.size(), bones.size());
    std::vector<math::Mat4> global_pose(n), global_rest(n);
    for (std::size_t b = 0; b < n; ++b) {
        int parent = bones[b].parent;
        math::Mat4 rest = matrix_of(bones[b].matrix);
        if (parent >= 0 && static_cast<std::size_t>(parent) < b) {
            global_pose[b] = global_pose[static_cast<std::size_t>(parent)] * locals[b];
            global_rest[b] = global_rest[static_cast<std::size_t>(parent)] * rest;
        } else {
            global_pose[b] = locals[b];
            global_rest[b] = rest;
        }
    }
    out.resize(n);
    for (std::size_t b = 0; b < n; ++b) out[b] = global_pose[b] * global_rest[b].inverse();
}

bool Mesh::attachment_matrix(const std::string& name, const std::vector<math::Mat4>& locals,
                             math::Mat4& out) const {
    for (const Attachment& a : attachments) {
        if (a.name != name) continue;
        math::Mat4 global;
        // The chain from the root down to the bone.
        std::vector<int> chain;
        for (int bone = a.bone, guard = 0;
             bone >= 0 && bone < static_cast<int>(bones.size()) && guard < 256; ++guard) {
            chain.push_back(bone);
            bone = bones[static_cast<std::size_t>(bone)].parent;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto b = static_cast<std::size_t>(*it);
            global = global * (b < locals.size() ? locals[b] : matrix_of(bones[b].matrix));
        }
        out = global * matrix_of(a.matrix);
        return true;
    }
    return false;
}

}  // namespace glint::scene

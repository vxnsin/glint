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
    // Like an object's angles: z turns clockwise on the canvas, so it is negated for the
    // y-up canvas. With the sign the other way, hair strands bend against their curve and
    // their thin triangle strips fold into lines.
    return math::Mat4::translate(k.translation[0], k.translation[1], k.translation[2]) *
           math::Mat4::rotate_z(-k.rotation[2]) * math::Mat4::rotate_y(k.rotation[1]) *
           math::Mat4::rotate_x(k.rotation[0]) *
           math::Mat4::scale(k.scale[0], k.scale[1], k.scale[2]);
}

}  // namespace

std::string parse_mdl(std::span<const std::byte> data, Mesh& out) {
    if (data.size() < 9 || std::memcmp(data.data(), "MDLV", 4) != 0) return "not an .mdl file";
    std::size_t offset = 9 + 12;
    if (!read_string(data, offset, out.material)) return "no material path";
    // What follows the material differs by version. Usual (MDLV0021 on): seven u32, the
    // first sometimes an id and the rest zero, u32 flags (0x0180000f), u32 byte size,
    // vertices of 80 bytes (position, normal, tangent, four bone indices, four weights,
    // uv). MDLV0016: u32 0, u32 flags (0x01800009), u32 byte size, vertices of 52 bytes
    // (position, bones, weights, uv). MDLV0013: u32 0, u32 byte size, the same 52-byte
    // vertices. Flags 0x0181000e come with 84-byte vertices (one float more in the middle).
    // In every layout the uv is the last two floats, the weights the four before, the bone
    // indices the four before those.
    std::uint32_t head[7] = {};
    std::size_t peek = offset;
    for (std::uint32_t& h : head) {
        if (!read(data, peek, h)) return "truncated";
    }
    std::uint32_t flags = 0, vertex_size = 0, index_size = 0;
    bool zeros = true;
    for (int i = 1; i < 7; ++i) zeros = zeros && head[i] == 0;
    if (zeros) {
        offset = peek;
        if (!read(data, offset, flags) || !read(data, offset, vertex_size)) return "truncated";
    } else if (head[0] == 0 && (head[1] & 0xff000000u) == 0x01000000u) {
        flags = head[1];
        vertex_size = head[2];
        offset += 12;
    } else if (head[0] == 0) {
        vertex_size = head[1];
        offset += 8;
    } else {
        return "unexpected header (" + std::to_string(head[0]) + ")";
    }
    std::size_t stride = 0;
    switch (flags) {
        case 0: stride = 52; break;
        case 0x01800009u: stride = 52; break;
        case 0x0180000fu: stride = 80; break;
        case 0x0181000eu: stride = 84; break;
        default: break;
    }
    if (stride == 0 || vertex_size % stride != 0) {
        // Unknown flags: the stride that divides the block and keeps the uvs in range.
        stride = 0;
        for (std::size_t candidate : {80u, 52u, 84u, 56u, 60u, 64u, 68u, 72u, 76u, 88u, 92u, 96u}) {
            if (vertex_size % candidate != 0 || offset + vertex_size > data.size()) continue;
            bool plausible = true;
            std::size_t n = std::min<std::size_t>(vertex_size / candidate, 64);
            for (std::size_t i = 0; i < n && plausible; ++i) {
                float uv[2];
                std::memcpy(uv, data.data() + offset + i * candidate + candidate - 8, sizeof(uv));
                plausible = uv[0] >= -0.01f && uv[0] <= 1.01f && uv[1] >= -0.01f && uv[1] <= 1.01f;
            }
            if (plausible) {
                stride = candidate;
                break;
            }
        }
        if (stride == 0) {
            return "unexpected vertex layout (flags " + std::to_string(flags) + ")";
        }
    }
    if (offset + vertex_size > data.size()) return "truncated vertices";
    std::size_t count = vertex_size / stride;
    out.vertices.resize(count * 5);
    out.bone_indices.resize(count);
    out.bone_weights.resize(count);
    std::size_t floats = stride / 4;
    for (std::size_t i = 0; i < count; ++i) {
        const std::byte* v = data.data() + offset + i * stride;
        float f[32] = {};
        std::memcpy(f, v, std::min<std::size_t>(stride, sizeof(f)));
        out.vertices[i * 5 + 0] = f[0];
        out.vertices[i * 5 + 1] = f[1];
        out.vertices[i * 5 + 2] = f[2];
        out.vertices[i * 5 + 3] = f[floats - 2];
        out.vertices[i * 5 + 4] = f[floats - 1];
        std::uint32_t bones[4];
        std::memcpy(bones, v + (floats - 10) * 4, sizeof(bones));
        for (int k = 0; k < 4; ++k) {
            out.bone_indices[i][static_cast<std::size_t>(k)] =
                static_cast<std::uint8_t>(std::min<std::uint32_t>(bones[k], 255));
            out.bone_weights[i][static_cast<std::size_t>(k)] =
                f[floats - 6 + static_cast<std::size_t>(k)];
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

namespace {

// A bone's place in the mesh: its joint follows the parent (the local translation taken
// through the parent's transform), but its rotation and scale are its own, not the chain's
// sum. A hair strand of six bones at ten degrees each bends ten degrees along its length,
// not sixty; with the chain's sum its tip swept across the body and the triangles between
// neighbouring bones stretched into spikes.
math::Mat4 chain(const math::Mat4& parent, const math::Mat4& local) {
    float x = local.m[12], y = local.m[13], z = local.m[14];
    math::Mat4 r = local;
    r.m[12] = parent.m[0] * x + parent.m[4] * y + parent.m[8] * z + parent.m[12];
    r.m[13] = parent.m[1] * x + parent.m[5] * y + parent.m[9] * z + parent.m[13];
    r.m[14] = parent.m[2] * x + parent.m[6] * y + parent.m[10] * z + parent.m[14];
    return r;
}

}  // namespace

void Mesh::skin_matrices(const std::vector<math::Mat4>& locals,
                         std::vector<math::Mat4>& out) const {
    std::size_t n = std::min(locals.size(), bones.size());
    std::vector<math::Mat4> global_pose(n), global_rest(n);
    for (std::size_t b = 0; b < n; ++b) {
        int parent = bones[b].parent;
        math::Mat4 rest = matrix_of(bones[b].matrix);
        if (parent >= 0 && static_cast<std::size_t>(parent) < b) {
            global_pose[b] = chain(global_pose[static_cast<std::size_t>(parent)], locals[b]);
            global_rest[b] = chain(global_rest[static_cast<std::size_t>(parent)], rest);
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
        // The chain from the root down to the bone, joints following as in skin_matrices.
        std::vector<int> path;
        for (int bone = a.bone, guard = 0;
             bone >= 0 && bone < static_cast<int>(bones.size()) && guard < 256; ++guard) {
            path.push_back(bone);
            bone = bones[static_cast<std::size_t>(bone)].parent;
        }
        bool first = true;
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            auto b = static_cast<std::size_t>(*it);
            math::Mat4 local = b < locals.size() ? locals[b] : matrix_of(bones[b].matrix);
            global = first ? local : chain(global, local);
            first = false;
        }
        out = global * matrix_of(a.matrix);
        return true;
    }
    return false;
}

}  // namespace glint::scene

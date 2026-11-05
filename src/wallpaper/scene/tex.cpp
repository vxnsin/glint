#include "wallpaper/scene/tex.hpp"

#include <lz4.h>

#include <cstring>
#include <string_view>

namespace glint::scene {

namespace {

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    bool magic(std::string_view prefix, std::string& out) {
        // Nine bytes: eight characters and a NUL.
        if (offset_ + 9 > data_.size()) return false;
        out.assign(reinterpret_cast<const char*>(data_.data() + offset_), 8);
        offset_ += 9;
        return out.starts_with(prefix);
    }
    template <typename T>
    bool value(T& out) {
        if (offset_ + sizeof(T) > data_.size()) return false;
        std::memcpy(&out, data_.data() + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }
    bool bytes(std::size_t count, std::span<const std::byte>& out) {
        if (count > data_.size() - offset_) return false;
        out = data_.subspan(offset_, count);
        offset_ += count;
        return true;
    }
    bool done() const { return offset_ == data_.size(); }

private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

int version_of(const std::string& magic) {
    // "TEXB0004" -> 4
    int value = 0;
    for (std::size_t i = 4; i < magic.size(); ++i) {
        if (magic[i] < '0' || magic[i] > '9') return -1;
        value = value * 10 + (magic[i] - '0');
    }
    return value;
}

}  // namespace

std::string parse_tex(std::span<const std::byte> data, Tex& out) {
    Reader in(data);
    std::string magic;
    if (!in.magic("TEXV", magic)) return "not a .tex file";
    if (!in.magic("TEXI", magic)) return "no TEXI header";
    std::int32_t tw, th, iw, ih;
    std::uint32_t unknown;
    if (!in.value(out.format) || !in.value(out.flags) || !in.value(tw) || !in.value(th) ||
        !in.value(iw) || !in.value(ih) || !in.value(unknown)) {
        return "truncated header";
    }
    if (tw <= 0 || th <= 0 || iw <= 0 || ih <= 0 || tw > 32768 || th > 32768) {
        return "bad size";
    }
    out.tex_width = static_cast<std::uint32_t>(tw);
    out.tex_height = static_cast<std::uint32_t>(th);
    out.img_width = static_cast<std::uint32_t>(iw);
    out.img_height = static_cast<std::uint32_t>(ih);

    if (!in.magic("TEXB", out.container)) return "no TEXB block";
    int version = version_of(out.container);
    if (version < 1 || version > 4) return "unknown container " + out.container;
    std::int32_t image_count = 0;
    if (!in.value(image_count) || image_count < 1 || image_count > 4096) {
        return "bad image count";
    }
    if (version >= 3 && !in.value(out.image_format)) return "truncated header";
    if (version >= 4) {
        std::int32_t video = 0;
        if (!in.value(video)) return "truncated header";
        out.video = video != 0;
    }

    for (std::int32_t image = 0; image < image_count; ++image) {
        std::int32_t mip_count = 0;
        if (!in.value(mip_count) || mip_count < 1 || mip_count > 16) return "bad mip count";
        auto& mips = out.images.emplace_back();
        for (std::int32_t i = 0; i < mip_count; ++i) {
            TexMip mip;
            std::int32_t w, h, compressed = 0, raw = 0, stored = 0;
            if (!in.value(w) || !in.value(h)) return "truncated mip";
            if (version >= 2 && (!in.value(compressed) || !in.value(raw))) {
                return "truncated mip";
            }
            if (!in.value(stored) || stored < 0 || raw < 0) return "truncated mip";
            if (w < 1 || h < 1 || w > 32768 || h > 32768) return "bad mip size";
            if (raw > (256 << 20)) return "mip too large";
            if (!in.bytes(static_cast<std::size_t>(stored), mip.stored)) {
                return "mip data runs past the end";
            }
            // The bytes a mip of this size takes in the format; an embedded file (PNG, JPEG)
            // and a video (TEXB0004's flag, or bit 5 of the header flags in older files)
            // are read by their own decoders and checked there.
            if (!out.embedded() && !out.video && (out.flags & 32u) == 0) {
                std::uint64_t expect = 0;
                auto blocks = [](std::int32_t n) {
                    return static_cast<std::uint64_t>((n + 3) / 4);
                };
                switch (out.format) {
                    case 1:
                    case 2:
                    case 4: expect = blocks(w) * blocks(h) * 16; break;             // DXT3, DXT5
                    case 7: expect = blocks(w) * blocks(h) * 8; break;              // DXT1
                    case 8: expect = static_cast<std::uint64_t>(w) * h * 2; break;  // RG88
                    case 9: expect = static_cast<std::uint64_t>(w) * h; break;      // R8
                    case 0: expect = static_cast<std::uint64_t>(w) * h * 4; break;  // RGBA8
                    default: break;
                }
                std::uint64_t have = compressed == 1 ? static_cast<std::uint64_t>(raw)
                                                     : static_cast<std::uint64_t>(stored);
                if (expect != 0 && have < expect) return "mip data shorter than its size";
            }
            mip.width = static_cast<std::uint32_t>(w);
            mip.height = static_cast<std::uint32_t>(h);
            mip.lz4 = compressed == 1;
            mip.raw_size = static_cast<std::uint32_t>(raw);
            mips.push_back(mip);
        }
    }

    if (out.sprite_sheet() && !in.done()) {
        std::string sheet;
        if (!in.magic("TEXS", sheet)) return "no TEXS block";
        int sheet_version = version_of(sheet);
        std::int32_t frame_count = 0;
        if (!in.value(frame_count) || frame_count < 0 || frame_count > 65536) {
            return "bad frame count";
        }
        if (sheet_version >= 3) {
            std::int32_t w, h;
            if (!in.value(w) || !in.value(h)) return "truncated sprite sheet";
        }
        for (std::int32_t i = 0; i < frame_count; ++i) {
            TexFrame frame;
            float unused1, unused2;
            if (!in.value(frame.image) || !in.value(frame.time) || !in.value(frame.x) ||
                !in.value(frame.y) || !in.value(frame.width) || !in.value(unused1) ||
                !in.value(unused2) || !in.value(frame.height)) {
                return "truncated sprite sheet";
            }
            out.frames.push_back(frame);
        }
    }
    return {};
}

bool mip_bytes(const TexMip& mip, std::vector<std::byte>& out) {
    if (!mip.lz4) {
        out.assign(mip.stored.begin(), mip.stored.end());
        return true;
    }
    out.resize(mip.raw_size);
    int n = LZ4_decompress_safe(
        reinterpret_cast<const char*>(mip.stored.data()), reinterpret_cast<char*>(out.data()),
        static_cast<int>(mip.stored.size()), static_cast<int>(mip.raw_size));
    return n == static_cast<int>(mip.raw_size);
}

const char* tex_format_name(std::int32_t format) {
    switch (format) {
        case 0: return "RGBA8";
        case 1:
        case 2: return "DXT3";
        case 4: return "DXT5";
        case 7: return "DXT1";
        case 8: return "RG8";
        case 9: return "R8";
        default: return "unknown";
    }
}

}  // namespace glint::scene

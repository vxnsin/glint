#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Wallpaper Engine's .tex container (docs/FORMATS.md, 3). Parsing keeps views into the file;
// payloads are decompressed on request.
namespace glint::scene {

enum class TexFormat : std::int32_t {
    rgba8 = 0,  // raw RGBA, or a whole PNG/JPEG per mip when image_format >= 0
    dxt3 = 1,   // also 2 in some files
    dxt5 = 4,
    dxt1 = 7,
    rg8 = 8,
    r8 = 9,
};

struct TexMip {
    std::uint32_t width = 0, height = 0;
    bool lz4 = false;
    std::uint32_t raw_size = 0;         // size after LZ4, 0 when not compressed
    std::span<const std::byte> stored;  // as in the file
};

struct TexFrame {  // one sprite-sheet frame, rectangle in texels of the first image
    std::int32_t image = 0;
    float time = 0;  // seconds
    float x = 0, y = 0, width = 0, height = 0;
};

struct Tex {
    std::int32_t format = 0;
    std::uint32_t flags = 0;
    std::uint32_t tex_width = 0, tex_height = 0;  // padded size of the stored data
    std::uint32_t img_width = 0, img_height = 0;  // the real picture inside it
    std::string container;                        // "TEXB0004"
    std::int32_t image_format = -1;  // FreeImage type of embedded files (13 PNG, 2 JPEG), -1 none
    bool video = false;              // TEXB0004: the payload is an mp4
    std::vector<std::vector<TexMip>> images;  // [image][mip]
    std::vector<TexFrame> frames;             // sprite sheet, empty otherwise

    bool sprite_sheet() const { return (flags & 8u) != 0; }
    bool clamp_u() const { return (flags & 2u) != 0; }
    bool clamp_v() const { return (flags & 4u) != 0; }
    bool no_interpolation() const { return (flags & 1u) != 0; }
    // The mip payloads are whole PNG/JPEG files.
    bool embedded() const { return format == 0 && image_format >= 0; }
    bool block_compressed() const {
        return format == 1 || format == 2 || format == 4 || format == 7;
    }
};

// Returns an error message or empty. The Tex views into data, which must outlive it.
std::string parse_tex(std::span<const std::byte> data, Tex& out);

// The payload of one mip, LZ4 undone. Returns false on a corrupt block.
bool mip_bytes(const TexMip& mip, std::vector<std::byte>& out);

const char* tex_format_name(std::int32_t format);

}  // namespace glint::scene

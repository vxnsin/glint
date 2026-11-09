#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Text objects: a string set in a font at a point size, laid out as Wallpaper Engine lays it
// out (docs/FORMATS.md, 12) and rasterised to an RGBA bitmap that is drawn like a picture.
// FreeType rasterises, HarfBuzz shapes, and fontconfig finds system fonts ("systemfont_arial")
// and a fallback for glyphs the item's font lacks (CJK in a Latin font, say).
namespace glint::scene {

class Vfs;

struct TextSpec {
    std::string text;
    std::string font;      // "fonts/x.ttf" in the item or assets, or "systemfont_<name>"
    float pointsize = 32;  // the em is pointsize * 300 / 72 canvas units
    std::string horizontal_align = "center";  // left, center, right
    std::string vertical_align = "center";    // top, center, bottom
    std::array<float, 2> padding{32, 32};     // room around the glyphs, canvas units
    std::array<float, 2> spacing{0, 0};       // added to every advance and to the line height
    float max_width = 500;                    // wraps when limit_width
    int max_rows = 1;
    bool limit_rows = false;
    bool limit_width = false;
    bool ellipsis = false;
    bool block_align = false;  // justifies the lines the wrap broke to max_width
    std::array<float, 3> color{1, 1, 1};
    float alpha = 1;
    std::array<float, 3> background{0, 0, 0};
    bool opaque_background = false;
    float pixels_per_unit = 1;  // the bitmap's resolution
};

// The block: the lines' bounds plus the padding, placed around the object's origin by the
// alignment (its centre is `center` from the origin, y up, canvas units).
struct TextBitmap {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // straight alpha, rows top down
    std::array<float, 2> size{0, 0};
    std::array<float, 2> center{0, 0};
};

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Any thread, one at a time. An empty bitmap when nothing could be drawn (the warning
    // says why); a missing font falls back to the system's sans-serif.
    TextBitmap render(const TextSpec& spec, const Vfs& vfs, std::vector<std::string>& warnings);

    struct Face;  // a loaded font at one size

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
};

}  // namespace glint::scene

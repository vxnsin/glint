#include "wallpaper/scene/text.hpp"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <hb-ft.h>
#include <hb.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

#include "wallpaper/scene/vfs.hpp"

namespace glint::scene {

struct TextRenderer::Face {
    std::vector<std::byte> bytes;  // the face reads from this for its whole life
    FT_Face face = nullptr;
    hb_font_t* font = nullptr;
    float size = 0;  // the point size it is set to

    ~Face() {
        if (font != nullptr) hb_font_destroy(font);
        if (face != nullptr) FT_Done_Face(face);
    }
};

struct TextRenderer::Impl {
    FT_Library library = nullptr;
    std::map<std::string, std::shared_ptr<Face>> faces;  // by path
    std::string fallback_path;                           // a system font with wide coverage
    bool fontconfig = false;
};

namespace {

constexpr std::uint32_t max_side = 4096;

// The file of a system font by family through fontconfig; empty if none.
std::string system_font(const std::string& family) {
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (pattern == nullptr) return {};
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    std::string path;
    if (match != nullptr) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
            path = reinterpret_cast<const char*>(file);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return path;
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    std::vector<std::byte> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

// UTF-8 to code points; malformed bytes become U+FFFD.
std::vector<std::uint32_t> decode_utf8(const std::string& text) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f;
            extra = 2;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            out.push_back(0xfffd);
            ++i;
            continue;
        }
        if (i + static_cast<std::size_t>(extra) >= text.size() + (extra == 0 ? 1 : 0)) {
            out.push_back(0xfffd);
            break;
        }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            auto cc = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xc0) != 0x80) ok = false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        out.push_back(ok ? cp : 0xfffd);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

struct Glyph {
    TextRenderer::Face* face;
    std::uint32_t index;
    float x_advance;  // canvas units, floored as WE floors it, with the letter spacing
    float x_offset, y_offset;
    float box_min, box_max;  // the outline's horizontal extent from the pen, canvas units
    std::uint32_t cluster;   // code point index
    bool space;
    float x = 0;  // laid out: the pen position from the line's start
};

struct Line {
    std::vector<Glyph> glyphs;
    float min_x = 0, width = 0;  // WE's span: the boxes joined with the pen's start
    bool wrapped = false;        // broken by the wrap, not by the text
};

// Shapes the code points with the face, or the fallback for runs the face has no glyph for.
void shape(std::vector<Glyph>& out, const std::vector<std::uint32_t>& text, std::size_t from,
           std::size_t to, TextRenderer::Face& face, TextRenderer::Face* fallback, float ppu,
           float letter_spacing) {
    // Runs by which font has the glyph.
    std::size_t i = from;
    while (i < to) {
        bool primary =
            FT_Get_Char_Index(face.face, text[i]) != 0 || fallback == nullptr || text[i] == ' ';
        std::size_t j = i + 1;
        while (j < to) {
            bool p =
                FT_Get_Char_Index(face.face, text[j]) != 0 || fallback == nullptr || text[j] == ' ';
            if (p != primary) break;
            ++j;
        }
        TextRenderer::Face& f = primary ? face : *fallback;
        hb_buffer_t* buffer = hb_buffer_create();
        hb_buffer_add_codepoints(buffer, text.data(), static_cast<int>(text.size()),
                                 static_cast<unsigned>(i), static_cast<int>(j - i));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(f.font, buffer, nullptr, 0);
        unsigned count = 0;
        hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
        hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
        for (unsigned k = 0; k < count; ++k) {
            Glyph g{};
            g.face = &f;
            g.index = infos[k].codepoint;
            // WE advances the pen by whole units (HarfBuzz's 26.6 advance shifted down) and
            // measures a line by the glyphs' pixel boxes.
            g.x_advance =
                std::floor(static_cast<float>(positions[k].x_advance) / 64 / ppu) + letter_spacing;
            g.x_offset = std::floor(static_cast<float>(positions[k].x_offset) / 64 / ppu);
            g.y_offset = std::floor(static_cast<float>(positions[k].y_offset) / 64 / ppu);
            if (FT_Load_Glyph(f.face, g.index, FT_LOAD_DEFAULT) == 0) {
                const FT_Glyph_Metrics& m = f.face->glyph->metrics;
                if (m.width > 0) {
                    g.box_min = std::floor(static_cast<float>(m.horiBearingX) / 64 / ppu);
                    g.box_max = std::ceil(static_cast<float>(m.horiBearingX + m.width) / 64 / ppu);
                }
            }
            g.cluster = infos[k].cluster;
            std::uint32_t cp = g.cluster < text.size() ? text[g.cluster] : 0;
            g.space = cp == ' ' || cp == '\t' || cp == '\r';
            out.push_back(g);
        }
        hb_buffer_destroy(buffer);
        i = j;
    }
}

// Lays the glyphs out from the pen's start and measures WE's span: the boxes joined with
// the start, min(0, left) to max(0, right). The last glyph's letter spacing adds nothing.
void lay_out(Line& line, float justify_to = 0) {
    float extra = 0;
    if (justify_to > 0) {
        float pen = 0, min_x = 0, max_x = 0;
        int gaps = 0;
        for (const Glyph& g : line.glyphs) {
            min_x = std::min(min_x, pen + g.x_offset + g.box_min);
            max_x = std::max(max_x, pen + g.x_offset + g.box_max);
            pen += g.x_advance;
            if (g.space) ++gaps;
        }
        if (gaps > 0) extra = (justify_to - (max_x - min_x)) / static_cast<float>(gaps);
    }
    float pen = 0, min_x = 0, max_x = 0;
    for (Glyph& g : line.glyphs) {
        g.x = pen;
        min_x = std::min(min_x, pen + g.x_offset + g.box_min);
        max_x = std::max(max_x, pen + g.x_offset + g.box_max);
        pen += g.x_advance + (g.space ? extra : 0);
    }
    line.min_x = min_x;
    line.width = justify_to > 0 && extra != 0 ? justify_to : max_x - min_x;
}

float width_of(const std::vector<Glyph>& glyphs) {
    Line line;
    line.glyphs = glyphs;
    lay_out(line);
    return line.width;
}

}  // namespace

TextRenderer::TextRenderer() : impl_(std::make_unique<Impl>()) {
    FT_Init_FreeType(&impl_->library);
    impl_->fontconfig = FcInit() == FcTrue;
    if (impl_->fontconfig) {
        // Wide coverage for what an item's font lacks: Noto Sans CJK where installed.
        impl_->fallback_path = system_font("sans-serif:lang=ja");
        if (impl_->fallback_path.empty()) impl_->fallback_path = system_font("sans-serif");
    }
}

TextRenderer::~TextRenderer() {
    impl_->faces.clear();
    if (impl_->library != nullptr) FT_Done_FreeType(impl_->library);
}

TextBitmap TextRenderer::render(const TextSpec& spec, const Vfs& vfs,
                                std::vector<std::string>& warnings) {
    std::lock_guard lock(mutex_);
    TextBitmap bitmap;
    if (impl_->library == nullptr) return bitmap;

    auto load = [&](const std::string& key, std::vector<std::byte> bytes) -> std::shared_ptr<Face> {
        if (auto it = impl_->faces.find(key); it != impl_->faces.end()) return it->second;
        auto face = std::make_shared<Face>();
        face->bytes = std::move(bytes);
        if (face->bytes.empty() ||
            FT_New_Memory_Face(impl_->library, reinterpret_cast<const FT_Byte*>(face->bytes.data()),
                               static_cast<FT_Long>(face->bytes.size()), 0, &face->face) != 0) {
            face->face = nullptr;
            return nullptr;
        }
        impl_->faces[key] = face;
        return face;
    };
    auto set_size = [&](Face& face, float size, float ppu) {
        float key = size * ppu;
        if (face.size == key) return;
        // WE: FT_Set_Char_Size(face, 0, pointsize * 64, 300, 300), one atlas pixel per unit.
        auto dpi = static_cast<FT_UInt>(std::lround(300 * ppu));
        FT_Set_Char_Size(face.face, 0, static_cast<FT_F26Dot6>(std::lround(size * 64)), dpi, dpi);
        if (face.font != nullptr) hb_font_destroy(face.font);
        face.font = hb_ft_font_create_referenced(face.face);
        face.size = key;
    };

    std::shared_ptr<Face> face;
    if (spec.font.starts_with("systemfont_")) {
        // WE's table of system fonts; a name outside it is Arial, as in WE. fontconfig
        // substitutes what is installed (Liberation Sans for Arial, say).
        static const std::map<std::string, std::string, std::less<>> families = {
            {"arial", "Arial"},       {"calibri", "Calibri"},
            {"cambria", "Cambria"},   {"comicsans", "Comic Sans MS"},
            {"consolas", "Consolas"}, {"sansserif", "Microsoft Sans Serif"},
            {"segoe", "Segoe UI"},    {"verdana", "Verdana"},
        };
        std::string name = spec.font.substr(11);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = families.find(name);
        std::string family = it != families.end() ? it->second : "Arial";
        std::string path = impl_->fontconfig ? system_font(family) : "";
        if (!path.empty()) face = load(path, read_file(path));
    } else if (!spec.font.empty()) {
        if (auto file = vfs.read(spec.font)) {
            face =
                load(spec.font, std::vector<std::byte>(file->bytes().begin(), file->bytes().end()));
        }
    }
    if (!face) {
        if (!spec.font.empty())
            warnings.push_back("font " + spec.font + ": not found, sans-serif used");
        std::string path = impl_->fontconfig ? system_font("sans-serif") : "";
        if (!path.empty()) face = load(path, read_file(path));
        if (!face) return bitmap;
    }
    std::shared_ptr<Face> fallback;
    if (!impl_->fallback_path.empty() && impl_->fallback_path != spec.font) {
        fallback = load(impl_->fallback_path, read_file(impl_->fallback_path));
    }
    // WE sets the face to the point size at 300 dpi; the bitmap holds ppu pixels per unit.
    float ppu = std::clamp(spec.pixels_per_unit, 0.05f, 16.0f);
    float size = std::clamp(spec.pointsize, 1.0f, 256.0f);
    set_size(*face, size, ppu);
    if (fallback) set_size(*fallback, size, ppu);
    float letter_spacing = spec.spacing[0];

    // Lines: the text's own breaks, then, when the width is limited, wrapping at words (or
    // anywhere for a word that is wider than the limit).
    std::vector<std::uint32_t> points = decode_utf8(spec.text);
    std::vector<Line> lines;
    float limit = spec.limit_width && spec.max_width > 0 ? spec.max_width : 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= points.size(); ++i) {
        if (i < points.size() && points[i] != '\n') continue;
        std::vector<Glyph> shaped;
        shape(shaped, points, start, i, *face, fallback.get(), ppu, letter_spacing);
        start = i + 1;
        if (limit <= 0 || width_of(shaped) <= limit) {
            lines.push_back({std::move(shaped)});
            continue;
        }
        std::vector<Glyph> current;
        std::size_t last_space = std::string::npos;
        for (const Glyph& g : shaped) {
            current.push_back(g);
            if (width_of(current) > limit && current.size() > 1) {
                current.pop_back();
                if (last_space != std::string::npos && last_space > 0) {
                    std::vector<Glyph> rest(
                        current.begin() + static_cast<std::ptrdiff_t>(last_space) + 1,
                        current.end());
                    current.resize(last_space);
                    lines.push_back({std::move(current), 0, 0, true});
                    current = std::move(rest);
                } else {
                    lines.push_back({std::move(current), 0, 0, true});
                    current.clear();
                }
                last_space = std::string::npos;
                current.push_back(g);
            }
            if (g.space) last_space = current.size() - 1;
        }
        // A wrapped line sheds the space it broke at.
        while (!current.empty() && current.front().space) current.erase(current.begin());
        lines.push_back({std::move(current)});
    }
    if (spec.limit_rows && spec.max_rows > 0 &&
        lines.size() > static_cast<std::size_t>(spec.max_rows)) {
        lines.resize(static_cast<std::size_t>(spec.max_rows));
        lines.back().wrapped = false;
        if (spec.ellipsis) {
            std::vector<std::uint32_t> dots = {0x2026};
            std::vector<Glyph> ellipsis;
            shape(ellipsis, dots, 0, 1, *face, fallback.get(), ppu, letter_spacing);
            std::vector<Glyph>& last = lines.back().glyphs;
            if (limit > 0) {
                auto fits = [&] {
                    std::vector<Glyph> with = last;
                    with.insert(with.end(), ellipsis.begin(), ellipsis.end());
                    return width_of(with) <= limit;
                };
                while (!last.empty() && !fits()) last.pop_back();
                while (!last.empty() && last.back().space) last.pop_back();
            }
            last.insert(last.end(), ellipsis.begin(), ellipsis.end());
        }
    }
    for (Line& line : lines) {
        lay_out(line, spec.block_align && line.wrapped && limit > 0 ? limit : 0);
    }

    // Metrics in canvas units, rounded as FreeType rounds them: the ascender up, the
    // descender down (it is negative), the line height to the nearest.
    const FT_Size_Metrics& metrics = face->face->size->metrics;
    float ascent = std::ceil(static_cast<float>(metrics.ascender) / 64 / ppu);
    float descent = std::floor(static_cast<float>(metrics.descender) / 64 / ppu);
    float line_height = std::round(static_cast<float>(metrics.height) / 64 / ppu);
    if (line_height <= 0) line_height = ascent - descent;
    line_height += spec.spacing[1];

    // Where each line's baseline starts from the object's origin (y up): vertically the block
    // from the first ascender to the last baseline is centred on the origin, or the first
    // ascender sits on it (top), or the last descender (bottom); horizontally the lines are
    // aligned within the widest and the block is centred on the origin, or starts (left) or
    // ends (right) there.
    auto count = static_cast<float>(lines.size());
    float first = spec.vertical_align == "top"      ? -ascent
                  : spec.vertical_align == "bottom" ? -descent + (count - 1) * line_height
                                                    : -(ascent - (count - 1) * line_height) / 2;
    float block_min = 0, block_max = 0, widest = 0;
    for (const Line& line : lines) {
        block_min = std::min(block_min, line.min_x);
        block_max = std::max(block_max, line.min_x + line.width);
        widest = std::max(widest, line.width);
    }
    float block_width = block_max - block_min;
    float shift = spec.horizontal_align == "left"    ? block_width / 2
                  : spec.horizontal_align == "right" ? -block_width / 2
                                                     : 0;
    std::vector<std::array<float, 2>> origins;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const Line& line = lines[l];
        float offset = spec.horizontal_align == "left"    ? 0
                       : spec.horizontal_align == "right" ? widest - line.width
                                                          : (widest - line.width) / 2;
        origins.push_back({shift - block_width / 2 - line.min_x + offset,
                           first - static_cast<float>(l) * line_height});
    }

    // The lines' bounds: the ink across, and from the first ascender down to the last
    // descender, or to where the next line's ascender would start if that is lower. The block
    // is those bounds plus the padding, centred on them.
    float low_x = INFINITY, low_y = INFINITY, high_x = -INFINITY, high_y = -INFINITY;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const Line& line = lines[l];
        const auto& o = origins[l];
        low_x = std::min(low_x, o[0] + line.min_x);
        high_x = std::max(high_x, o[0] + line.min_x + line.width);
        low_y = std::min({low_y, o[1] + descent, o[1] + ascent - line_height});
        high_y = std::max(high_y, o[1] + ascent);
    }
    if (lines.empty()) low_x = low_y = high_x = high_y = 0;
    float pad_x = std::clamp(spec.padding[0], 0.0f, 512.0f);
    float pad_y = std::clamp(spec.padding[1], 0.0f, 512.0f);
    bitmap.size = {high_x - low_x + 2 * pad_x, high_y - low_y + 2 * pad_y};
    bitmap.center = {(low_x + high_x) / 2, (low_y + high_y) / 2};
    auto width =
        static_cast<std::uint32_t>(std::clamp<float>(std::ceil(bitmap.size[0] * ppu), 1, max_side));
    auto height =
        static_cast<std::uint32_t>(std::clamp<float>(std::ceil(bitmap.size[1] * ppu), 1, max_side));
    bitmap.width = width;
    bitmap.height = height;
    bitmap.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    if (spec.opaque_background) {
        for (std::size_t i = 0; i < bitmap.rgba.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                bitmap.rgba[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(
                    std::clamp(spec.background[static_cast<std::size_t>(c)], 0.0f, 1.0f) * 255);
            }
            bitmap.rgba[i + 3] = 255;
        }
    }

    auto cr = static_cast<std::uint8_t>(std::clamp(spec.color[0], 0.0f, 1.0f) * 255);
    auto cg = static_cast<std::uint8_t>(std::clamp(spec.color[1], 0.0f, 1.0f) * 255);
    auto cb = static_cast<std::uint8_t>(std::clamp(spec.color[2], 0.0f, 1.0f) * 255);
    float alpha = std::clamp(spec.alpha, 0.0f, 1.0f);
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const auto& o = origins[l];
        // The bitmap's pixel of a canvas point: the bounds' low corner lands at the padding.
        float base_x = (o[0] - low_x + pad_x) * ppu;
        float base_y = (high_y + pad_y - o[1]) * ppu;  // the baseline, rows top down
        for (const Glyph& g : lines[l].glyphs) {
            FT_Face ft = g.face->face;
            if (FT_Load_Glyph(ft, g.index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) continue;
            FT_Bitmap& bm = ft->glyph->bitmap;
            int left = static_cast<int>(std::lround(base_x + (g.x + g.x_offset) * ppu)) +
                       ft->glyph->bitmap_left;
            int top =
                static_cast<int>(std::lround(base_y - g.y_offset * ppu)) - ft->glyph->bitmap_top;
            for (unsigned row = 0; row < bm.rows; ++row) {
                int y = top + static_cast<int>(row);
                if (y < 0 || y >= static_cast<int>(height)) continue;
                const unsigned char* src = bm.buffer + row * static_cast<unsigned>(bm.pitch);
                for (unsigned col = 0; col < bm.width; ++col) {
                    int x = left + static_cast<int>(col);
                    if (x < 0 || x >= static_cast<int>(width)) continue;
                    float coverage = bm.pixel_mode == FT_PIXEL_MODE_MONO
                                         ? (((src[col / 8] >> (7 - col % 8)) & 1) ? 1.0f : 0.0f)
                                         : static_cast<float>(src[col]) / 255.0f;
                    float a = coverage * alpha;
                    if (a <= 0) continue;
                    std::uint8_t* px =
                        bitmap.rgba.data() +
                        (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
                    // Straight-alpha "over" onto what is there.
                    float da = static_cast<float>(px[3]) / 255.0f;
                    float oa = a + da * (1 - a);
                    auto mix = [&](std::uint8_t d, std::uint8_t s) {
                        float v = oa > 0 ? (static_cast<float>(s) * a +
                                            static_cast<float>(d) * da * (1 - a)) /
                                               oa
                                         : 0;
                        return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
                    };
                    px[0] = mix(px[0], cr);
                    px[1] = mix(px[1], cg);
                    px[2] = mix(px[2], cb);
                    px[3] = static_cast<std::uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f));
                }
            }
        }
    }
    return bitmap;
}

}  // namespace glint::scene

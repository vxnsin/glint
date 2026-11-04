#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/pictures.hpp"

// The interface every wallpaper kind implements (image, video, scene, later web).
// One instance per output; the daemon owns it and calls these from the main thread.
// Loading runs on workers; until it is done the previous wallpaper keeps drawing.
namespace glint {

class Workers;

namespace gpu {
class Context;
class StagingRing;
struct Target;
}  // namespace gpu

namespace image {
class Textures;
}

enum class Kind { image, video, scene, web };
std::string_view kind_name(Kind kind);

// What "set" passes in "options" (docs/IPC.md).
struct Options {
    gpu::Scaling scaling = gpu::Scaling::fill;
    std::string color = "#000000";    // as given, for status and the state file
    float linear[3] = {0, 0, 0};      // the same colour, linear
    std::string transition = "fade";  // "fade" or "none"
    int duration_ms = 300;
};

// Parses "fill", "fit", ...; false if unknown.
bool parse_scaling(std::string_view text, gpu::Scaling& out);
std::string_view scaling_name(gpu::Scaling scaling);
// Parses "#rrggbb" into a linear colour; false if malformed.
bool parse_color(std::string_view text, float linear[3]);

// The daemon-wide things a wallpaper draws and loads with.
struct Services {
    gpu::Context& gpu;
    gpu::Pictures& pictures;
    gpu::StagingRing& staging;
    Workers& workers;
    image::Textures& textures;
};

struct OutputInfo {
    std::string name;     // connector name, e.g. "DP-1"
    std::uint32_t width;  // in pixels, after fractional scale
    std::uint32_t height;
    double scale;
    double refresh_hz;
};

struct Cursor {
    double x;  // in output pixels
    double y;
    std::uint32_t buttons;  // bit 0 left, bit 1 right, bit 2 middle
};

// One entry of what "props" lists over IPC. "value" and the bounds are JSON text, so this
// header stays free of a JSON library; the daemon parses them where it needs to.
struct Property {
    std::string name;
    std::string type;  // bool, slider, combo, color, text, textinput, file, directory, scenetexture
    std::string text;  // label
    std::string value;  // JSON
    std::string def;    // JSON, the project's default
    std::string extra;  // JSON: min/max/step, options[], condition
    bool visible = true;
    int order = 0;
};

struct Frame {
    double time;   // seconds since the wallpaper started, pauses excluded
    double delta;  // seconds since the last rendered frame
    std::uint64_t index;
};

class Wallpaper {
public:
    virtual ~Wallpaper() = default;

    virtual Kind kind() const = 0;
    virtual const std::filesystem::path& path() const = 0;
    virtual const Options& options() const = 0;

    // Starts loading. done is called once on the main thread, with an error message or empty;
    // it is not called if the wallpaper is destroyed first.
    virtual void load(std::function<void(std::string error)> done) = 0;
    virtual bool ready() const = 0;

    virtual void resize(const OutputInfo&) {}

    // Draws into the target, which is already cleared to the background colour, at an
    // opacity (below 1 during a crossfade). Returns false when nothing was drawn.
    virtual bool render(const Frame& frame, const gpu::Target& target, float opacity) = 0;

    // Whether the wallpaper wants another frame after this one (false = idle).
    virtual bool wants_frames() const = 0;

    virtual void pause() {}
    virtual void resume() {}

    // GPU memory it holds, in bytes; shared textures count fully for every user.
    virtual std::uint64_t memory() const { return 0; }

    // Later milestones: scene settings, cursor and audio input.
    virtual std::vector<Property> properties() const { return {}; }
    // Returns the names that changed; a shader recompile, if one is needed, runs in the
    // background and the old pipeline keeps drawing until it is ready.
    virtual std::vector<std::string> set_property(std::string_view, std::string_view) { return {}; }
    virtual bool wants_cursor() const { return false; }
    virtual void cursor(const Cursor&) {}
    // 64 bands per channel, as Wallpaper Engine's g_AudioSpectrum64*. Only scenes use it.
    virtual void audio_spectrum(const float*, const float*) {}
    virtual bool wants_audio() const { return false; }
};

// The checks make_wallpaper does, without making one: error message or empty.
std::string check_path(const std::filesystem::path& path);

// Picks the kind from the path: a file is decoded as a picture (video comes in M4), a
// directory with project.json is a scene (M2). Returns null with an error otherwise.
std::unique_ptr<Wallpaper> make_wallpaper(const std::filesystem::path& path, const Options& options,
                                          Services& services, std::string& error);

}  // namespace glint

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The interface every wallpaper kind implements (image, video, scene, later web).
// One instance per output; the renderer owns it and calls these from the main thread.
// Loading happens on a worker; until ready() the previous wallpaper keeps drawing.
namespace glint {

enum class Kind { image, video, scene, web };

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

    // Called once on a worker thread. Returns an error message or empty.
    virtual std::string load(const std::filesystem::path& path, const OutputInfo& output) = 0;
    virtual bool ready() const = 0;

    virtual void resize(const OutputInfo& output) = 0;

    // Draw into the output's current image. Returns false when nothing changed and the
    // present can be skipped (still pictures, paused videos, idle scenes).
    virtual bool render(const Frame& frame) = 0;

    // Whether the wallpaper wants a frame callback after this one (false = idle).
    virtual bool wants_frames() const = 0;

    virtual void pause() = 0;
    virtual void resume() = 0;

    virtual std::vector<Property> properties() const = 0;
    // Returns the names that changed; a shader recompile, if one is needed, runs in the
    // background and the old pipeline keeps drawing until it is ready.
    virtual std::vector<std::string> set_property(std::string_view name, std::string_view json) = 0;

    virtual void cursor(const Cursor& cursor) = 0;

    // 64 bands per channel, as Wallpaper Engine's g_AudioSpectrum64*. Only scenes use it.
    virtual void audio_spectrum(const float* left, const float* right) = 0;
    virtual bool wants_audio() const = 0;
};

std::unique_ptr<Wallpaper> make_wallpaper(const std::filesystem::path& path);

}  // namespace glint

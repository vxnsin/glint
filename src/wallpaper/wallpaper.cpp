#include "wallpaper/wallpaper.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <system_error>

#include "wallpaper/image/image.hpp"
#include "wallpaper/scene/scene_wallpaper.hpp"
#include "wallpaper/video/video.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace glint {

std::string_view kind_name(Kind kind) {
    switch (kind) {
        case Kind::image: return "image";
        case Kind::video: return "video";
        case Kind::scene: return "scene";
        case Kind::web: return "web";
    }
    return "?";
}

namespace {

constexpr std::pair<gpu::Scaling, std::string_view> scalings[] = {
    {gpu::Scaling::fill, "fill"},       {gpu::Scaling::fit, "fit"},
    {gpu::Scaling::center, "center"},   {gpu::Scaling::tile, "tile"},
    {gpu::Scaling::stretch, "stretch"},
};

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool parse_scaling(std::string_view text, gpu::Scaling& out) {
    for (auto [value, name] : scalings) {
        if (name == text) {
            out = value;
            return true;
        }
    }
    return false;
}

std::string_view scaling_name(gpu::Scaling scaling) {
    for (auto [value, name] : scalings) {
        if (value == scaling) return name;
    }
    return "?";
}

bool parse_color(std::string_view text, float linear[3]) {
    if (text.size() != 7 || text[0] != '#') return false;
    for (int i = 0; i < 3; ++i) {
        int hi = hex(text[static_cast<std::size_t>(1 + i * 2)]);
        int lo = hex(text[static_cast<std::size_t>(2 + i * 2)]);
        if (hi < 0 || lo < 0) return false;
        linear[i] = srgb_to_linear(static_cast<float>(hi * 16 + lo) / 255.0f);
    }
    return true;
}

// A file is a video when FFmpeg finds a video stream that is not a still picture format.
bool is_video(const std::filesystem::path& path) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) return false;
    std::string name = format->iformat != nullptr ? format->iformat->name : "";
    bool still = name.find("image2") != std::string::npos || name.ends_with("_pipe");
    bool video = false;
    if (!still && avformat_find_stream_info(format, nullptr) >= 0) {
        int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        video = stream >= 0 && format->duration > AV_TIME_BASE / 2;
    }
    avformat_close_input(&format);
    return video;
}

std::string check_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) return "no such file";
    if (std::filesystem::is_directory(status)) {
        auto project = path / "project.json";
        if (!std::filesystem::exists(project, ec)) {
            return "not a picture or a Wallpaper Engine project";
        }
        std::ifstream in(project);
        auto json = nlohmann::json::parse(in, nullptr, false);
        std::string type = json.is_object() ? json.value("type", "") : "";
        std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
        if (type == "scene") return {};
        if (type == "video") {
            std::string file = json.value("file", "");
            if (file.empty() || !std::filesystem::exists(path / file, ec)) {
                return "the video item has no video file";
            }
            return {};
        }
        if (type == "web") return "web wallpapers come with milestone M6";
        return "a Wallpaper Engine " + (type.empty() ? std::string("item") : type) +
               " glint does not show";
    }
    if (::access(path.c_str(), R_OK) != 0) return "not readable";
    return {};
}

std::unique_ptr<Wallpaper> make_wallpaper(const std::filesystem::path& path, const Options& options,
                                          Services& services, std::string& error) {
    error = check_path(path);
    if (!error.empty()) return nullptr;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        std::ifstream in(path / "project.json");
        auto json = nlohmann::json::parse(in, nullptr, false);
        std::string type = json.is_object() ? json.value("type", "") : "";
        std::ranges::transform(type, type.begin(), [](unsigned char c) { return std::tolower(c); });
        if (type == "video") {
            return std::make_unique<video::Video>(path / json.value("file", ""), options, services);
        }
        return std::make_unique<scene::Scene>(path, options, services);
    }
    if (is_video(path)) return std::make_unique<video::Video>(path, options, services);
    return std::make_unique<image::Image>(path, options, services);
}

}  // namespace glint

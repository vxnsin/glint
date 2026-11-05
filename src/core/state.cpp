#include "core/state.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <fstream>
#include <system_error>

#include "core/log.hpp"
#include "core/paths.hpp"

namespace glint::state {

namespace {

constexpr int state_version = 1;

nlohmann::json entry_json(const Entry& entry) {
    return {{"path", entry.path.string()}, {"options", options_json(entry.options)}};
}

std::optional<Entry> entry_from(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("path") || !json["path"].is_string()) {
        return std::nullopt;
    }
    Entry entry;
    entry.path = json["path"].get<std::string>();
    if (json.contains("options")) {
        if (std::string error = parse_options(json["options"], entry.options); !error.empty()) {
            log::warn("state file: {}: {}", entry.path.string(), error);
        }
    }
    return entry;
}

}  // namespace

// One file per Wayland display: a nested compositor's daemon keeps its own assignments.
// The first display keeps the old name, so nothing is lost on an update.
std::filesystem::path file() {
    std::string display = paths::display_name();
    if (display == "wayland-0" || display == "wayland-1" || display.empty()) {
        return paths::state_dir() / "state.json";
    }
    return paths::state_dir() / ("state-" + display + ".json");
}

nlohmann::json options_json(const Options& options) {
    nlohmann::json json = {
        {"scaling", scaling_name(options.scaling)},
        {"color", options.color},
        {"transition", options.transition},
        {"duration", options.duration_ms},
    };
    if (options.fps > 0) json["fps"] = options.fps;
    if (options.volume > 0) json["volume"] = options.volume;
    if (options.speed != 1) json["speed"] = options.speed;
    if (options.hwdec != "auto") json["hwdec"] = options.hwdec;
    if (options.mute) json["mute"] = true;
    if (options.properties.is_object() && !options.properties.empty()) {
        json["properties"] = options.properties;
    }
    return json;
}

std::string parse_options(const nlohmann::json& json, Options& options) {
    if (json.is_null()) return {};
    if (!json.is_object()) return "options must be an object";
    for (const auto& [key, value] : json.items()) {
        if (key == "scaling") {
            if (!value.is_string() || !parse_scaling(value.get<std::string>(), options.scaling)) {
                return "scaling is one of fill, fit, center, tile, stretch";
            }
        } else if (key == "color") {
            if (!value.is_string() || !parse_color(value.get<std::string>(), options.linear)) {
                return "color is #rrggbb";
            }
            options.color = value.get<std::string>();
        } else if (key == "transition") {
            if (!value.is_string() || (value != "fade" && value != "none")) {
                return "transition is fade or none";
            }
            options.transition = value.get<std::string>();
        } else if (key == "duration") {
            if (!value.is_number_integer() || value.get<int>() < 0 || value.get<int>() > 10000) {
                return "duration is milliseconds, 0 to 10000";
            }
            options.duration_ms = value.get<int>();
        } else if (key == "properties") {
            if (!value.is_object()) return "properties is an object";
            options.properties = value;
        } else if (key == "volume") {
            if (!value.is_number() || value.get<double>() < 0 || value.get<double>() > 1) {
                return "volume is 0 to 1";
            }
            options.volume = value.get<float>();
        } else if (key == "mute") {
            if (value.is_boolean()) {
                options.mute = value.get<bool>();
            } else if (value == "on" || value == "off") {
                options.mute = value == "on";
            } else {
                return "mute is on or off";
            }
        } else if (key == "speed") {
            if (!value.is_number() || value.get<double>() <= 0 || value.get<double>() > 16) {
                return "speed is a number above 0, up to 16";
            }
            options.speed = value.get<float>();
        } else if (key == "hwdec") {
            if (!value.is_string() || (value != "auto" && value != "vaapi" && value != "no")) {
                return "hwdec is auto, vaapi or no";
            }
            options.hwdec = value.get<std::string>();
        } else if (key == "loop") {
            // Videos loop; accepted for the protocol.
        } else if (key == "fps") {
            // For every kind; a still picture has no use for it.
            if (!value.is_number() || value.get<double>() < 0 || value.get<double>() > 1000) {
                return "fps is a number, 0 to 1000";
            }
            options.fps = static_cast<int>(std::lround(value.get<double>()));
        } else {
            return "unknown option " + key;
        }
    }
    return {};
}

State load() {
    State state;
    std::ifstream in(file());
    if (!in) return state;
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        log::warn("state file {} is not valid JSON, ignored", file().string());
        return state;
    }
    if (json.contains("all")) state.all = entry_from(json["all"]);
    if (json.contains("properties") && json["properties"].is_object()) {
        for (const auto& [path, values] : json["properties"].items()) {
            if (values.is_object()) state.properties[path] = values;
        }
    }
    if (json.contains("outputs") && json["outputs"].is_object()) {
        for (const auto& [name, value] : json["outputs"].items()) {
            if (auto entry = entry_from(value)) state.outputs[name] = *entry;
        }
    }
    return state;
}

bool save(const State& state) {
    nlohmann::json json = {{"version", state_version}, {"outputs", nlohmann::json::object()}};
    if (state.all) json["all"] = entry_json(*state.all);
    for (const auto& [name, entry] : state.outputs) json["outputs"][name] = entry_json(entry);
    if (!state.properties.empty()) {
        json["properties"] = nlohmann::json::object();
        for (const auto& [path, values] : state.properties) json["properties"][path] = values;
    }

    std::error_code ec;
    std::filesystem::create_directories(paths::state_dir(), ec);
    auto path = file();
    auto tmp = path;
    tmp += ".tmp";
    std::string text = json.dump(2) + "\n";
    // Written, synced, then renamed over the old file: a crash leaves one or the other.
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < text.size()) {
        ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

}  // namespace glint::state

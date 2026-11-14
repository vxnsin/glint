#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "core/fd.hpp"

// Hyprland's sockets. The cursor, wherever it is: the layer surface only sees the pointer
// while no window is over it, Hyprland's request socket always knows. Asked once per drawn
// frame while a wallpaper wants the cursor; nothing runs otherwise. And which outputs show a
// fullscreen window on a visible workspace, which wlr-foreign-toplevel cannot say.
namespace glint::input {

class HyprlandCursor {
public:
    HyprlandCursor();  // finds the sockets from HYPRLAND_INSTANCE_SIGNATURE

    bool available() const { return !socket_.empty(); }
    // Global layout coordinates (logical pixels), or nothing when the request failed.
    std::optional<std::pair<double, double>> position() const;
    // Connector names of the outputs whose active (or shown special) workspace holds a
    // fullscreen window, or nothing when the request failed.
    std::optional<std::set<std::string>> fullscreen_outputs() const;
    // A non-blocking connection to the event socket (socket2), or an invalid fd.
    UniqueFd events() const;

private:
    std::optional<std::string> request(std::string_view command) const;

    std::filesystem::path socket_;
    std::filesystem::path events_;
};

}  // namespace glint::input

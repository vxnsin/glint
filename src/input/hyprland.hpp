#pragma once

#include <filesystem>
#include <optional>
#include <utility>

// The cursor on Hyprland, wherever it is: the layer surface only sees the pointer while no
// window is over it, Hyprland's request socket always knows. Asked once per drawn frame while
// a wallpaper wants the cursor; nothing runs otherwise.
namespace glint::input {

class HyprlandCursor {
public:
    HyprlandCursor();  // finds the socket from HYPRLAND_INSTANCE_SIGNATURE

    bool available() const { return !socket_.empty(); }
    // Global layout coordinates (logical pixels), or nothing when the request failed.
    std::optional<std::pair<double, double>> position() const;

private:
    std::filesystem::path socket_;
};

}  // namespace glint::input

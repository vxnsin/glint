#pragma once

#include <filesystem>

// XDG base directories, resolved once per call from the environment.
namespace glint::paths {

std::filesystem::path runtime_dir();  // $XDG_RUNTIME_DIR, else /tmp/glint-$UID
std::filesystem::path state_dir();    // $XDG_STATE_HOME/glint
std::filesystem::path cache_dir();    // $XDG_CACHE_HOME/glint

// $GLINT_SOCKET, else $XDG_RUNTIME_DIR/glint/$WAYLAND_DISPLAY.sock
std::filesystem::path socket_path();

}  // namespace glint::paths

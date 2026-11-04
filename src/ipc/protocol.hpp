#pragma once

#include <array>
#include <string_view>

// Names shared by the daemon and the CLI. The protocol itself is docs/IPC.md; this header
// only keeps the strings in one place so the two sides cannot drift.
namespace glint::ipc {

// Socket: $XDG_RUNTIME_DIR/glint/$WAYLAND_DISPLAY.sock, overridden by $GLINT_SOCKET.
inline constexpr std::string_view socket_dir = "glint";
inline constexpr std::string_view socket_env = "GLINT_SOCKET";

// Operations a client may send ("op").
inline constexpr std::array<std::string_view, 13> ops = {
    "outputs", "set",    "unset",  "status",   "props",   "pause",     "resume",
    "audio",   "cursor", "settings", "inspect", "subscribe", "version",
};

// Event names ("event") sent to subscribers.
inline constexpr std::array<std::string_view, 4> events = {"state", "props", "outputs", "error"};

// Pause reasons; they stack per output.
inline constexpr std::array<std::string_view, 4> pause_reasons = {"covered", "locked", "request",
                                                                  "idle"};

// Wallpaper states as reported in "outputs" and "status".
inline constexpr std::array<std::string_view, 5> states = {"none", "loading", "running", "paused",
                                                           "error"};

}  // namespace glint::ipc

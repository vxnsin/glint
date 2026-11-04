#pragma once

#include <format>
#include <string_view>
#include <utility>

// Minimal logging: one line per message on stderr, a level prefix, no dependencies.
// Journald picks stderr up when glint runs from a systemd user unit or Hyprland.
namespace glint::log {

enum class Level { debug, info, warn, error };

void set_level(Level level);
void write(Level level, std::string_view message);

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace glint::log

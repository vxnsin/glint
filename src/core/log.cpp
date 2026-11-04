#include "core/log.hpp"

#include <cstdio>

namespace glint::log {

namespace {

Level current = Level::info;

const char* prefix(Level level) {
    switch (level) {
        case Level::debug: return "debug";
        case Level::info: return "info";
        case Level::warn: return "warn";
        case Level::error: return "error";
    }
    return "?";
}

}  // namespace

void set_level(Level level) { current = level; }

void write(Level level, std::string_view message) {
    if (level < current) return;
    std::fprintf(stderr, "glint: %s: %.*s\n", prefix(level),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace glint::log

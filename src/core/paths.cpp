#include "core/paths.hpp"

#include <unistd.h>

#include <cstdlib>
#include <string>

#include "ipc/protocol.hpp"

namespace glint::paths {

namespace {

std::filesystem::path env_or(const char* name, const std::filesystem::path& fallback) {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] == '/') return value;
    return fallback;
}

std::filesystem::path home() {
    const char* value = std::getenv("HOME");
    return value != nullptr ? value : "/";
}

}  // namespace

std::filesystem::path runtime_dir() {
    return env_or("XDG_RUNTIME_DIR", "/tmp/glint-" + std::to_string(::getuid()));
}

std::filesystem::path state_dir() {
    return env_or("XDG_STATE_HOME", home() / ".local/state") / "glint";
}

std::filesystem::path cache_dir() {
    return env_or("XDG_CACHE_HOME", home() / ".cache") / "glint";
}

std::string display_name() {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    std::string name = display != nullptr && display[0] != '\0' ? display : "wayland-0";
    // WAYLAND_DISPLAY may be an absolute path; keep only the last component.
    return std::filesystem::path(name).filename().string();
}

std::filesystem::path socket_path() {
    const char* override_path = std::getenv(std::string(ipc::socket_env).c_str());
    if (override_path != nullptr && override_path[0] != '\0') return override_path;
    return runtime_dir() / ipc::socket_dir / (display_name() + ".sock");
}

}  // namespace glint::paths

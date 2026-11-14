#include "input/hyprland.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

namespace glint::input {

HyprlandCursor::HyprlandCursor() {
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (signature == nullptr || runtime == nullptr || *signature == '\0') return;
    std::filesystem::path path =
        std::filesystem::path(runtime) / "hypr" / signature / ".socket.sock";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) socket_ = path;
    path.replace_filename(".socket2.sock");
    if (std::filesystem::exists(path, ec)) events_ = path;
}

namespace {

UniqueFd connect_to(const std::filesystem::path& socket, int flags) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::string path = socket.string();
    if (path.empty() || path.size() >= sizeof(address.sun_path)) return {};
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | flags, 0));
    if (!fd || ::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return {};
    }
    return fd;
}

}  // namespace

std::optional<std::string> HyprlandCursor::request(std::string_view command) const {
    UniqueFd fd = connect_to(socket_, 0);
    if (!fd) return std::nullopt;
    timeval timeout{0, 200'000};  // a hung compositor must not hang the daemon
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (::write(fd.get(), command.data(), command.size()) !=
        static_cast<ssize_t>(command.size())) {
        return std::nullopt;
    }
    std::string reply;
    char chunk[16384];
    for (;;) {
        ssize_t n = ::read(fd.get(), chunk, sizeof(chunk));
        if (n < 0) return std::nullopt;
        if (n == 0) break;
        reply.append(chunk, static_cast<std::size_t>(n));
    }
    return reply;
}

std::optional<std::set<std::string>> HyprlandCursor::fullscreen_outputs() const {
    auto monitors_text = request("j/monitors");
    auto clients_text = request("j/clients");
    if (!monitors_text || !clients_text) return std::nullopt;
    auto monitors = nlohmann::json::parse(*monitors_text, nullptr, false);
    auto clients = nlohmann::json::parse(*clients_text, nullptr, false);
    if (!monitors.is_array() || !clients.is_array()) return std::nullopt;
    // Workspaces by name: special workspaces have no id on every Hyprland version.
    auto name_of = [](const nlohmann::json& object, const char* key) {
        const auto it = object.find(key);
        if (it == object.end() || !it->is_object()) return std::string();
        return it->value("name", std::string());
    };
    std::set<std::string> result;
    for (const auto& monitor : monitors) {
        if (!monitor.is_object()) continue;
        std::string active = name_of(monitor, "activeWorkspace");
        std::string special = name_of(monitor, "specialWorkspace");
        for (const auto& client : clients) {
            if (!client.is_object()) continue;
            // 2 is real fullscreen; 1 is maximised, which leaves gaps and bars in view.
            if (client.value("fullscreen", 0) < 2 || !client.value("mapped", true) ||
                client.value("hidden", false)) {
                continue;
            }
            std::string workspace = name_of(client, "workspace");
            if (workspace.empty()) continue;
            if (workspace == active || (!special.empty() && workspace == special)) {
                result.insert(monitor.value("name", std::string()));
            }
        }
    }
    return result;
}

UniqueFd HyprlandCursor::events() const { return connect_to(events_, SOCK_NONBLOCK); }

std::optional<std::pair<double, double>> HyprlandCursor::position() const {
    UniqueFd fd = connect_to(socket_, 0);
    if (!fd) return std::nullopt;
    const char request[] = "cursorpos";
    if (::write(fd.get(), request, sizeof(request) - 1) != sizeof(request) - 1) {
        return std::nullopt;
    }
    char reply[64] = {};
    ssize_t n = ::read(fd.get(), reply, sizeof(reply) - 1);
    if (n <= 0) return std::nullopt;
    // "1943, 452"
    char* end = nullptr;
    double x = std::strtod(reply, &end);
    if (end == reply || *end != ',') return std::nullopt;
    double y = std::strtod(end + 1, nullptr);
    return std::make_pair(x, y);
}

}  // namespace glint::input

#include "input/hyprland.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "core/fd.hpp"

namespace glint::input {

HyprlandCursor::HyprlandCursor() {
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (signature == nullptr || runtime == nullptr || *signature == '\0') return;
    std::filesystem::path path =
        std::filesystem::path(runtime) / "hypr" / signature / ".socket.sock";
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) socket_ = path;
}

std::optional<std::pair<double, double>> HyprlandCursor::position() const {
    if (socket_.empty()) return std::nullopt;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::string path = socket_.string();
    if (path.size() >= sizeof(address.sun_path)) return std::nullopt;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd || ::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return std::nullopt;
    }
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

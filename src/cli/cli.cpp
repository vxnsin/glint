#include "cli/cli.hpp"

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "core/fd.hpp"
#include "core/paths.hpp"

namespace glint::cli {

namespace {

using nlohmann::json;

int usage_error(const char* message) {
    std::fprintf(stderr, "glint: %s (glint --help)\n", message);
    return 2;
}

UniqueFd connect_daemon() {
    std::string path = paths::socket_path().string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return {};
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "glint: no daemon on %s (start it with: glint)\n", path.c_str());
        return {};
    }
    return fd;
}

bool send_line(int fd, const json& message) {
    std::string line = message.dump() + "\n";
    std::size_t done = 0;
    while (done < line.size()) {
        ssize_t n = ::write(fd, line.data() + done, line.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads one line; false on EOF.
bool read_line(int fd, std::string& buffer, std::string& line) {
    for (;;) {
        if (auto end = buffer.find('\n'); end != std::string::npos) {
            line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            return true;
        }
        char chunk[4096];
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
}

std::string number(const json& value, int decimals) {
    if (!value.is_number()) return "-";
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value.get<double>());
    return text;
}

void print_outputs(const json& list) {
    for (const auto& o : list) {
        const auto& w = o["wallpaper"];
        std::printf("%s  %dx%d  scale %s  %s Hz  at %d,%d  %s", o.value("name", "?").c_str(),
                    o.value("width", 0), o.value("height", 0), number(o["scale"], 2).c_str(),
                    number(o["refresh"], 2).c_str(), o.value("x", 0), o.value("y", 0),
                    w.value("state", "?").c_str());
        if (w.contains("reason")) std::printf(" (%s)", w["reason"].get<std::string>().c_str());
        if (w.contains("path")) std::printf("  %s", w["path"].get<std::string>().c_str());
        std::printf("\n");
    }
}

void print_status(const std::string& name, const json& s) {
    std::printf("%s: %s", name.c_str(), s.value("state", "?").c_str());
    if (s["paused_reasons"].is_array() && !s["paused_reasons"].empty()) {
        std::string reasons;
        for (const auto& r : s["paused_reasons"]) {
            if (!reasons.empty()) reasons += ", ";
            reasons += r.get<std::string>();
        }
        std::printf(" (%s)", reasons.c_str());
    }
    std::printf("\n");
    if (s["path"].is_string()) {
        std::printf("  %s %s\n", s.value("kind", "?").c_str(),
                    s["path"].get<std::string>().c_str());
    }
    std::printf("  %ux%u, %s fps now, %llu frames, last frame %s ms CPU, %s ms GPU\n",
                s.value("width", 0u), s.value("height", 0u), number(s["fps"], 0).c_str(),
                static_cast<unsigned long long>(s.value("frames", 0ull)),
                number(s["frame_ms"], 2).c_str(), number(s["gpu_ms"], 3).c_str());
    std::printf("  load %s ms, %s MB on the GPU\n", number(s["load_ms"], 1).c_str(),
                number(s["memory_mb"], 1).c_str());
    if (s.contains("message"))
        std::printf("  error: %s\n", s["message"].get<std::string>().c_str());
}

void print_text(const std::string& op, const json& result) {
    if (op == "outputs") {
        print_outputs(result);
    } else if (op == "status") {
        if (result.contains("state")) {
            print_status("output", result);
        } else {
            for (const auto& [name, s] : result.items()) print_status(name, s);
        }
    } else if (op == "set") {
        std::printf("%s, %s ms\n", result.value("kind", "?").c_str(),
                    number(result["load_ms"], 1).c_str());
    } else if (op == "props" && result.is_array()) {
        for (const auto& p : result) {
            if (p.value("type", "") == "text" || p.value("type", "") == "group") continue;
            std::printf("%-16s %-8s %-28s %s%s\n", p.value("name", "?").c_str(),
                        p.value("type", "?").c_str(), p["value"].dump().c_str(),
                        p.value("text", "").substr(0, 40).c_str(),
                        p.value("visible", true) ? "" : "  (hidden)");
        }
    } else if (op == "screenshot") {
        std::printf("%s, %dx%d\n", result.value("path", "?").c_str(), result.value("width", 0),
                    result.value("height", 0));
    } else if (op == "version") {
        std::printf("glint %s (protocol %d), Vulkan %s on %s\n",
                    result.value("version", "?").c_str(), result.value("protocol", 0),
                    result.value("vulkan", "?").c_str(), result.value("device", "?").c_str());
    } else if (!result.is_null()) {
        std::printf("%s\n", result.dump(2).c_str());
    }
}

}  // namespace

int run(int argc, char** argv) {
    std::vector<std::string_view> args;
    bool as_json = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--json") {
            as_json = true;
        } else {
            args.push_back(arg);
        }
    }
    if (args.empty()) return usage_error("no command");
    std::string op(args[0]);
    json request = {{"id", 1}, {"op", op}};

    if (op == "outputs" || op == "version") {
        if (args.size() != 1) return usage_error("too many arguments");
    } else if (op == "status") {
        if (args.size() > 2) return usage_error("status takes one output at most");
        if (args.size() == 2) request["output"] = std::string(args[1]);
    } else if (op == "set") {
        if (args.size() < 3) return usage_error("set <output|*> <path> [key=value ...]");
        request["output"] = std::string(args[1]);
        std::error_code ec;
        std::string target(args[2]);
        // A bare Workshop id names a folder in Steam's Workshop for Wallpaper Engine.
        bool id = !target.empty() &&
                  std::ranges::all_of(target, [](char c) { return c >= '0' && c <= '9'; });
        if (id && !std::filesystem::exists(target, ec)) {
            const char* home = std::getenv("HOME");
            const char* data = std::getenv("XDG_DATA_HOME");
            std::filesystem::path base =
                data != nullptr && data[0] == '/'
                    ? std::filesystem::path(data)
                    : std::filesystem::path(home != nullptr ? home : "/") / ".local/share";
            target = (base / "Steam/steamapps/workshop/content/431960" / target).string();
        }
        request["path"] = std::filesystem::absolute(target, ec).lexically_normal().string();
        json options = json::object();
        for (std::size_t i = 3; i < args.size(); ++i) {
            auto eq = args[i].find('=');
            if (eq == std::string_view::npos) return usage_error("options are key=value");
            std::string key(args[i].substr(0, eq));
            std::string value(args[i].substr(eq + 1));
            if (key == "duration" || key == "fps") {
                try {
                    options[key] = std::stoi(value);
                } catch (...) {
                    return usage_error("duration and fps are numbers");
                }
            } else {
                options[key] = value;
            }
        }
        if (!options.empty()) request["options"] = options;
    } else if (op == "unset") {
        if (args.size() != 2) return usage_error("unset <output|*>");
        request["output"] = std::string(args[1]);
    } else if (op == "pause" || op == "resume") {
        if (args.size() > 3) return usage_error("pause|resume [output] [reason]");
        request["output"] = args.size() >= 2 ? std::string(args[1]) : "*";
        if (args.size() == 3) request["reason"] = std::string(args[2]);
    } else if (op == "screenshot") {
        if (args.size() != 3) return usage_error("screenshot <output> <file.png>");
        request["output"] = std::string(args[1]);
        std::error_code ec;
        request["path"] = std::filesystem::absolute(std::string(args[2]), ec).string();
    } else if (op == "subscribe") {
        json events = json::array();
        for (std::size_t i = 1; i < args.size(); ++i) events.push_back(std::string(args[i]));
        if (!events.empty()) request["events"] = events;
    } else if (op == "props") {
        // props <output>                 list
        // props <output> reset
        // props <output> name value ...  set (value as JSON, else a string)
        if (args.size() < 2) return usage_error("props <output> [reset | name value ...]");
        request["output"] = std::string(args[1]);
        if (args.size() == 3 && args[2] == "reset") {
            request["reset"] = true;
        } else if (args.size() > 2) {
            if ((args.size() - 2) % 2 != 0) return usage_error("props: name value pairs");
            json set = json::object();
            for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
                std::string text(args[i + 1]);
                if (text == "on") text = "true";
                if (text == "off") text = "false";
                json value = json::parse(text, nullptr, false);
                set[std::string(args[i])] = value.is_discarded() ? json(text) : value;
            }
            request["set"] = set;
        }
    } else if (op == "audio" || op == "cursor" || op == "settings" || op == "inspect") {
        // Sent as is; the daemon says which milestone brings it.
    } else {
        std::fprintf(stderr, "glint: unknown command '%s'\n", op.c_str());
        return 2;
    }

    UniqueFd fd = connect_daemon();
    if (!fd) return 1;
    if (!send_line(fd.get(), request)) {
        std::fprintf(stderr, "glint: the daemon closed the connection\n");
        return 1;
    }

    std::string buffer, line;
    // The reply first.
    for (;;) {
        if (!read_line(fd.get(), buffer, line)) {
            std::fprintf(stderr, "glint: the daemon closed the connection\n");
            return 1;
        }
        json reply = json::parse(line, nullptr, false);
        if (reply.is_discarded() || reply.contains("event")) continue;
        if (!reply.value("ok", false)) {
            std::fprintf(stderr, "glint: %s\n", reply.value("error", "failed").c_str());
            return 1;
        }
        if (as_json) {
            std::printf("%s\n", reply["result"].dump(2).c_str());
        } else if (op != "subscribe") {
            print_text(op, reply["result"]);
        }
        break;
    }
    if (op != "subscribe") return 0;

    // Then events, one JSON line each, until the daemon goes away or we are interrupted.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    while (read_line(fd.get(), buffer, line)) std::printf("%s\n", line.c_str());
    return 0;
}

}  // namespace glint::cli

#include <cstdlib>
#include <fstream>
#include <mutex>

#include "cli/cli.hpp"
#include "cli/tui.hpp"

namespace glint::cli {
nlohmann::json request_daemon(const nlohmann::json& request, std::stop_token) {
    using nlohmann::json;
    static std::mutex mutex;
    const std::lock_guard guard(mutex);
    static json options = {
        {"fps", 60}, {"volume", 0.5}, {"scaling", "fit"}, {"transition", "fade"}};
    static bool clock = true;
    std::ofstream log(std::getenv("GLINT_TEST_REQUESTS"), std::ios::app);
    log << request.dump() << '\n';
    const auto op = request.value("op", "");
    if (op == "outputs")
        return {{"ok", true},
                {"result",
                 json::array({{{"name", "DP-2"},
                               {"width", 2560},
                               {"height", 1440},
                               {"wallpaper", {{"path", "/test/scene"}, {"state", "running"}}}},
                              {{"name", "HDMI-A-1"},
                               {"width", 1920},
                               {"height", 1080},
                               {"wallpaper", {{"path", "/other/scene"}, {"state", "running"}}}}})}};
    if (op == "status")
        return {{"ok", true},
                {"result", {{"path", "/test/scene"}, {"kind", "scene"}, {"options", options}}}};
    if (op == "props") {
        if (request.contains("set")) {
            clock = request["set"].value("clock", clock);
            return {{"ok", true}};
        }
        return {{"ok", true},
                {"result", json::array({{{"name", "clock"},
                                         {"text", "Show clock"},
                                         {"type", "bool"},
                                         {"value", clock}}})}};
    }
    if (op == "set") {
        options = request.value("options", options);
        return {{"ok", true}};
    }
    return {{"ok", true}};
}
}  // namespace glint::cli
int main() {
    return glint::cli::run_tui();
}

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
        {"DP-2", {{"fps", 60}, {"volume", 0.5}, {"scaling", "fit"}, {"transition", "fade"}}},
        {"HDMI-A-1", {{"fps", 30}, {"volume", 0.2}, {"scaling", "fill"}, {"transition", "none"}}}};
    static bool clock = true;
    static json defaults = options["DP-2"];
    std::ofstream log(std::getenv("GLINT_TEST_REQUESTS"), std::ios::app);
    log << request.dump() << '\n';
    const auto op = request.value("op", "");
    if (op == "settings") {
        if (request.contains("set")) {
            defaults.update(request["set"]);
            for (auto& settings : options) settings.update(defaults);
        }
        return {{"ok", true}, {"result", defaults}};
    }
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
    if (op == "status") {
        json states = {
            {"DP-2", {{"path", "/test/scene"}, {"kind", "scene"}, {"options", options["DP-2"]}}},
            {"HDMI-A-1",
             {{"path", "/other/scene"}, {"kind", "scene"}, {"options", options["HDMI-A-1"]}}}};
        const auto output = request.value("output", "*");
        return {{"ok", true}, {"result", output == "*" ? states : states[output]}};
    }
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
        if (request.contains("options")) {
            if (request.value("output", "*") == "*") {
                for (auto& settings : options) settings = request["options"];
            } else
                options[request["output"].get<std::string>()] = request["options"];
        }
        return {{"ok", true}};
    }
    return {{"ok", true}};
}
}  // namespace glint::cli
int main() {
    return glint::cli::run_tui();
}

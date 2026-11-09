#include "cli/configure.hpp"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "configure: %s\n", what);
        }
    };
    json settings = json::array({
        {{"name", "clock"}, {"text", "Clock"}, {"type", "bool"}, {"value", true}},
        {{"name", "mode"},
         {"text", "Mode"},
         {"type", "combo"},
         {"value", "a"},
         {"options", json::array({{{"label", "First"}, {"value", "a"}},
                                  {{"label", "Second"}, {"value", "b"}}})}},
        {{"name", "speed"},
         {"text", "Speed"},
         {"type", "slider"},
         {"value", 1},
         {"min", 0},
         {"max", 3}},
        {{"name", "hidden"},
         {"text", "Hidden setting"},
         {"type", "bool"},
         {"value", false},
         {"visible", false}},
        {{"name", "dependent"},
         {"text", "Dependent setting"},
         {"type", "bool"},
         {"value", false},
         {"visible", false}},
    });
    std::vector<json> changes;
    auto exchange = [&](const json& request) -> json {
        if (request.value("output", "") != "DP-1") return {{"ok", false}, {"error", "output"}};
        if (!request.contains("set")) return {{"ok", true}, {"result", settings}};
        changes.push_back(request["set"]);
        for (auto& item : settings) {
            const auto name = item.value("name", "");
            if (request["set"].contains(name)) item["value"] = request["set"][name];
            if (name == "dependent" && request["set"].contains("clock"))
                item["visible"] = !request["set"]["clock"].get<bool>();
        }
        return {{"ok", true}, {"result", json::object()}};
    };
    std::istringstream in("1\n2\n2\n3\n2.5\nq\n");
    std::ostringstream out;
    check(glint::cli::configure("DP-1", in, out, exchange) == 0, "menu failed");
    check(changes.size() == 3, "expected three changes");
    if (changes.size() == 3) {
        check(changes[0]["clock"] == false, "boolean toggle");
        check(changes[1]["mode"] == "b", "combo choice");
        check(changes[2]["speed"] == 2.5, "slider value");
    }
    check(out.str().find("Hidden setting") == std::string::npos, "hidden setting displayed");
    check(out.str().find("Dependent setting") != std::string::npos, "dependent setting missing");
    check(out.str().find("Second") != std::string::npos, "combo label missing");

    std::istringstream bad_in("3\nnot-a-number\nq\n");
    std::ostringstream bad_out;
    changes.clear();
    check(glint::cli::configure("DP-1", bad_in, bad_out, exchange) == 0, "invalid input quit");
    check(changes.empty(), "invalid slider was sent");

    std::istringstream error_in;
    std::ostringstream error_out;
    auto error = [](const json&) { return json{{"ok", false}, {"error", "no such output"}}; };
    check(glint::cli::configure("DP-1", error_in, error_out, error) == 1, "daemon error exit");
    check(error_out.str().find("no such output") != std::string::npos, "daemon error hidden");
    const json library = json::array({
        {{"title", "Lake"}, {"type", "scene"}, {"path", "/lake"}},
        {{"title", "Web"}, {"type", "web"}, {"path", "/web"}},
        {{"title", "Sky"}, {"type", "video"}, {"path", "/sky"}},
    });
    json selected;
    auto menu_exchange = [&](const json& request) -> json {
        if (request.value("op", "") == "outputs")
            return {{"ok", true},
                    {"result", json::array({{{"name", "DP-1"}}, {{"name", "DP-2"}}})}};
        selected = request;
        return {{"ok", true}, {"result", json::object()}};
    };
    std::istringstream menu_in("0\n2\n3\n");
    std::ostringstream menu_out;
    check(glint::cli::choose_wallpaper(library, menu_in, menu_out, menu_exchange) == 0,
          "wallpaper menu failed");
    check(selected.value("path", "") == "/sky" && selected.value("output", "") == "DP-2",
          "wallpaper menu selected wrong wallpaper or output");
    check(menu_out.str().find("Web") == std::string::npos, "unsupported wallpaper listed");
    std::istringstream cancel_in("q\n");
    std::ostringstream cancel_out;
    selected = nullptr;
    check(glint::cli::choose_wallpaper(library, cancel_in, cancel_out, menu_exchange) == 0 &&
              selected.is_null(),
          "wallpaper menu cancel");
    std::printf("configure regressions: %d failures\n", failures);
    return failures != 0;
}

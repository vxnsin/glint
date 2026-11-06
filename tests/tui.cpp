#include "cli/tui.hpp"

#include <cstdio>
#include <stdexcept>

using nlohmann::json;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "tui: %s\n", label);
        }
    };
    json status = {
        {"path", "/test/scene"},
        {"options",
         {{"fps", 60}, {"volume", 0.5}, {"scaling", "fit"}, {"properties", {{"clock", true}}}}}};
    auto request = glint::cli::tui_setting_request("DP-2", status, "fps", 144);
    check(request["output"] == "DP-2" && request["options"]["fps"] == 144, "fps target");
    check(request["options"]["volume"] == 0.5 && request["options"]["scaling"] == "fit" &&
              request["options"]["properties"]["clock"] == true,
          "setting update lost other options");
    check(status["options"]["fps"] == 60, "setting update mutated snapshot");
    const json states = {{"DP-2", status},
                         {"HDMI-A-1",
                          {{"path", "/different/video"},
                           {"options", {{"fps", 30}, {"volume", 0.2}, {"speed", 1.5}}}}},
                         {"DP-3", {{"path", nullptr}}}};
    const auto batch = glint::cli::tui_setting_requests("*", states, "fps", 120);
    check(batch.size() == 2, "all-display settings include inactive displays");
    for (const auto& item : batch) {
        const std::string name = item["output"];
        check(item["path"] == states[name]["path"],
              "all-display edit replaced a different wallpaper");
        check(item["options"]["fps"] == 120 &&
                  item["options"]["volume"] == states[name]["options"]["volume"],
              "all-display edit lost individual options");
    }
    const auto image = glint::cli::kitty_image(std::string("\x00\xff\x00", 3), 1, 1, 1, 1, 42);
    check(image.find("q=2,C=1,i=42,s=1,v=1,c=1,r=1,m=0;AP8A") != std::string::npos,
          "Kitty image encoding");
    const auto chunked = glint::cli::kitty_image(std::string(6000, 'x'), 20, 100, 10, 10, 42);
    check(chunked.find("m=1;") != std::string::npos &&
              chunked.find("\033_Gm=0;") != std::string::npos,
          "Kitty image chunking");
    try {
        glint::cli::tui_setting_request("*", status, "fps", 30);
        check(false, "all-display edit could overwrite different wallpapers");
    } catch (const std::invalid_argument&) {
    }
    try {
        glint::cli::tui_setting_request("DP-2", json::object(), "fps", 30);
        check(false, "missing wallpaper accepted");
    } catch (const std::invalid_argument&) {
    }
    check(glint::cli::workshop_item_id("2919051696") == "2919051696", "plain id");
    check(
        glint::cli::workshop_item_id(
            "https://steamcommunity.com/sharedfiles/filedetails/?id=2919051696&searchtext=arona") ==
            "2919051696",
        "workshop link");
    check(
        glint::cli::workshop_item_id(
            "https://steamcommunity.com/workshop/filedetails/?foo=1&id=2919051696") == "2919051696",
        "second query parameter");
    for (const auto* bad : {"", "0", "00123", "123 +quit", "../../etc", "https://evil.test/?id=123",
                            "https://steamcommunity.com/sharedfiles/filedetails/?someid=123"})
        check(glint::cli::workshop_item_id(bad).empty(), "invalid Workshop input accepted");
    std::printf("tui regressions: %d failures\n", failures);
    return failures != 0;
}

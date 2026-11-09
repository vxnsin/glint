#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "cli/tui.hpp"

namespace glint::cli {
using nlohmann::json;

std::string kitty_image(const std::string& rgb, int width, int height, int columns, int rows,
                        unsigned id) {
    if (width <= 0 || height <= 0 || columns <= 0 || rows <= 0 ||
        rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
        throw std::invalid_argument("Invalid preview dimensions");
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((rgb.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < rgb.size(); i += 3) {
        unsigned bits = static_cast<unsigned char>(rgb[i]);
        bits = (bits << 8) | (i + 1 < rgb.size() ? static_cast<unsigned char>(rgb[i + 1]) : 0u);
        bits = (bits << 8) | (i + 2 < rgb.size() ? static_cast<unsigned char>(rgb[i + 2]) : 0u);
        encoded += alphabet[(bits >> 18) & 63];
        encoded += alphabet[(bits >> 12) & 63];
        encoded += i + 1 < rgb.size() ? alphabet[(bits >> 6) & 63] : '=';
        encoded += i + 2 < rgb.size() ? alphabet[bits & 63] : '=';
    }
    std::string out;
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4096) {
        out += "\033_G";
        if (offset == 0)
            out += "a=T,f=24,t=d,q=2,C=1,i=" + std::to_string(id) + ",s=" + std::to_string(width) +
                   ",v=" + std::to_string(height) + ",c=" + std::to_string(columns) +
                   ",r=" + std::to_string(rows) + ",";
        out += offset + 4096 < encoded.size() ? "m=1;" : "m=0;";
        out.append(encoded, offset, std::min<std::size_t>(4096, encoded.size() - offset));
        out += "\033\\";
    }
    return out;
}

json tui_setting_request(const std::string& output, const json& status, const std::string& name,
                         const json& value) {
    if (output.empty() || output == "*" || !status.is_object() || !status.contains("path") ||
        !status["path"].is_string())
        throw std::invalid_argument("Select a display with a running wallpaper first");
    json options = status.value("options", json::object());
    if (!options.is_object()) throw std::invalid_argument("Invalid wallpaper options");
    static constexpr std::string_view names[] = {
        "fps", "scaling", "transition", "duration", "volume", "mute", "color", "speed", "hwdec"};
    if (std::ranges::find(names, name) == std::end(names))
        throw std::invalid_argument("Unknown display setting");
    options[name] = value;
    return {{"op", "set"}, {"output", output}, {"path", status["path"]}, {"options", options}};
}

json tui_setting_requests(const std::string& output, const json& statuses, const std::string& name,
                          const json& value) {
    if (!statuses.is_object() || output.empty())
        throw std::invalid_argument("Invalid display state");
    json requests = json::array();
    for (const auto& [display, status] : statuses.items()) {
        if (output != "*" && output != display) continue;
        if (!status.is_object() || !status.contains("path") || !status["path"].is_string())
            continue;
        requests.push_back(tui_setting_request(display, status, name, value));
    }
    if (requests.empty())
        throw std::invalid_argument("No running wallpapers on the selected displays");
    return requests;
}

std::string workshop_item_id(const std::string& text) {
    std::string_view id = text;
    if (id.starts_with("https://steamcommunity.com/sharedfiles/filedetails/") ||
        id.starts_with("https://steamcommunity.com/workshop/filedetails/")) {
        auto query = id.find('?');
        if (query == std::string_view::npos) return {};
        id.remove_prefix(query + 1);
        while (!id.empty() && !id.starts_with("id=")) {
            auto next = id.find('&');
            if (next == std::string_view::npos) return {};
            id.remove_prefix(next + 1);
        }
        if (!id.starts_with("id=")) return {};
        id.remove_prefix(3);
        id = id.substr(0, id.find_first_of("&#"));
    }
    if (id.empty() || id.size() > 20 || id.front() == '0' ||
        !std::ranges::all_of(id, [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    return std::string(id);
}
}  // namespace glint::cli

#include "cli/configure.hpp"

#include <algorithm>
#include <charconv>
#include <istream>
#include <optional>
#include <ostream>
#include <string_view>
#include <vector>

namespace glint::cli {
namespace {
using nlohmann::json;

std::optional<std::size_t> selection(const std::string& text, std::size_t max) {
    std::size_t number = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc{} || end != text.data() + text.size() || number == 0 || number > max)
        return std::nullopt;
    return number - 1;
}

std::string display(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "on" : "off";
    return value.dump();
}

bool editable(const json& p) {
    if (!p.is_object() || !p.value("visible", true)) return false;
    const auto type = p.value("type", "");
    return type == "bool" || type == "slider" || type == "combo" || type == "color" ||
           type == "textinput" || type == "file" || type == "directory" || type == "scenetexture";
}

std::string label(const json& p) {
    const auto name = p.value("name", "?");
    const auto text = p.value("text", "");
    return text.empty() ? name : text;
}

std::string current(const json& p) {
    const json value = p.value("value", json());
    if (p.contains("options") && p["options"].is_array()) {
        for (const auto& option : p["options"]) {
            if (option.is_object() && option.contains("value") &&
                (option["value"] == value || display(option["value"]) == display(value)))
                return option.value("label", display(value));
        }
    }
    return display(value);
}

bool reply_ok(const json& reply, std::ostream& out) {
    if (reply.is_object() && reply.value("ok", false)) return true;
    out << "Error: "
        << (reply.is_object() ? reply.value("error", "invalid reply") : "invalid reply") << '\n';
    return false;
}
}  // namespace

int configure(const std::string& output, std::istream& in, std::ostream& out,
              const Exchange& exchange) {
    for (;;) {
        json reply = exchange({{"op", "props"}, {"output", output}});
        if (!reply_ok(reply, out)) return 1;
        if (!reply.contains("result") || !reply["result"].is_array()) {
            out << "Error: invalid settings list\n";
            return 1;
        }
        std::vector<json> properties;
        for (const auto& p : reply["result"])
            if (editable(p)) properties.push_back(p);
        out << "\nWallpaper settings on " << output << ":\n";
        for (std::size_t i = 0; i < properties.size(); ++i)
            out << "  " << i + 1 << ". " << label(properties[i]) << " [" << current(properties[i])
                << "]\n";
        if (properties.empty()) out << "  No editable settings.\n";
        out << "Select a number, or q to quit: " << std::flush;
        std::string line;
        if (!std::getline(in, line) || line == "q" || line == "Q") return 0;
        auto index = selection(line, properties.size());
        if (!index) {
            out << "Enter a number from the list.\n";
            continue;
        }
        const json& p = properties[*index];
        const auto type = p.value("type", "");
        json value;
        if (type == "bool") {
            if (!p.contains("value") || !p["value"].is_boolean()) {
                out << "Error: invalid boolean setting\n";
                continue;
            }
            value = !p["value"].get<bool>();
        } else if (type == "combo" && p.contains("options") && p["options"].is_array() &&
                   !p["options"].empty()) {
            const auto& options = p["options"];
            out << label(p) << ":\n";
            for (std::size_t i = 0; i < options.size(); ++i) {
                const auto& option = options[i];
                if (!option.is_object() || !option.contains("value")) continue;
                out << "  " << i + 1 << ". " << option.value("label", display(option["value"]))
                    << '\n';
            }
            out << "Select a number, or Enter to cancel: " << std::flush;
            if (!std::getline(in, line)) return 0;
            if (line.empty()) continue;
            auto choice = selection(line, options.size());
            if (!choice || !options[*choice].is_object() || !options[*choice].contains("value")) {
                out << "Invalid choice.\n";
                continue;
            }
            value = options[*choice]["value"];
        } else {
            out << label(p);
            if (type == "slider" && p.contains("min") && p.contains("max"))
                out << " (" << display(p["min"]) << " to " << display(p["max"]) << ")";
            if (type == "color") out << " (#rrggbb)";
            if (type == "textinput") out << " (\"\" to clear)";
            out << ": ";
            out << std::flush;
            if (!std::getline(in, line)) return 0;
            if (line.empty()) continue;
            if (type == "slider") {
                value = json::parse(line, nullptr, false);
                if (value.is_discarded() || !value.is_number()) {
                    out << "Enter a number.\n";
                    continue;
                }
            } else {
                value = type == "textinput" && line == "\"\"" ? "" : line;
            }
        }
        reply = exchange(
            {{"op", "props"}, {"output", output}, {"set", {{p.value("name", ""), value}}}});
        if (reply_ok(reply, out)) out << "Updated " << label(p) << ".\n";
    }
}

}  // namespace glint::cli

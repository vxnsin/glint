#include "cli/library.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/scene.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace glint::cli {
namespace {
using nlohmann::json;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

json summary(const scene::Project& p, const std::filesystem::path& folder) {
    return {{"id", p.workshop_id.empty() ? folder.filename().string() : p.workshop_id},
            {"title", p.title},
            {"type", p.type},
            {"rating", p.rating},
            {"tags", p.tags},
            {"path", folder.string()},
            {"file", p.file},
            {"preview", p.preview},
            {"audio", p.audio},
            {"property_count", p.properties.size()}};
}

bool read_project(const std::filesystem::path& folder, scene::Project& p, std::string& error) {
    scene::Vfs vfs(folder, {});
    auto file = vfs.read("project.json");
    if (!file) {
        error = "cannot read " + (folder / "project.json").string();
        return false;
    }
    error = scene::parse_project(file->text(), p);
    return error.empty();
}
}  // namespace

std::filesystem::path workshop_path() {
    if (const char* root = std::getenv("GLINT_WORKSHOP"); root && root[0]) return root;
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const auto base = data && data[0] == '/'
                          ? std::filesystem::path(data)
                          : std::filesystem::path(home ? home : "/") / ".local/share";
    return base / "Steam/steamapps/workshop/content/431960";
}

std::filesystem::path resolve_wallpaper(std::string_view target) {
    std::filesystem::path path(target);
    std::error_code ec;
    const bool id =
        !target.empty() && std::ranges::all_of(target, [](char c) { return c >= '0' && c <= '9'; });
    if (id && !std::filesystem::exists(path, ec)) path = workshop_path() / path;
    auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

json list_library(const std::filesystem::path& workshop, std::string_view search,
                  std::string& error) {
    error.clear();
    json items = json::array();
    std::error_code ec;
    std::filesystem::directory_iterator entries(workshop, ec);
    if (ec) {
        error = "cannot read Workshop folder " + workshop.string() + ": " + ec.message();
        return items;
    }
    const std::string query = lower(search);
    for (auto it = entries; it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) {
            ec.clear();
            continue;
        }
        scene::Project p;
        std::string ignored;
        if (!read_project(it->path(), p, ignored)) continue;
        auto item = summary(p, it->path());
        const auto text =
            lower(p.title + " " + item["id"].get<std::string>() + " " + json(p.tags).dump());
        if (!query.empty() && text.find(query) == std::string::npos) continue;
        items.push_back(std::move(item));
    }
    if (ec) error = "reading Workshop folder: " + ec.message();
    std::sort(items.begin(), items.end(), [](const json& a, const json& b) {
        const auto ta = lower(a["title"].get<std::string>());
        const auto tb = lower(b["title"].get<std::string>());
        return ta == tb ? a["id"].get<std::string>() < b["id"].get<std::string>() : ta < tb;
    });
    return items;
}

json inspect_wallpaper(const std::filesystem::path& folder, std::string& error) {
    scene::Project project;
    if (!read_project(folder, project, error)) return {};
    auto result = summary(project, folder);
    result["properties"] = json::array();
    const auto values = scene::default_values(project);
    for (const auto& p : project.properties) {
        json prop = {{"name", p.name},
                     {"type", p.type},
                     {"text", p.text},
                     {"value", p.value},
                     {"condition", p.condition},
                     {"visible", scene::evaluate_condition(p.condition, values)}};
        if (!p.min.is_null()) prop["min"] = p.min;
        if (!p.max.is_null()) prop["max"] = p.max;
        prop["options"] = json::array();
        for (const auto& [label, value] : p.options)
            prop["options"].push_back({{"label", label}, {"value", value}});
        result["properties"].push_back(std::move(prop));
    }
    scene::Vfs vfs(folder, scene::default_assets());
    error = vfs.open_package();
    if (!error.empty()) return {};
    result["entry_exists"] = vfs.exists(project.file);
    if (project.type != "scene") return result;
    scene::SceneDesc description;
    error = scene::load_scene(vfs, project.file, description);
    if (!error.empty()) return {};
    result["scene"] = {{"width", description.general.ortho[0]},
                       {"height", description.general.ortho[1]},
                       {"images", description.images.size()},
                       {"particles", description.particles.size()},
                       {"texts", description.texts.size()},
                       {"sounds", description.sounds.size()},
                       {"groups", description.groups.size()},
                       {"skipped_objects", description.skipped},
                       {"perspective", description.general.perspective}};
    result["warnings"] = description.warnings;
    return result;
}
}  // namespace glint::cli

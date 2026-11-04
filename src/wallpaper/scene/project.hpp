#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// project.json of a Wallpaper Engine item (docs/FORMATS.md, 1).
namespace glint::scene {

struct ProjectProperty {
    std::string name;
    std::string type;      // bool, slider, combo, color, text, textinput, file, directory,
                           // scenetexture, group; "text" also when the file has no type
    std::string text;      // label, localised when the project has a localization block
    nlohmann::json value;  // default as in the file; colours stay "r g b"
    nlohmann::json min, max, step, precision;
    std::vector<std::pair<std::string, std::string>> options;  // combo: label, value
    std::string condition;
    int order = 0;
};

struct Project {
    std::string title;
    std::string type;  // lower case: scene, video, web, application, preset
    std::string file;
    std::string preview;
    std::string rating;  // contentrating
    std::string workshop_id;
    std::vector<std::string> tags;
    bool audio = false;                       // general.supportsaudioprocessing
    std::vector<ProjectProperty> properties;  // in "order"

    const ProjectProperty* property(std::string_view name) const;
};

// Returns an error message or empty. language picks the localization block ("en-us").
std::string parse_project(std::string_view text, Project& out, std::string_view language = "en-us");

// The value table conditions and bindings read: name -> current value.
using Values = std::map<std::string, nlohmann::json, std::less<>>;
Values default_values(const Project& project);

// Evaluates a property condition such as "season.value == 'winter' && snow.value".
// Supported: name.value, numbers, 'strings', "strings", true, false, ==, !=, ===, !==, <, <=,
// >, >=, !, &&, ||, parentheses. Comparisons are loose like JavaScript's ==, so "1" == 1.
// An expression it cannot read is true (the entry stays visible) and error is set.
bool evaluate_condition(std::string_view expression, const Values& values,
                        std::string* error = nullptr);

// "r g b" floats to "#rrggbb" and back.
std::string color_to_hex(std::string_view rgb);
std::optional<std::string> hex_to_color(std::string_view hex);

}  // namespace glint::scene

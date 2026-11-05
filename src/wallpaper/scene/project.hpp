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
// A value a script wrote over a bound field (SceneScript, docs/FORMATS.md 11), and the
// playback state of the field's timeline. One per bound field and per output; the scene
// description itself is shared between outputs and stays as parsed.
struct ScriptCell {
    nlohmann::json value;
    bool set = false;
    // The timeline's IAnimation state: playing at a rate from a start time, or holding a
    // frame while paused or stopped.
    bool playing = true;
    double rate = 1;
    double start = 0;  // the scene time the timeline was (re)started at
    double held = 0;   // the frame it holds while not playing
    bool held_set = false;
};

// The property values by name, and the cells scripts write per output (null when none).
struct Values : std::map<std::string, nlohmann::json, std::less<>> {
    using map::map;
    const std::vector<ScriptCell>* cells = nullptr;
    const ScriptCell* cell(int key) const {
        if (cells == nullptr || key < 0 || static_cast<std::size_t>(key) >= cells->size()) {
            return nullptr;
        }
        return &(*cells)[static_cast<std::size_t>(key)];
    }
};
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

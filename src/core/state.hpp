#pragma once

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "wallpaper/wallpaper.hpp"

// What is on which output, kept in $XDG_STATE_HOME/glint/state.json so the daemon restores it
// on the next start. Written whole and atomically after every change.
namespace glint::state {

struct Entry {
    std::filesystem::path path;
    Options options;
};

struct State {
    std::optional<Entry> all;              // "*"
    std::map<std::string, Entry> outputs;  // by connector name
};

std::filesystem::path file();
State load();  // empty on a missing or unreadable file
bool save(const State& state);

// Options as JSON, the same keys as "options" in docs/IPC.md.
nlohmann::json options_json(const Options& options);
// Fills options from JSON; returns an error message or empty. Unknown keys are errors.
std::string parse_options(const nlohmann::json& json, Options& options);

}  // namespace glint::state

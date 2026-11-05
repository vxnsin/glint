#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Window rules: what glint does to an output because of the windows on it. The built-in
// rule is "a fullscreen window with focus pauses the output" (that is `covered` in
// docs/IPC.md); users add rules to pause, mute or stop for other windows, or to exempt
// windows from the built-in rule, as linux-wallpaperengine's --fullscreen-pause-ignore-appid
// and mpvpaper's auto-pause modes do.
//
// Rules match on what wlr-foreign-toplevel reports: app_id (the X11 class for Xwayland
// windows), title, and the states fullscreen, maximized and activated. Patterns are globs
// with `*` and `?`, matched without regard to case. The first rule that matches a window
// decides for that window; an output gets the union of what its windows ask for. Minimized
// windows never count.
//
// This is plain data and logic with no Wayland in it. The daemon fills Window from its
// toplevels and reads the rules from the settings (JSON, in the daemon).
namespace glint::policy {

enum class Action {
    pause,   // no frames on the output while the window is there
    mute,    // sounds off, frames go on
    stop,    // pause and free the wallpaper's memory; rebuilt when the window goes
    ignore,  // nothing, and the built-in fullscreen rule does not apply either
};

// A tri-state condition: unset means "either".
enum class Want : std::uint8_t { any, yes, no };

struct Rule {
    std::string app_id;  // glob, empty matches everything
    std::string title;   // glob, empty matches everything
    Want fullscreen = Want::any;
    Want maximized = Want::any;
    Want activated = Want::any;
    Action action = Action::pause;
};

struct Window {
    std::string app_id;
    std::string title;
    std::vector<std::string> outputs;  // connector names the window is on
    bool fullscreen = false;
    bool maximized = false;
    bool activated = false;
    bool minimized = false;
};

struct Verdict {
    bool pause = false;
    bool mute = false;
    bool stop = false;
    bool operator==(const Verdict&) const = default;
};

// `pattern` with `*` (any run) and `?` (one character) against `text`, case-insensitively
// for ASCII. Every other character matches itself.
bool glob_match(std::string_view pattern, std::string_view text);

class Rules {
public:
    // The built-in rule is on by default; off means only the user's rules apply.
    void set_builtin_fullscreen(bool on) { builtin_ = on; }
    bool builtin_fullscreen() const { return builtin_; }

    void set_rules(std::vector<Rule> rules) { rules_ = std::move(rules); }
    const std::vector<Rule>& rules() const { return rules_; }

    // What the first matching rule says for one window, or the built-in rule, or nothing
    // (a verdict with everything false).
    Verdict judge(const Window& window) const;

    // Verdicts for every output that has a window asking for something. Outputs not in the
    // result want nothing.
    std::map<std::string, Verdict> evaluate(const std::vector<Window>& windows) const;

private:
    bool builtin_ = true;
    std::vector<Rule> rules_;
};

std::string_view action_name(Action action);
bool action_from(std::string_view name, Action& action);

}  // namespace glint::policy

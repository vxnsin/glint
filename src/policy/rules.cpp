#include "policy/rules.hpp"

#include <cctype>

namespace glint::policy {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool want_ok(Want want, bool value) {
    switch (want) {
        case Want::any: return true;
        case Want::yes: return value;
        case Want::no: return !value;
    }
    return true;
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
    // Iterative matching with one backtrack point, the last `*` seen.
    std::size_t p = 0, t = 0;
    std::size_t star = std::string_view::npos, star_text = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_text = t;
        } else if (p < pattern.size() &&
                   (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++star_text;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

Verdict Rules::judge(const Window& window) const {
    Verdict verdict;
    if (window.minimized) return verdict;
    for (const Rule& rule : rules_) {
        if (!rule.app_id.empty() && !glob_match(rule.app_id, window.app_id)) continue;
        if (!rule.title.empty() && !glob_match(rule.title, window.title)) continue;
        if (!want_ok(rule.fullscreen, window.fullscreen)) continue;
        if (!want_ok(rule.maximized, window.maximized)) continue;
        if (!want_ok(rule.activated, window.activated)) continue;
        switch (rule.action) {
            case Action::pause: verdict.pause = true; break;
            case Action::mute: verdict.mute = true; break;
            case Action::stop: verdict.pause = verdict.stop = true; break;
            case Action::ignore: break;
        }
        return verdict;
    }
    if (builtin_ && window.fullscreen && window.activated) verdict.pause = true;
    return verdict;
}

std::map<std::string, Verdict> Rules::evaluate(const std::vector<Window>& windows) const {
    std::map<std::string, Verdict> result;
    for (const Window& window : windows) {
        const Verdict verdict = judge(window);
        if (verdict == Verdict{}) continue;
        for (const std::string& output : window.outputs) {
            Verdict& v = result[output];
            v.pause = v.pause || verdict.pause;
            v.mute = v.mute || verdict.mute;
            v.stop = v.stop || verdict.stop;
        }
    }
    return result;
}

std::string_view action_name(Action action) {
    switch (action) {
        case Action::pause: return "pause";
        case Action::mute: return "mute";
        case Action::stop: return "stop";
        case Action::ignore: return "ignore";
    }
    return "pause";
}

bool action_from(std::string_view name, Action& action) {
    for (Action a : {Action::pause, Action::mute, Action::stop, Action::ignore}) {
        if (action_name(a) == name) {
            action = a;
            return true;
        }
    }
    return false;
}

}  // namespace glint::policy

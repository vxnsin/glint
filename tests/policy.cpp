// Tests for the window rules and the power policy.

#include <cstdio>
#include <string>
#include <vector>

#include "core/transition.hpp"
#include "core/playback_clock.hpp"
#include "policy/power.hpp"
#include "policy/rules.hpp"

namespace {

using namespace glint::policy;

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++checks;                                                                         \
        if (!(cond)) {                                                                    \
            ++failures;                                                                   \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

constexpr std::int64_t s = 1'000'000'000;

void test_playback_clock() {
    glint::PlaybackClock clock;
    // Loading, including loading behind fullscreen, consumes no playback time.
    CHECK(clock.seconds(120 * s) == 0);
    clock.set_running(false, 120 * s);
    clock.set_running(true, 180 * s);
    CHECK(clock.seconds(182 * s) == 2);
    // Another output starting or stopping must not restart a shared clock.
    clock.set_running(true, 182 * s);
    CHECK(clock.seconds(185 * s) == 5);
    clock.set_running(false, 185 * s);
    CHECK(clock.seconds(785 * s) == 5);
    clock.set_running(false, 785 * s);
    clock.set_running(true, 800 * s);
    CHECK(clock.seconds(801 * s) == 6);
    clock.set_running(false, 802 * s);
    clock.set_running(true, 900 * s);
    CHECK(clock.seconds(903 * s) == 10);
}

void test_transition_clock() {
    glint::TransitionClock fade;
    // Loading and even repeated failed frame attempts must not advance a pending fade.
    CHECK(fade.opacity(2 * s, 300) == 0);
    CHECK(fade.opacity(20 * s, 300) == 0);
    CHECK(!fade.started());
    fade.submitted(20 * s);
    CHECK(fade.opacity(20 * s, 300) == 0);
    CHECK(fade.opacity(20 * s + 150'000'000, 300) == 0.5f);
    fade.submitted(20 * s + 150'000'000);  // later frames do not restart the fade
    CHECK(fade.opacity(20 * s + 300'000'000, 300) == 1);
    fade.resume(5 * s);  // covering/uncovering the output preserves the halfway state
    CHECK(fade.opacity(25 * s + 150'000'000, 300) == 0.5f);
    CHECK(fade.opacity(30 * s, 300) == 1);
    fade.reset();
    fade.resume(5 * s);  // a pending fade is still pending after a pause
    CHECK(fade.opacity(50 * s, 300) == 0);
    CHECK(fade.opacity(50 * s, 0) == 1);
    CHECK(fade.opacity(50 * s, -1) == 1);
    fade.submitted(50 * s);
    CHECK(fade.opacity(50 * s - 1, 300) == 0);
}

void test_glob() {
    CHECK(glob_match("", ""));
    CHECK(!glob_match("", "a"));
    CHECK(glob_match("*", ""));
    CHECK(glob_match("*", "anything"));
    CHECK(glob_match("firefox", "Firefox"));
    CHECK(!glob_match("firefox", "firefox-bin"));
    CHECK(glob_match("firefox*", "firefox-bin"));
    CHECK(glob_match("*fox*", "org.mozilla.firefox"));
    CHECK(glob_match("org.*.firefox", "org.mozilla.firefox"));
    CHECK(!glob_match("org.*.firefox", "org.mozilla.thunderbird"));
    CHECK(glob_match("steam_app_???", "steam_app_123"));
    CHECK(!glob_match("steam_app_???", "steam_app_1234"));
    CHECK(glob_match("steam_app_*", "steam_app_1234"));
    CHECK(glob_match("*.mp4", "a.b.mp4"));
    CHECK(!glob_match("*.mp4", "a.mp4.bak"));
    CHECK(glob_match("a*b*c", "axxbyyc"));
    CHECK(!glob_match("a*b*c", "axxbyy"));
    CHECK(glob_match("***", "x"));
}

Window window(std::string app_id, std::string title, std::vector<std::string> outputs,
              bool fullscreen = false, bool activated = false) {
    Window w;
    w.app_id = std::move(app_id);
    w.title = std::move(title);
    w.outputs = std::move(outputs);
    w.fullscreen = fullscreen;
    w.activated = activated;
    return w;
}

void test_builtin_rule() {
    Rules rules;
    // A fullscreen window with focus pauses its outputs; without focus it does not.
    CHECK(rules.judge(window("kitty", "", {"DP-2"}, true, true)).pause);
    CHECK(!rules.judge(window("kitty", "", {"DP-2"}, true, false)).pause);
    CHECK(!rules.judge(window("kitty", "", {"DP-2"}, false, true)).pause);
    // Minimized never counts, whatever else is set.
    Window minimized = window("kitty", "", {"DP-2"}, true, true);
    minimized.minimized = true;
    CHECK(rules.judge(minimized) == Verdict{});
    // Switched off.
    rules.set_builtin_fullscreen(false);
    CHECK(!rules.judge(window("kitty", "", {"DP-2"}, true, true)).pause);
}

void test_user_rules() {
    Rules rules;
    Rule ignore_mpv;
    ignore_mpv.app_id = "mpv";
    ignore_mpv.action = Action::ignore;
    Rule mute_discord;
    mute_discord.app_id = "discord";
    mute_discord.action = Action::mute;
    Rule stop_games;
    stop_games.app_id = "steam_app_*";
    stop_games.fullscreen = Want::yes;
    stop_games.action = Action::stop;
    Rule pause_maximized_video;
    pause_maximized_video.title = "*youtube*";
    pause_maximized_video.maximized = Want::yes;
    pause_maximized_video.action = Action::pause;
    rules.set_rules({ignore_mpv, mute_discord, stop_games, pause_maximized_video});

    // ignore beats the built-in rule.
    CHECK(rules.judge(window("mpv", "film", {"DP-2"}, true, true)) == Verdict{});
    // mute leaves frames on.
    {
        const Verdict v = rules.judge(window("discord", "call", {"DP-2"}));
        CHECK(v.mute && !v.pause && !v.stop);
    }
    // stop implies pause; the fullscreen condition must hold.
    {
        const Verdict v = rules.judge(window("steam_app_570", "Dota", {"DP-2"}, true, true));
        CHECK(v.stop && v.pause && !v.mute);
        const Verdict windowed = rules.judge(window("steam_app_570", "Dota", {"DP-2"}));
        CHECK(windowed == Verdict{});
    }
    // A title rule with a maximized condition.
    {
        Window w = window("firefox", "Cats - YouTube", {"HDMI-A-1"});
        CHECK(rules.judge(w) == Verdict{});
        w.maximized = true;
        CHECK(rules.judge(w).pause);
    }
    // An unmatched fullscreen window still hits the built-in rule.
    CHECK(rules.judge(window("kitty", "", {"DP-2"}, true, true)).pause);
    // First match wins: a second rule for the same app does not add to the first.
    Rule pause_discord;
    pause_discord.app_id = "discord";
    pause_discord.action = Action::pause;
    rules.set_rules({mute_discord, pause_discord});
    {
        const Verdict v = rules.judge(window("discord", "", {"DP-2"}));
        CHECK(v.mute && !v.pause);
    }
}

void test_evaluate_per_output() {
    Rules rules;
    Rule mute_discord;
    mute_discord.app_id = "discord";
    mute_discord.action = Action::mute;
    rules.set_rules({mute_discord});

    std::vector<Window> windows = {
        window("kitty", "", {"DP-2"}, true, true),        // pauses DP-2
        window("discord", "", {"DP-2", "HDMI-A-1"}),      // mutes both
        window("firefox", "", {"HDMI-A-1"}, true, false)  // fullscreen without focus
    };
    const auto result = rules.evaluate(windows);
    CHECK(result.size() == 2);
    CHECK(result.at("DP-2").pause && result.at("DP-2").mute && !result.at("DP-2").stop);
    CHECK(!result.at("HDMI-A-1").pause && result.at("HDMI-A-1").mute);
    CHECK(!result.contains("DP-3"));

    // A window on no output asks nothing of anyone.
    CHECK(rules.evaluate({window("kitty", "", {}, true, true)}).empty());
}

void test_actions() {
    Action action = Action::pause;
    CHECK(action_from("mute", action) && action == Action::mute);
    CHECK(action_from("ignore", action) && action == Action::ignore);
    CHECK(!action_from("sleep", action));
    CHECK(action_name(Action::stop) == "stop");
}

void test_battery() {
    Power power;
    PowerOptions o;
    o.battery_fps = 30;
    power.set_options(o);
    CHECK(power.evaluate("DP-2", 0) == PowerVerdict{});
    power.set_on_battery(true);
    {
        const PowerVerdict v = power.evaluate("DP-2", 0);
        CHECK(v.fps_cap == 30 && !v.pause_battery);
    }
    o.battery_pause = true;
    power.set_options(o);
    CHECK(power.evaluate("DP-2", 0).pause_battery);
    power.set_on_battery(false);
    CHECK(power.evaluate("DP-2", 0) == PowerVerdict{});
    // Battery alone sets no deadline: it changes only with UPower.
    CHECK(!power.next_deadline(0));
}

void test_idle() {
    Power power;
    PowerOptions o;
    o.idle_after_s = 300;
    power.set_options(o);
    // Without any input yet, nothing is idle and nothing is scheduled.
    CHECK(!power.evaluate("DP-2", 1000 * s).pause_idle);
    CHECK(!power.next_deadline(1000 * s));
    power.input(1000 * s);
    CHECK(!power.evaluate("DP-2", 1000 * s).pause_idle);
    CHECK(power.next_deadline(1000 * s) == 1300 * s);
    CHECK(!power.evaluate("DP-2", 1299 * s).pause_idle);
    CHECK(power.evaluate("DP-2", 1300 * s).pause_idle);
    CHECK(power.evaluate("HDMI-A-1", 2000 * s).pause_idle);  // every output
    // Due deadlines are not reported again.
    CHECK(!power.next_deadline(1300 * s));
    // Input lifts it and schedules the next one.
    power.input(2000 * s);
    CHECK(!power.evaluate("DP-2", 2000 * s).pause_idle);
    CHECK(power.next_deadline(2000 * s) == 2300 * s);
    // Off.
    o.idle_after_s = 0;
    power.set_options(o);
    CHECK(!power.evaluate("DP-2", 5000 * s).pause_idle);
    CHECK(!power.next_deadline(5000 * s));
}

void test_free_after_covered() {
    Power power;
    PowerOptions o;
    o.free_after_s = 600;
    power.set_options(o);
    power.set_covered("DP-2", true, 100 * s);
    CHECK(!power.evaluate("DP-2", 100 * s).free);
    CHECK(power.next_deadline(100 * s) == 700 * s);
    // A repeated covered event does not restart the clock.
    power.set_covered("DP-2", true, 400 * s);
    CHECK(power.next_deadline(400 * s) == 700 * s);
    CHECK(!power.evaluate("DP-2", 699 * s).free);
    CHECK(power.evaluate("DP-2", 700 * s).free);
    CHECK(!power.evaluate("HDMI-A-1", 700 * s).free);  // only the covered one
    CHECK(!power.next_deadline(700 * s));
    // Uncovered: back at once, and covered again later starts over.
    power.set_covered("DP-2", false, 800 * s);
    CHECK(!power.evaluate("DP-2", 800 * s).free);
    power.set_covered("DP-2", true, 900 * s);
    CHECK(power.next_deadline(900 * s) == 1500 * s);
    // Two outputs: the earlier deadline wins.
    power.set_covered("HDMI-A-1", true, 1000 * s);
    CHECK(power.next_deadline(1000 * s) == 1500 * s);
    power.output_removed("DP-2");
    CHECK(power.next_deadline(1000 * s) == 1600 * s);
    // Option off: covered outputs are never freed and set no deadline.
    o.free_after_s = 0;
    power.set_options(o);
    CHECK(!power.evaluate("HDMI-A-1", 5000 * s).free);
    CHECK(!power.next_deadline(1000 * s));
}

void test_combined_deadline() {
    Power power;
    PowerOptions o;
    o.idle_after_s = 300;
    o.free_after_s = 60;
    power.set_options(o);
    power.input(0);
    power.set_covered("DP-2", true, 10 * s);
    CHECK(power.next_deadline(10 * s) == 70 * s);   // free comes first
    CHECK(power.next_deadline(70 * s) == 300 * s);  // then idle
    const PowerVerdict v = power.evaluate("DP-2", 300 * s);
    CHECK(v.free && v.pause_idle);
}

}  // namespace

int main() {
    test_playback_clock();
    test_transition_clock();
    test_glob();
    test_builtin_rule();
    test_user_rules();
    test_evaluate_per_output();
    test_actions();
    test_battery();
    test_idle();
    test_free_after_covered();
    test_combined_deadline();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

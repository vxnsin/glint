#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// Power policy: what the battery, user idleness and long-covered outputs do to the frame
// rate and the pause reasons. Pure bookkeeping on a monotonic clock in nanoseconds; the
// daemon feeds it UPower's OnBattery, its own covered state per output and the time of the
// last input, and asks it when to wake up next, so nothing polls.
//
// - On battery: `battery_fps` caps the frame rate of every output (0: no cap), and
//   `battery_pause` pauses them all.
// - Idle: with `idle_after_s` set, every output pauses once no input arrived for that long,
//   and resumes with the next input (the `idle` reason in docs/IPC.md).
// - Free: with `free_after_s` set, an output covered for that long is to drop its
//   wallpaper's memory (decoders, textures, render targets); it is rebuilt from the caches
//   when the output is uncovered. This is mpvpaper's stop mode, for RAM and for GPUs that
//   keep a high clock while anything is resident.
namespace glint::policy {

struct PowerOptions {
    unsigned battery_fps = 0;  // frames per second on battery, 0 for the output's own
    bool battery_pause = false;
    unsigned idle_after_s = 0;  // pause after this long without input, 0 for never
    unsigned free_after_s = 0;  // free an output covered this long, 0 for never
};

struct PowerVerdict {
    unsigned fps_cap = 0;  // 0: none
    bool pause_battery = false;
    bool pause_idle = false;
    bool free = false;
    bool operator==(const PowerVerdict&) const = default;
};

class Power {
public:
    void set_options(const PowerOptions& options) { options_ = options; }
    const PowerOptions& options() const { return options_; }

    void set_on_battery(bool on) { on_battery_ = on; }
    bool on_battery() const { return on_battery_; }

    // The user did something at `now_ns`.
    void input(std::int64_t now_ns) { last_input_ns_ = now_ns; }

    // The daemon's covered state for an output changed at `now_ns`.
    void set_covered(std::string_view output, bool covered, std::int64_t now_ns);
    void output_removed(std::string_view output);

    PowerVerdict evaluate(std::string_view output, std::int64_t now_ns) const;

    // The next time a verdict changes by the clock alone (idle sets in, a covered output
    // becomes free), or nothing if none is pending. The daemon uses it as its poll timeout.
    std::optional<std::int64_t> next_deadline(std::int64_t now_ns) const;

private:
    static constexpr std::int64_t second_ns = 1'000'000'000;

    PowerOptions options_;
    bool on_battery_ = false;
    std::optional<std::int64_t> last_input_ns_;          // nothing until the first input
    std::map<std::string, std::int64_t> covered_since_;  // outputs covered right now
};

}  // namespace glint::policy

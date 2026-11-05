#include "policy/power.hpp"

#include <algorithm>

namespace glint::policy {

void Power::set_covered(std::string_view output, bool covered, std::int64_t now_ns) {
    const std::string name(output);
    if (covered) {
        covered_since_.try_emplace(name, now_ns);  // a repeat keeps the first time
    } else {
        covered_since_.erase(name);
    }
}

void Power::output_removed(std::string_view output) { covered_since_.erase(std::string(output)); }

PowerVerdict Power::evaluate(std::string_view output, std::int64_t now_ns) const {
    PowerVerdict verdict;
    if (on_battery_) {
        verdict.fps_cap = options_.battery_fps;
        verdict.pause_battery = options_.battery_pause;
    }
    if (options_.idle_after_s > 0 && last_input_ns_) {
        verdict.pause_idle = now_ns - *last_input_ns_ >= options_.idle_after_s * second_ns;
    }
    if (options_.free_after_s > 0) {
        const auto it = covered_since_.find(std::string(output));
        if (it != covered_since_.end()) {
            verdict.free = now_ns - it->second >= options_.free_after_s * second_ns;
        }
    }
    return verdict;
}

std::optional<std::int64_t> Power::next_deadline(std::int64_t now_ns) const {
    std::optional<std::int64_t> deadline;
    auto consider = [&](std::int64_t at) {
        if (at <= now_ns) return;  // already due; the caller evaluates now
        deadline = deadline ? std::min(*deadline, at) : at;
    };
    if (options_.idle_after_s > 0 && last_input_ns_) {
        consider(*last_input_ns_ + options_.idle_after_s * second_ns);
    }
    if (options_.free_after_s > 0) {
        for (const auto& [name, since] : covered_since_) {
            consider(since + options_.free_after_s * second_ns);
        }
    }
    return deadline;
}

}  // namespace glint::policy

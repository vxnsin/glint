#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/fd.hpp"
#include "core/loop.hpp"
#include "gpu/context.hpp"
#include "gpu/surface.hpp"
#include "wayland/display.hpp"

namespace glint {

// Pause reasons; they stack. An output draws only when none is set.
enum PauseReason : std::uint32_t {
    pause_covered = 1u << 0,  // a fullscreen window is on the output (set by glint)
    pause_locked = 1u << 1,   // the session is locked (set by the shell)
    pause_request = 1u << 2,  // asked for (set by the shell or the user)
    pause_idle = 1u << 3,     // no input for a while (set by glint, later)
};

std::string_view pause_reason_name(std::uint32_t bit);
std::uint32_t pause_reason_from(std::string_view name);  // 0 if unknown
std::vector<std::string> pause_reason_names(std::uint32_t reasons);

class Daemon : public wl::Display::Listener {
public:
    Daemon();
    ~Daemon() override;

    int run();

    // wl::Display::Listener
    void output_added(wl::Output& output) override;
    void output_removed(wl::Output& output) override;
    void output_resized(wl::Output& output) override;
    void output_frame(wl::Output& output, std::uint32_t time_ms) override;
    void output_covered(wl::Output& output, bool covered) override;
    void pointer(wl::Output& output, double x, double y, std::uint32_t buttons) override;

private:
    // One per output with a configured layer surface.
    struct Screen {
        wl::Output* output = nullptr;
        std::unique_ptr<gpu::Surface> surface;
        float background[4] = {0, 0, 0, 1};

        bool dirty = true;         // something changed that is not on screen yet
        bool resized = true;       // the swapchain needs the new size
        std::uint32_t paused = 0;  // this output's own reasons; see reasons()

        // Statistics for status.
        std::uint64_t frames = 0;
        std::array<std::int64_t, 64> present_ns{};  // ring of recent present times
        double cpu_ms = 0;
        double pointer[2] = {0, 0};
    };

    bool setup_signals();
    Screen* screen(wl::Output& output);
    std::uint32_t reasons(const Screen& screen) const;
    bool drawable(const Screen& screen) const;
    void render(Screen& screen);
    void before_poll();
    void after_poll();
    void pause_changed(Screen& screen, std::uint32_t before);

    Loop loop_;
    wl::Display display_;
    std::unique_ptr<gpu::Context> gpu_;
    std::vector<std::unique_ptr<Screen>> screens_;
    std::uint32_t paused_all_ = 0;  // reasons set for "*", on every output including new ones
    UniqueFd signal_fd_;
    int wayland_id_ = 0;
};

}  // namespace glint

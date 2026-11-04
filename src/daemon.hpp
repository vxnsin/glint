#pragma once

#include "core/fd.hpp"
#include "core/loop.hpp"
#include "wayland/display.hpp"

namespace glint {

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
    bool setup_signals();

    Loop loop_;
    wl::Display display_;
    UniqueFd signal_fd_;
    int wayland_id_ = 0;
};

}  // namespace glint

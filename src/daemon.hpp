#pragma once

#include <memory>
#include <vector>

#include "core/fd.hpp"
#include "core/loop.hpp"
#include "gpu/context.hpp"
#include "gpu/surface.hpp"
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
    // One per output with a configured layer surface.
    struct Screen {
        wl::Output* output = nullptr;
        std::unique_ptr<gpu::Surface> surface;
        float background[4] = {0, 0, 0, 1};
    };

    bool setup_signals();
    Screen* screen(wl::Output& output);
    void draw(Screen& screen);

    Loop loop_;
    wl::Display display_;
    std::unique_ptr<gpu::Context> gpu_;
    std::vector<std::unique_ptr<Screen>> screens_;
    UniqueFd signal_fd_;
    int wayland_id_ = 0;
};

}  // namespace glint

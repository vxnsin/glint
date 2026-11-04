#include "daemon.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>

#include "core/log.hpp"

namespace glint {

Daemon::Daemon() : display_(*this) {}

Daemon::~Daemon() = default;

bool Daemon::setup_signals() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) return false;
    signal_fd_.reset(::signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signal_fd_) return false;
    // A client that goes away mid-write must not kill the daemon.
    ::signal(SIGPIPE, SIG_IGN);
    loop_.add(signal_fd_.get(), POLLIN, [this](short) {
        signalfd_siginfo info{};
        while (::read(signal_fd_.get(), &info, sizeof(info)) == sizeof(info)) {
            log::info("signal {}, stopping", info.ssi_signo);
            loop_.quit();
        }
    });
    return true;
}

int Daemon::run() {
    if (!setup_signals()) {
        log::error("cannot set up signal handling");
        return 1;
    }
    if (std::string error = display_.connect(); !error.empty()) {
        log::error("{}", error);
        return 1;
    }
    if (auto device = display_.main_device()) {
        log::info("compositor renders on DRM device {}:{}", major(*device), minor(*device));
    }

    wayland_id_ = loop_.add(display_.fd(), POLLIN, [](short) {});
    loop_.set_before_poll([this] {
        if (!display_.prepare()) {
            log::error("Wayland connection lost");
            loop_.quit();
            return;
        }
        loop_.modify(wayland_id_, display_.flush_pending() ? POLLIN | POLLOUT : POLLIN);
    });
    loop_.set_after_poll([this] {
        short revents = loop_.revents(wayland_id_);
        if ((revents & POLLIN) != 0) {
            display_.read();
        } else {
            display_.cancel();
        }
        if ((revents & (POLLERR | POLLHUP)) != 0 || !display_.dispatch()) {
            log::error("Wayland connection lost");
            loop_.quit();
        }
    });

    bool ok = loop_.run();
    log::info("stopped after {} wakeups", loop_.wakeups());
    return ok ? 0 : 1;
}

void Daemon::output_added(wl::Output& output) {
    log::info("{}: {} {}x{} px, logical {}x{}, scale {:.3f}, {:.2f} Hz, at {},{}", output.name(),
              output.description(), output.pixel_width(), output.pixel_height(),
              output.logical_width(), output.logical_height(), output.scale(), output.refresh_hz(),
              output.x(), output.y());
}

void Daemon::output_removed(wl::Output& output) {
    log::info("{}: surface gone", output.name());
}

void Daemon::output_resized(wl::Output& output) {
    log::info("{}: now {}x{} px, scale {:.3f}", output.name(), output.pixel_width(),
              output.pixel_height(), output.scale());
}

void Daemon::output_frame(wl::Output& output, std::uint32_t time_ms) {
    log::debug("{}: frame at {} ms", output.name(), time_ms);
}

void Daemon::output_covered(wl::Output& output, bool covered) {
    log::info("{}: {}", output.name(), covered ? "covered by a fullscreen window" : "uncovered");
}

void Daemon::pointer(wl::Output& output, double x, double y, std::uint32_t buttons) {
    log::debug("{}: pointer {:.1f},{:.1f} buttons {}", output.name(), x, y, buttons);
}

}  // namespace glint

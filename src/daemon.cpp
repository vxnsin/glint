#include "daemon.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>

#include <algorithm>

#include "core/clock.hpp"
#include "core/log.hpp"

namespace glint {

namespace {

constexpr std::array<std::pair<std::uint32_t, std::string_view>, 4> reason_names = {{
    {pause_covered, "covered"},
    {pause_locked, "locked"},
    {pause_request, "request"},
    {pause_idle, "idle"},
}};

}  // namespace

std::string_view pause_reason_name(std::uint32_t bit) {
    for (auto [value, name] : reason_names) {
        if (value == bit) return name;
    }
    return "?";
}

std::uint32_t pause_reason_from(std::string_view name) {
    for (auto [value, text] : reason_names) {
        if (text == name) return value;
    }
    return 0;
}

std::vector<std::string> pause_reason_names(std::uint32_t reasons) {
    std::vector<std::string> names;
    for (auto [value, name] : reason_names) {
        if ((reasons & value) != 0) names.emplace_back(name);
    }
    return names;
}

Daemon::Daemon() : display_(*this) {}

Daemon::~Daemon() {
    screens_.clear();
    gpu_.reset();
}

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
    std::string error;
    gpu_ = gpu::Context::create(display_.handle(), display_.main_device(), error);
    if (!gpu_) {
        log::error("{}", error);
        return 1;
    }
    // Outputs configured during connect() arrived before the GPU existed.
    for (const auto& output : display_.outputs()) {
        if (output->configured()) output_added(*output);
    }

    wayland_id_ = loop_.add(display_.fd(), POLLIN, [](short) {});
    loop_.set_before_poll([this] { before_poll(); });
    loop_.set_after_poll([this] { after_poll(); });

    bool ok = loop_.run();
    log::info("stopped after {} wakeups", loop_.wakeups());
    return ok ? 0 : 1;
}

// Everything that draws happens here, outside libwayland's prepared-read window: the
// present calls into the Wayland connection.
void Daemon::before_poll() {
    for (auto& entry : screens_) {
        if (drawable(*entry)) render(*entry);
    }
    // Free what the GPU is done with. Whatever is still in flight is waited for now, while
    // the GPU is busy for a millisecond at most, rather than needing a wakeup later.
    if (gpu_->collect()) gpu_->drain();

    if (!display_.prepare()) {
        log::error("Wayland connection lost");
        loop_.quit();
        return;
    }
    loop_.modify(wayland_id_, display_.flush_pending() ? POLLIN | POLLOUT : POLLIN);
}

void Daemon::after_poll() {
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
}

Daemon::Screen* Daemon::screen(wl::Output& output) {
    for (auto& entry : screens_) {
        if (entry->output == &output) return entry.get();
    }
    return nullptr;
}

std::uint32_t Daemon::reasons(const Screen& entry) const {
    return entry.paused | paused_all_ | (entry.output->covered() ? pause_covered : 0u);
}

bool Daemon::drawable(const Screen& entry) const {
    // A frame callback still pending means the compositor has not shown the last frame yet;
    // the next draw waits for it.
    return entry.dirty && reasons(entry) == 0 && !entry.output->frame_pending() &&
           entry.output->configured();
}

void Daemon::render(Screen& entry) {
    std::int64_t start = clock::now_ns();
    wl::Output& output = *entry.output;
    if (entry.resized) {
        output.apply_surface_state();
        if (!entry.surface->resize(output.pixel_width(), output.pixel_height())) return;
        entry.resized = false;
    }

    gpu::FrameUniforms uniforms{};
    uniforms.resolution[0] = static_cast<float>(entry.surface->extent().width);
    uniforms.resolution[1] = static_cast<float>(entry.surface->extent().height);
    uniforms.pointer[0] = static_cast<float>(entry.pointer[0]);
    uniforms.pointer[1] = static_cast<float>(entry.pointer[1]);

    gpu::Target target{};
    if (!entry.surface->begin(entry.background, uniforms, target)) {
        // Out of date: recreate with the next draw.
        entry.resized = true;
        return;
    }
    bool more = false;  // whether another frame is wanted after this one

    // Only ask for a frame callback when another frame will be wanted; a still picture
    // presents once and then gets no more callbacks, so no more wakeups.
    if (more) output.request_frame();
    entry.surface->end();
    entry.dirty = more;

    entry.present_ns[entry.frames % entry.present_ns.size()] = start;
    ++entry.frames;
    entry.cpu_ms = clock::ms_since(start);
    log::debug("{}: frame {} in {:.2f} ms CPU", output.name(), entry.frames, entry.cpu_ms);
}

void Daemon::output_added(wl::Output& output) {
    if (!gpu_ || screen(output) != nullptr) return;
    log::info("{}: {} {}x{} px, logical {}x{}, scale {:.3f}, {:.2f} Hz, at {},{}", output.name(),
              output.description(), output.pixel_width(), output.pixel_height(),
              output.logical_width(), output.logical_height(), output.scale(), output.refresh_hz(),
              output.x(), output.y());
    auto entry = std::make_unique<Screen>();
    entry->output = &output;
    entry->surface = std::make_unique<gpu::Surface>(*gpu_, display_.handle(), output.surface());
    if (!entry->surface->valid()) return;
    screens_.push_back(std::move(entry));
}

void Daemon::output_removed(wl::Output& output) {
    log::info("{}: surface gone", output.name());
    std::erase_if(screens_, [&](const auto& entry) { return entry->output == &output; });
}

void Daemon::output_resized(wl::Output& output) {
    log::info("{}: now {}x{} px, scale {:.3f}", output.name(), output.pixel_width(),
              output.pixel_height(), output.scale());
    if (Screen* entry = screen(output)) {
        entry->resized = true;
        entry->dirty = true;
    }
}

void Daemon::output_frame(wl::Output& output, std::uint32_t time_ms) {
    log::debug("{}: frame callback at {} ms", output.name(), time_ms);
}

void Daemon::output_covered(wl::Output& output, bool covered) {
    log::info("{}: {}", output.name(), covered ? "covered by a fullscreen window" : "uncovered");
    if (Screen* entry = screen(output)) {
        pause_changed(*entry, reasons(*entry) ^ pause_covered);
    }
}

void Daemon::pause_changed(Screen& entry, std::uint32_t before) {
    std::uint32_t now = reasons(entry);
    if ((before == 0) == (now == 0)) return;
    log::info("{}: {}", entry.output->name(), now != 0 ? "paused" : "running");
}

void Daemon::pointer(wl::Output& output, double x, double y, std::uint32_t buttons) {
    if (Screen* entry = screen(output)) {
        entry->pointer[0] = x;
        entry->pointer[1] = y;
    }
    (void)buttons;
}

}  // namespace glint

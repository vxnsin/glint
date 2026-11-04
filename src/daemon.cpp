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

// The staging ring holds one 2560x1440 RGBA picture; larger ones get a buffer of their own
// for the duration of the upload.
constexpr VkDeviceSize staging_size = 16u << 20;

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
    // Wallpapers first (they hold textures), then the loaders, then the GPU.
    screens_.clear();
    workers_.reset();
    textures_.reset();
    services_.reset();
    pictures_.reset();
    if (gpu_) gpu_->drain();
    staging_.reset();
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
    staging_ = std::make_unique<gpu::StagingRing>(*gpu_, staging_size);
    pictures_ = std::make_unique<gpu::Pictures>(*gpu_);
    workers_ = std::make_unique<Workers>(loop_, 2);
    textures_ = std::make_unique<image::Textures>(*gpu_, *staging_, *workers_);
    services_ =
        std::make_unique<Services>(Services{*gpu_, *pictures_, *staging_, *workers_, *textures_});

    if (!show_at_start_.empty()) all_ = Assignment{show_at_start_, Options{}};

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

Daemon::Screen* Daemon::screen(std::string_view name) {
    for (auto& entry : screens_) {
        if (entry->output->name() == name) return entry.get();
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

    // Crossfade progress; the incoming wallpaper is only drawn once it is ready.
    bool fading = entry.incoming && entry.incoming->ready() && entry.fade_start_ns != 0;
    float t = 1.0f;
    if (fading) {
        int duration = entry.fade_ms;
        t = duration <= 0 ? 1.0f
                          : std::clamp(static_cast<float>(clock::ms_since(entry.fade_start_ns)) /
                                           static_cast<float>(duration),
                                       0.0f, 1.0f);
    }

    gpu::FrameUniforms uniforms{};
    uniforms.resolution[0] = static_cast<float>(entry.surface->extent().width);
    uniforms.resolution[1] = static_cast<float>(entry.surface->extent().height);
    uniforms.pointer[0] = static_cast<float>(entry.pointer[0]);
    uniforms.pointer[1] = static_cast<float>(entry.pointer[1]);

    // Cleared to the background of what is on screen, so a fit picture fading in over
    // nothing fades in from that colour.
    float clear[4] = {0, 0, 0, 1};
    const Wallpaper* base =
        entry.current ? entry.current.get() : (fading ? entry.incoming.get() : nullptr);
    if (base != nullptr) std::copy_n(base->options().linear, 3, clear);

    gpu::Target target{};
    if (!entry.surface->begin(clear, uniforms, target)) {
        // Out of date: recreate with the next draw.
        entry.resized = true;
        return;
    }
    Frame frame{.time = 0, .delta = 0, .index = entry.frames};
    if (entry.current) entry.current->render(frame, target, 1.0f);
    if (fading) entry.incoming->render(frame, target, t);

    bool fade_done = fading && t >= 1.0f;
    bool more = (fading && !fade_done) || (entry.current && entry.current->wants_frames()) ||
                (fade_done && entry.incoming->wants_frames());

    // Only ask for a frame callback when another frame will be wanted; a still picture
    // presents once and then gets no more callbacks, so no more wakeups.
    if (more) output.request_frame();
    entry.surface->end();
    entry.dirty = more;

    entry.present_ns[entry.frames % entry.present_ns.size()] = start;
    entry.last_present_ns = start;
    ++entry.frames;
    entry.cpu_ms = clock::ms_since(start);

    // The first frame that shows the new wallpaper completes its set request.
    if (fading && entry.request) {
        finish(entry.request, {});
        entry.request.reset();
    }
    if (fade_done) {
        entry.current = std::move(entry.incoming);
        entry.fade_start_ns = 0;
    }
    log::debug("{}: frame {} in {:.2f} ms CPU{}", output.name(), entry.frames, entry.cpu_ms,
               fading ? std::format(", fade {:.2f}", t) : "");
}

// ---- Wallpapers -------------------------------------------------------------------------

const Daemon::Assignment* Daemon::assignment(std::string_view name) const {
    if (auto it = outputs_.find(std::string(name)); it != outputs_.end()) return &it->second;
    return all_ ? &*all_ : nullptr;
}

void Daemon::set(std::string_view output, const std::filesystem::path& path, const Options& options,
                 SetDone done) {
    auto request = std::make_shared<Request>();
    request->done = std::move(done);
    request->start_ns = clock::now_ns();

    std::vector<Screen*> targets;
    if (output == "*") {
        // "*" is every output now and later; it replaces the per-output entries.
        all_ = Assignment{path, options};
        outputs_.clear();
        for (auto& entry : screens_) targets.push_back(entry.get());
    } else {
        Screen* entry = screen(output);
        if (entry == nullptr) {
            request->done({.error = "no such output"});
            return;
        }
        outputs_[std::string(output)] = Assignment{path, options};
        targets.push_back(entry);
    }
    request->remaining = static_cast<int>(targets.size());
    if (targets.empty()) {
        finish(request, {});
        return;
    }
    for (Screen* entry : targets) apply(*entry, *assignment(entry->output->name()), request);
}

void Daemon::apply(Screen& entry, const Assignment& assignment, std::shared_ptr<Request> request) {
    // A load still running for this output gives way.
    if (entry.request) {
        finish(entry.request, "superseded");
        entry.request.reset();
    }
    std::string error;
    auto wallpaper = make_wallpaper(assignment.path, assignment.options, *services_, error);
    if (!wallpaper) {
        entry.error = error;
        if (request) finish(request, error);
        log::warn("{}: {}: {}", entry.output->name(), assignment.path.string(), error);
        return;
    }
    if (request) request->kind = std::string(kind_name(wallpaper->kind()));
    // If a crossfade is running, the incoming one is far enough: it becomes the current one.
    if (entry.incoming && entry.incoming->ready() && entry.fade_start_ns != 0) {
        entry.current = std::move(entry.incoming);
        entry.fade_start_ns = 0;
    }
    entry.incoming = std::move(wallpaper);
    entry.request = std::move(request);
    entry.error.clear();
    Wallpaper* incoming = entry.incoming.get();
    wl::Output* output = entry.output;
    incoming->load([this, output, incoming](std::string load_error) {
        Screen* target = screen(*output);
        if (target != nullptr && target->incoming.get() == incoming) {
            loaded(*target, incoming, load_error);
        }
    });
}

void Daemon::loaded(Screen& entry, Wallpaper* wallpaper, const std::string& error) {
    if (!error.empty()) {
        log::warn("{}: {}: {}", entry.output->name(), wallpaper->path().string(), error);
        entry.error = error;
        entry.incoming.reset();
        if (entry.request) finish(entry.request, error);
        entry.request.reset();
        return;
    }
    // The first wallpaper on an output appears at once; later ones fade if asked to.
    bool fade = entry.current && wallpaper->options().transition == "fade";
    entry.fade_ms = fade ? std::max(wallpaper->options().duration_ms, 0) : 0;
    entry.fade_start_ns = clock::now_ns();
    entry.dirty = true;
    // A paused output takes the new wallpaper without drawing; the request is done now.
    if (reasons(entry) != 0 && entry.request) {
        finish(entry.request, {});
        entry.request.reset();
    }
}

void Daemon::finish(const std::shared_ptr<Request>& request, const std::string& error) {
    if (request->finished) return;
    if (!error.empty()) {
        request->finished = true;
        request->error = error;
        if (request->done) request->done({.error = error, .kind = request->kind});
        return;
    }
    if (--request->remaining > 0) return;
    request->finished = true;
    double ms = clock::ms_since(request->start_ns);
    for (auto& entry : screens_) {
        if (entry->request == request) entry->load_ms = ms;
    }
    if (request->done) request->done({.kind = request->kind, .load_ms = ms});
}

// ---- Outputs ----------------------------------------------------------------------------

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
    Screen& added = *entry;
    screens_.push_back(std::move(entry));
    if (const Assignment* a = assignment(output.name())) {
        // Nothing is drawn until the picture is ready, so the output does not flash the
        // background colour first.
        added.dirty = false;
        apply(added, *a, nullptr);
    }
}

void Daemon::output_removed(wl::Output& output) {
    log::info("{}: surface gone", output.name());
    if (Screen* entry = screen(output); entry != nullptr && entry->request) {
        finish(entry->request, "output removed");
    }
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
    if (now != 0) {
        if (entry.current) entry.current->pause();
        // A set waiting for its first frame will not get one while paused.
        if (entry.request && entry.incoming && entry.incoming->ready()) {
            finish(entry.request, {});
            entry.request.reset();
        }
    } else if (entry.current) {
        entry.current->resume();
    }
}

void Daemon::pointer(wl::Output& output, double x, double y, std::uint32_t buttons) {
    if (Screen* entry = screen(output)) {
        entry->pointer[0] = x;
        entry->pointer[1] = y;
        if (entry->current && entry->current->wants_cursor()) {
            entry->current->cursor({x, y, buttons});
        }
    }
}

}  // namespace glint

#include "daemon.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>

#include <algorithm>
#include <cstdlib>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/version.hpp"
#include "gpu/texture.hpp"
#include "ipc/library.hpp"
#include "ipc/protocol.hpp"
#include "wallpaper/image/decode.hpp"
#include "wallpaper/image/image.hpp"
#include "wallpaper/scene/scene_wallpaper.hpp"

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

Daemon::Daemon() : display_(*this) {
    const char* force = std::getenv("GLINT_TRACK_TOPLEVELS");
    force_toplevels_ = force != nullptr && force[0] == '1';
}

Daemon::~Daemon() {
    // Clients first, wallpapers (they hold textures), then the loaders, then the GPU.
    server_.reset();
    screens_.clear();
    display_.shutdown();  // its outputs call back into screens_, which is still here
    workers_.reset();
    textures_.reset();
    scenes_.reset();
    videos_.reset();
    services_.reset();
    blit_.reset();
    video_.reset();
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
    blit_ = std::make_unique<gpu::Blit>(*gpu_);
    video_ = std::make_unique<gpu::VideoPass>(*gpu_);
    scenes_ = std::make_unique<scene::SceneCache>(*gpu_, *staging_, *workers_);
    videos_ = std::make_unique<video::Sources>();
    services_ = std::make_unique<Services>(Services{
        *gpu_, *pictures_, *staging_, *workers_, *textures_, *scenes_, *blit_, *video_, *videos_});

    server_ = std::make_unique<ipc::Server>(
        loop_, [this](const nlohmann::json& request, ipc::Server::Reply reply) {
            handle(request, std::move(reply));
        });
    if (std::string listen_error = server_->listen(paths::socket_path().string());
        !listen_error.empty()) {
        log::error("{}", listen_error);
        return 1;
    }

    // What was on screen last time.
    state::State restored = state::load();
    all_ = restored.all;
    outputs_ = std::move(restored.outputs);
    properties_ = std::move(restored.properties);
    if (all_ || !outputs_.empty()) {
        log::info("restoring {} from {}", all_ ? all_->path.string() : "per-output wallpapers",
                  state::file().string());
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
    // Mesa reads the connection while a frame is presented; events for this client that
    // arrive then are dispatched here, and what they changed is drawn in the same pass.
    for (int pass = 0; pass < 3; ++pass) {
        for (auto& entry : screens_) {
            if (drawable(*entry) && due(*entry)) render(*entry);
        }
        if (display_.dispatch_pending() <= 0) break;
    }
    emit_states();
    // Fullscreen windows matter only while something animates (or when asked to for tests).
    bool animating = force_toplevels_;
    for (const auto& entry : screens_) {
        if ((entry->current && entry->current->wants_frames()) ||
            (entry->incoming && entry->incoming->wants_frames())) {
            animating = true;
        }
    }
    display_.track_toplevels(animating);

    // Audio capture only while a shown, running wallpaper wants the spectrum.
    bool audio = false;
    for (const auto& entry : screens_) {
        if (reasons(*entry) != 0) continue;
        if ((entry->current && entry->current->wants_audio()) ||
            (entry->incoming && entry->incoming->ready() && entry->incoming->wants_audio())) {
            audio = true;
        }
    }
    if (audio && !capture_.running()) {
        if (std::string error = capture_.start(); !error.empty()) {
            log::warn("audio capture: {}", error);
        } else {
            log::info("audio capture started");
        }
    } else if (!audio && capture_.running()) {
        capture_.stop();
        log::info("audio capture stopped");
    }

    // The media client only while a shown, running wallpaper shows or reacts to media.
    bool media = false;
    for (const auto& entry : screens_) {
        if (reasons(*entry) != 0) continue;
        if ((entry->current && entry->current->wants_media()) ||
            (entry->incoming && entry->incoming->ready() && entry->incoming->wants_media())) {
            media = true;
        }
    }
    if (media && !mpris_ && !mpris_failed_) {
        mpris_ = std::make_unique<media::Mpris>(loop_, *workers_, *gpu_, *staging_);
        if (std::string error = mpris_->start(); !error.empty()) {
            // No session bus, say: said once, not tried again every frame.
            log::warn("media: {} (media integration off)", error);
            mpris_.reset();
            mpris_failed_ = true;
        } else {
            log::info("media client started");
            mpris_->set_listener([this] {
                for (auto& entry : screens_) entry->dirty = true;
            });
        }
    } else if (!media && mpris_) {
        mpris_.reset();
        log::info("media client stopped");
    }
    if (mpris_) mpris_->prepare_poll();

    // Free what the GPU is done with. Whatever is still in flight is waited for now, while
    // the GPU is busy for a millisecond at most, rather than needing a wakeup later.
    if (gpu_->collect()) gpu_->drain();
    // Staging memory is only needed while pictures load.
    bool loading = std::ranges::any_of(
        screens_, [](const auto& e) { return e->incoming && !e->incoming->ready(); });
    if (!loading) staging_->trim();

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

bool Daemon::due(Screen& entry) {
    int fps = entry.current ? entry.current->options().fps : 0;
    if (entry.incoming && entry.incoming->ready()) {
        int f = entry.incoming->options().fps;
        if (f > 0 && (fps == 0 || f > fps)) fps = f;  // a fade runs at the faster of the two
    }
    if (fps <= 0) {
        entry.cap_due_ns = 0;
        return true;
    }
    // Draws are due on a grid of the period, so the rate holds exactly over time whatever
    // the timer's lateness; the grid restarts when a draw is a whole period late (after a
    // pause). Hyprland's frame callback follows the commit within a few milliseconds and
    // says nothing about the vblank, so it is no clock to go by: the draws land on whatever
    // vblank follows them, and 24 on a 60 Hz output alternates two and three vblanks.
    auto period_ns = static_cast<std::int64_t>(1e9 / fps);
    std::int64_t now = clock::now_ns();
    if (entry.cap_due_ns == 0 || now >= entry.cap_due_ns + period_ns) entry.cap_due_ns = now;
    if (now + 500'000 >= entry.cap_due_ns) {
        entry.cap_due_ns += period_ns;
        return true;
    }
    if (entry.timer.get() < 0) {
        entry.timer.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
        if (entry.timer.get() < 0) return true;
        Screen* raw = &entry;
        entry.timer_id = loop_.add(entry.timer.get(), POLLIN, [raw](short) {
            std::uint64_t expirations = 0;
            [[maybe_unused]] ssize_t n =
                ::read(raw->timer.get(), &expirations, sizeof(expirations));
            raw->timer_armed = false;
            // The loop's next before_poll draws the frame.
        });
    }
    if (!entry.timer_armed) {
        itimerspec spec{};
        spec.it_value.tv_sec = static_cast<time_t>(entry.cap_due_ns / 1'000'000'000);
        spec.it_value.tv_nsec = static_cast<long>(entry.cap_due_ns % 1'000'000'000);
        ::timerfd_settime(entry.timer.get(), TFD_TIMER_ABSTIME, &spec, nullptr);
        entry.timer_armed = true;
    }
    return false;
}

void Daemon::render(Screen& entry) {
    std::int64_t start = clock::now_ns();
    wl::Output& output = *entry.output;
    if (entry.resized) {
        output.apply_surface_state();
        if (!entry.surface->resize(output.pixel_width(), output.pixel_height())) {
            loop_.wake();  // tried again with the next pass, which needs a wakeup
            return;
        }
        entry.resized = false;
    }

    // A ready incoming wallpaper may still need its first render-plan/script setup.
    // Start the visible fade only after that work, when a frame can be submitted.
    bool fading = entry.incoming && entry.incoming->ready();

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

    // Each wallpaper's own clock, pauses excluded.
    if (entry.current && entry.current_origin_ns == 0) entry.current_origin_ns = start;
    if (fading && entry.incoming_origin_ns == 0) entry.incoming_origin_ns = start;
    double delta = entry.last_frame_ns != 0
                       ? std::min(static_cast<double>(start - entry.last_frame_ns) / 1e9, 0.25)
                       : 0.0;
    entry.last_frame_ns = start;
    auto seconds = [start](std::int64_t origin) {
        return origin != 0 ? static_cast<double>(start - origin) / 1e9 : 0.0;
    };
    Frame current_frame{seconds(entry.current_origin_ns), delta, entry.frames};
    Frame incoming_frame{seconds(entry.incoming_origin_ns), delta, entry.frames};

    // A wallpaper that wants the cursor gets it: on Hyprland from its socket, wherever the
    // pointer is; elsewhere from pointer input on the surface, which only sees the pointer
    // while no window is over it.
    bool cursor = (entry.current && entry.current->wants_cursor()) ||
                  (fading && entry.incoming->wants_cursor());
    output.set_pointer_input(cursor && !hyprland_.available());
    if (cursor && hyprland_.available()) {
        if (auto at = hyprland_.position()) {
            double px = (at->first - output.x()) * output.scale();
            double py = (at->second - output.y()) * output.scale();
            Cursor c{px, py, 0};
            if (entry.current && entry.current->wants_cursor()) entry.current->cursor(c);
            if (fading && entry.incoming->wants_cursor()) entry.incoming->cursor(c);
        }
    }

    if (capture_.running()) {
        audio::Levels levels = capture_.levels();
        if (entry.current) entry.current->audio_spectrum(levels.left.data(), levels.right.data());
        if (fading) entry.incoming->audio_spectrum(levels.left.data(), levels.right.data());
    }
    if (mpris_) {
        const MediaState& media = mpris_->state();
        if (entry.current && entry.current->wants_media()) entry.current->media(media);
        if (fading && entry.incoming->wants_media()) entry.incoming->media(media);
    }

    gpu::Target target{};
    auto offscreen = [&](VkCommandBuffer cmd, VkExtent2D extent) {
        if (entry.current) entry.current->prepare(current_frame, cmd, extent);
        if (fading) entry.incoming->prepare(incoming_frame, cmd, extent);
    };
    if (!entry.surface->begin(clear, uniforms, target, offscreen)) {
        // Out of date: recreate with the next draw, which needs a wakeup of its own.
        entry.resized = true;
        loop_.wake();
        return;
    }
    const std::int64_t fade_now = clock::now_ns();
    const float t = fading ? entry.transition.opacity(fade_now, entry.fade_ms) : 1.0f;
    if (entry.current) entry.current->render(current_frame, target, 1.0f);
    if (fading) entry.incoming->render(incoming_frame, target, t);

    bool fade_done = fading && t >= 1.0f;
    bool more = (fading && !fade_done) || (entry.current && entry.current->wants_frames()) ||
                (fade_done && entry.incoming->wants_frames());

    // Only ask for a frame callback when another frame will be wanted; a still picture
    // presents once and then gets no more callbacks, so no more wakeups.
    if (more) output.request_frame();
    const bool submitted = entry.surface->end();
    if (!submitted) {
        // Suboptimal or out of date: the swapchain is made anew with the next draw.
        entry.resized = true;
        entry.dirty = true;
        loop_.wake();
    } else {
        if (fading) entry.transition.submitted(fade_now);
        entry.dirty = more;
    }

    entry.present_ns[entry.frames % entry.present_ns.size()] = start;
    entry.last_present_ns = start;
    ++entry.frames;
    entry.cpu_ms = clock::ms_since(start);

    // The first frame that shows the new wallpaper completes its set request.
    if (submitted && fading && entry.request) {
        entry.load_ms = clock::ms_since(entry.request->start_ns);
        finish(entry.request, {});
        entry.request.reset();
    }
    if (submitted && fade_done) {
        entry.current = std::move(entry.incoming);
        entry.current_origin_ns = entry.incoming_origin_ns;
        entry.incoming_origin_ns = 0;
        entry.transition.reset();
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

    // What can be checked now is checked before anything changes; only a picture that
    // fails to decode is found out later, and then the old assignment comes back.
    if (std::string error = check_path(path); !error.empty()) {
        request->done({.error = error});
        return;
    }
    if (output != "*" && screen(output) == nullptr) {
        request->done({.error = "no such output"});
        return;
    }
    request->previous_all = all_;
    request->previous_outputs = outputs_;
    request->generation = ++generation_;

    std::vector<Screen*> targets;
    if (output == "*") {
        // "*" is every output now and later; it replaces the per-output entries.
        all_ = Assignment{path, options};
        outputs_.clear();
        for (auto& entry : screens_) targets.push_back(entry.get());
    } else {
        Screen* entry = screen(output);
        outputs_[std::string(output)] = Assignment{path, options};
        targets.push_back(entry);
    }
    save_state();
    request->remaining = static_cast<int>(targets.size());
    if (targets.empty()) {
        finish(request, {});
        return;
    }
    // Resolved before applying: a failure rolls the assignments back while this loop runs.
    std::vector<std::pair<Screen*, Assignment>> resolved;
    for (Screen* entry : targets) resolved.emplace_back(entry, *assignment(entry->output->name()));
    for (auto& [entry, a] : resolved) apply(*entry, a, request);
}

void Daemon::apply(Screen& entry, const Assignment& assignment, std::shared_ptr<Request> request) {
    // A load still running for this output gives way.
    if (entry.request) {
        finish(entry.request, "superseded");
        entry.request.reset();
    }
    std::string error;
    // Scene settings saved for this project come along.
    Options options = assignment.options;
    if (auto it = properties_.find(assignment.path.string()); it != properties_.end()) {
        if (!options.properties.is_object()) options.properties = nlohmann::json::object();
        for (const auto& [k, v] : it->second.items()) options.properties[k] = v;
    }
    auto wallpaper = make_wallpaper(assignment.path, options, *services_, error);
    if (!wallpaper) {
        entry.error = error;
        if (request) finish(request, error);
        log::warn("{}: {}: {}", entry.output->name(), assignment.path.string(), error);
        return;
    }
    if (request) request->kind = std::string(kind_name(wallpaper->kind()));
    // If a crossfade is running, the incoming one is far enough: it becomes the current one.
    if (entry.incoming && entry.incoming->ready() && entry.transition.started()) {
        entry.current = std::move(entry.incoming);
        entry.current_origin_ns = entry.incoming_origin_ns;
        entry.transition.reset();
    }
    entry.incoming_origin_ns = 0;
    entry.incoming = std::move(wallpaper);
    entry.last_state.clear();  // An assignment can change while the output stays paused.
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
    entry.last_state.clear();
    if (!error.empty()) {
        log::warn("{}: {}: {}", entry.output->name(), wallpaper->path().string(), error);
        if (server_) {
            server_->broadcast("error", {{"output", entry.output->name()},
                                         {"message", wallpaper->path().string() + ": " + error}});
        }
        entry.error = error;
        entry.incoming.reset();
        if (entry.request) finish(entry.request, error);
        entry.request.reset();
        return;
    }
    // The first wallpaper on an output appears at once; later ones fade if asked to.
    bool fade = entry.current && wallpaper->options().transition == "fade";
    entry.fade_ms = fade ? std::max(wallpaper->options().duration_ms, 0) : 0;
    entry.transition.reset();  // the first submitted frame starts the visible transition
    entry.dirty = true;
    // A paused output takes the new wallpaper without drawing, and one still waiting for the
    // compositor's frame callback (it is covered, say) draws when the compositor asks: the
    // request is done now for both, or it could wait for minutes.
    if ((reasons(entry) != 0 || entry.output->frame_pending()) && entry.request) {
        finish(entry.request, {});
        entry.request.reset();
    }
}

void Daemon::finish(const std::shared_ptr<Request>& request, const std::string& error) {
    if (request->finished) return;
    if (!error.empty()) {
        request->finished = true;
        request->error = error;
        if (error != "superseded" && request->generation == generation_ &&
            request->generation != 0) {
            all_ = request->previous_all;
            outputs_ = request->previous_outputs;
            ++generation_;
            save_state();
        }
        if (request->done) request->done({.error = error, .kind = request->kind});
        return;
    }
    if (--request->remaining > 0) return;
    request->finished = true;
    double ms = clock::ms_since(request->start_ns);
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
    if (server_)
        server_->broadcast("outputs",
                           {{"added", {output.name()}}, {"removed", nlohmann::json::array()}});
    if (const Assignment* a = assignment(output.name())) {
        // Nothing is drawn until the picture is ready, so the output does not flash the
        // background colour first.
        added.dirty = false;
        apply(added, *a, nullptr);
    }
}

void Daemon::output_removed(wl::Output& output) {
    log::info("{}: surface gone", output.name());
    if (Screen* entry = screen(output); entry != nullptr) {
        // The set did not fail, one of its targets went away: it is done as far as this
        // output is concerned.
        if (entry->request) finish(entry->request, {});
        if (entry->surface) entry->surface->cancel_capture();
    }
    bool had = screen(output) != nullptr;
    if (Screen* entry = screen(output); entry != nullptr && entry->timer_id >= 0) {
        loop_.remove(entry->timer_id);
    }
    std::erase_if(screens_, [&](const auto& entry) { return entry->output == &output; });
    if (had && server_) {
        server_->broadcast("outputs",
                           {{"added", nlohmann::json::array()}, {"removed", {output.name()}}});
    }
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
    std::int64_t at = clock::now_ns();
    if (now != 0) {
        entry.paused_at_ns = at;
    } else if (entry.paused_at_ns != 0) {
        // The clocks stand still while paused.
        std::int64_t gap = at - entry.paused_at_ns;
        if (entry.current_origin_ns != 0) entry.current_origin_ns += gap;
        if (entry.incoming_origin_ns != 0) entry.incoming_origin_ns += gap;
        entry.transition.resume(gap);
        entry.last_frame_ns = 0;
        entry.paused_at_ns = 0;
    }
    if (now != 0) {
        if (entry.surface) entry.surface->cancel_capture();  // no frame will come
        if (entry.current) entry.current->pause();
        // A set waiting for its first frame will not get one while paused.
        if (entry.request && entry.incoming && entry.incoming->ready()) {
            finish(entry.request, {});
            entry.request.reset();
        }
    } else {
        entry.output->abandon_frame();
        entry.dirty = true;  // the frame it would have drawn while paused
        if (entry.current) entry.current->resume();
        loop_.wake();
    }
}

void Daemon::pointer(wl::Output& output, double x, double y, std::uint32_t buttons) {
    if (Screen* entry = screen(output)) {
        entry->pointer[0] = x;
        entry->pointer[1] = y;
        if (entry->current && entry->current->wants_cursor()) {
            entry->current->cursor({x, y, buttons});
            entry->dirty = true;  // a parallax scene at rest moves with the pointer
        }
        if (entry->incoming && entry->incoming->ready() && entry->incoming->wants_cursor()) {
            entry->incoming->cursor({x, y, buttons});
            entry->dirty = true;
        }
    }
}

// ---- IPC --------------------------------------------------------------------------------

void Daemon::save_state() const {
    state::State current;
    current.all = all_;
    current.outputs = outputs_;
    current.properties = properties_;
    if (!state::save(current)) log::warn("cannot write {}", state::file().string());
}

std::string Daemon::state_of(const Screen& entry) const {
    if (reasons(entry) != 0) return "paused";
    if (entry.incoming && !entry.incoming->ready()) return "loading";
    if (entry.current || entry.incoming) return "running";
    if (!entry.error.empty()) return "error";
    return "none";
}

void Daemon::emit_states() {
    for (auto& entry : screens_) {
        std::string now = state_of(*entry);
        if (now == entry->last_state) continue;
        entry->last_state = now;
        if (!server_) continue;
        nlohmann::json event = {{"output", entry->output->name()}, {"state", now}};
        auto names = pause_reason_names(reasons(*entry));
        if (!names.empty()) event["reason"] = names.front();
        if (names.size() > 1) event["reasons"] = names;
        if (now == "error") event["message"] = entry->error;
        server_->broadcast("state", event);
    }
}

nlohmann::json Daemon::output_json(const Screen& entry) const {
    const wl::Output& output = *entry.output;
    nlohmann::json json = {
        {"name", output.name()},
        {"make", output.make()},
        {"model", output.model()},
        {"description", output.description()},
        {"width", output.pixel_width()},
        {"height", output.pixel_height()},
        {"scale", output.scale()},
        {"refresh", output.refresh_hz()},
        {"x", output.x()},
        {"y", output.y()},
    };
    const Wallpaper* shown = entry.incoming ? entry.incoming.get() : entry.current.get();
    nlohmann::json wallpaper = {{"state", state_of(entry)}};
    if (shown != nullptr) {
        wallpaper["path"] = shown->path().string();
        wallpaper["kind"] = kind_name(shown->kind());
    }
    auto names = pause_reason_names(reasons(entry));
    if (!names.empty()) wallpaper["reason"] = names.front();
    if (!entry.error.empty()) wallpaper["message"] = entry.error;
    json["wallpaper"] = wallpaper;
    return json;
}

nlohmann::json Daemon::status_json(Screen& entry) {
    const Wallpaper* shown = entry.incoming ? entry.incoming.get() : entry.current.get();
    // Frames presented in the last second.
    std::int64_t now = clock::now_ns();
    int recent = 0;
    for (std::int64_t t : entry.present_ns) {
        if (t != 0 && now - t < 1'000'000'000) ++recent;
    }
    auto names = pause_reason_names(reasons(entry));
    nlohmann::json json = {
        {"state", state_of(entry)},
        {"fps", recent},
        {"frames", entry.frames},
        {"frame_ms", entry.cpu_ms},
        {"gpu_ms", entry.surface->gpu_ms()},
        {"load_ms", entry.load_ms},
        {"paused_reason", names.empty() ? nlohmann::json(nullptr) : nlohmann::json(names.front())},
        {"paused_reasons", names},
        {"width", entry.surface->extent().width},
        {"height", entry.surface->extent().height},
        {"swapchain_images", entry.surface->image_count()},
    };
    std::uint64_t memory = 0;
    if (entry.current) memory += entry.current->memory();
    if (entry.incoming && entry.incoming.get() != entry.current.get()) {
        memory += entry.incoming->memory();
    }
    json["memory_mb"] = static_cast<double>(memory) / (1024.0 * 1024.0);
    if (shown != nullptr) {
        json["path"] = shown->path().string();
        json["kind"] = kind_name(shown->kind());
        json["options"] = state::options_json(shown->options());
        if (auto* picture = dynamic_cast<const image::Image*>(shown)) {
            json["picture"] = {{"width", picture->width()}, {"height", picture->height()}};
        }
    } else {
        json["path"] = nullptr;
        json["kind"] = nullptr;
    }
    if (!entry.error.empty()) json["message"] = entry.error;
    json["wakeups"] = loop_.wakeups();
    nlohmann::json audio = {{"capture", capture_.running()}};
    if (capture_.running()) {
        audio::Levels levels = capture_.levels();
        float peak = 0;
        for (std::size_t i = 0; i < levels.left.size(); ++i) {
            peak = std::max({peak, levels.left[i], levels.right[i]});
        }
        audio["peak"] = peak;
    }
    json["audio"] = audio;
    return json;
}

std::string Daemon::unset(std::string_view output) {
    ++generation_;
    std::vector<Screen*> targets;
    if (output == "*") {
        all_.reset();
        outputs_.clear();
        for (auto& entry : screens_) targets.push_back(entry.get());
    } else {
        Screen* entry = screen(output);
        if (entry == nullptr) return "no such output";
        outputs_.erase(std::string(output));
        // Without its own entry the output falls back to "*", if there is one.
        if (all_) {
            apply(*entry, *all_, nullptr);
            save_state();
            return {};
        }
        targets.push_back(entry);
    }
    for (Screen* entry : targets) {
        if (entry->request) finish(entry->request, "superseded");
        entry->request.reset();
        entry->current.reset();
        entry->incoming.reset();
        entry->transition.reset();
        entry->error.clear();
        entry->last_state.clear();
        entry->dirty = true;
    }
    save_state();
    return {};
}

std::string Daemon::pause(std::string_view output, std::uint32_t reason, bool on) {
    std::vector<std::pair<Screen*, std::uint32_t>> before;
    for (auto& entry : screens_) before.emplace_back(entry.get(), reasons(*entry));
    if (output == "*") {
        if (on) {
            paused_all_ |= reason;
        } else {
            // Lifting a reason for "*" lifts it everywhere.
            paused_all_ &= ~reason;
            for (auto& entry : screens_) entry->paused &= ~reason;
        }
    } else {
        Screen* entry = screen(output);
        if (entry == nullptr) return "no such output";
        if (on) {
            entry->paused |= reason;
        } else {
            entry->paused &= ~reason;
        }
    }
    for (auto [entry, was] : before) pause_changed(*entry, was);
    return {};
}

void Daemon::handle(const nlohmann::json& request, ipc::Server::Reply reply) {
    auto text_field = [&](const char* key, const char* fallback) {
        return request.contains(key) && request[key].is_string() ? request[key].get<std::string>()
                                                                 : std::string(fallback);
    };
    std::string op = text_field("op", "");
    std::string output = request.contains("output") && request["output"].is_string()
                             ? request["output"].get<std::string>()
                             : "";

    if (op == "version") {
        nlohmann::json operations = nlohmann::json::array();
        for (const auto name : ipc::implemented_ops) operations.push_back(std::string(name));
        reply.ok({{"version", version},
                  {"protocol", protocol_version},
                  {"vulkan", "1.3"},
                  {"device", gpu_->device_name()},
                  {"operations", std::move(operations)}});
    } else if (op == "outputs") {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& entry : screens_) list.push_back(output_json(*entry));
        reply.ok(list);
    } else if (op == "status") {
        if (!output.empty() && output != "*") {
            Screen* entry = screen(output);
            if (entry == nullptr) return reply.error("no such output");
            return reply.ok(status_json(*entry));
        }
        nlohmann::json all = nlohmann::json::object();
        for (auto& entry : screens_) all[entry->output->name()] = status_json(*entry);
        reply.ok(all);
    } else if (op == "set") {
        if (output.empty()) return reply.error("set needs an output");
        if (!request.contains("path") || !request["path"].is_string()) {
            return reply.error("set needs a path");
        }
        std::filesystem::path path = request["path"].get<std::string>();
        if (!path.is_absolute()) return reply.error("path must be absolute");
        Options options;
        if (request.contains("options")) {
            if (std::string error = state::parse_options(request["options"], options);
                !error.empty()) {
                return reply.error(error);
            }
        }
        std::string title = path.stem().string();
        set(output, path, options, [reply, title](const SetResult& result) mutable {
            if (!result.error.empty()) return reply.error(result.error);
            reply.ok({{"kind", result.kind}, {"title", title}, {"load_ms", result.load_ms}});
        });
    } else if (op == "unset") {
        if (output.empty()) return reply.error("unset needs an output");
        if (std::string error = unset(output); !error.empty()) return reply.error(error);
        reply.ok();
    } else if (op == "pause" || op == "resume") {
        std::string reason_name = text_field("reason", "request");
        std::uint32_t reason = pause_reason_from(reason_name);
        if (reason != pause_locked && reason != pause_request) {
            return reply.error("reason is locked or request (glint sets covered and idle itself)");
        }
        if (std::string error = pause(output.empty() ? "*" : output, reason, op == "pause");
            !error.empty()) {
            return reply.error(error);
        }
        reply.ok();
    } else if (op == "screenshot") {
        Screen* entry = screen(output);
        if (entry == nullptr) return reply.error("screenshot needs an output");
        if (!request.contains("path") || !request["path"].is_string()) {
            return reply.error("screenshot needs a path");
        }
        if (reasons(*entry) != 0) return reply.error("the output is paused, nothing is drawn");
        if (!entry->surface || !entry->surface->can_capture()) {
            return reply.error("the output cannot be read back");
        }
        if (entry->surface->capture_pending())
            return reply.error("a screenshot is already pending");
        std::filesystem::path path = request["path"].get<std::string>();
        if (!path.is_absolute()) return reply.error("path must be absolute");
        bool bgra = entry->surface->format() == VK_FORMAT_B8G8R8A8_SRGB ||
                    entry->surface->format() == VK_FORMAT_B8G8R8A8_UNORM;
        Workers* workers = workers_.get();
        entry->surface->capture_next([reply, path, bgra, workers](gpu::Surface::Capture c) mutable {
            if (c.pixels.empty()) {
                return reply.error("no frame came: the output is not being drawn (covered?)");
            }
            auto shot = std::make_shared<gpu::Surface::Capture>(std::move(c));
            auto error = std::make_shared<std::string>();
            workers->post(
                [shot, error, path, bgra] {
                    *error = image::write_png(path, shot->pixels, shot->width, shot->height, bgra);
                },
                [reply, shot, error, path]() mutable {
                    if (!error->empty()) return reply.error(*error);
                    reply.ok({{"path", path.string()},
                              {"width", shot->width},
                              {"height", shot->height}});
                });
        });
        entry->dirty = true;  // a still output draws once more for it
        // A compositor that does not draw this output sends no frame: the request is
        // answered after a while instead of waiting forever.
        int timer = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer >= 0) {
            itimerspec spec{};
            spec.it_value.tv_sec = 3;
            ::timerfd_settime(timer, 0, &spec, nullptr);
            wl::Output* output_ptr = entry->output;
            int* id = new int(0);
            *id = loop_.add(timer, POLLIN, [this, timer, id, output_ptr](short) {
                if (Screen* late = screen(*output_ptr); late != nullptr && late->surface) {
                    late->surface->cancel_capture();
                }
                loop_.remove(*id);
                ::close(timer);
                delete id;
            });
        }
    } else if (op == "props") {
        props(request, std::move(reply));
    } else if (op == "list" || op == "inspect") {
        std::string error;
        auto parsed = ipc::parse_library_request(request, error);
        if (!parsed) return reply.error(error);
        struct LibraryResult {
            nlohmann::json data;
            std::string error = "library inspection failed";
        };
        auto result = std::make_shared<LibraryResult>();
        workers_->post(
            [parsed = std::move(*parsed), result] {
                try {
                    result->data = ipc::execute_library_request(parsed, result->error);
                } catch (const std::exception& e) {
                    result->error = e.what();
                }
            },
            [result, reply = std::move(reply)]() mutable {
                if (!result->error.empty()) return reply.error(result->error);
                reply.ok(std::move(result->data));
            });
    } else if (op == "cursor") {
        // For compositors where glint cannot see the pointer: the shell sends it.
        Screen* entry = screen(output);
        if (entry == nullptr) return reply.error("cursor needs an output");
        auto number_field = [&](const char* key) {
            return request.contains(key) && request[key].is_number() ? request[key].get<double>()
                                                                     : 0.0;
        };
        Cursor c{number_field("x"), number_field("y"),
                 static_cast<std::uint32_t>(std::max(0.0, number_field("buttons")))};
        if (entry->current) entry->current->cursor(c);
        entry->dirty = true;
        reply.ok();
    } else if (op == "audio") {
        reply.error("audio comes with sound objects later in M3");
    } else if (op == "settings") {
        reply.error("settings come with milestone M6");
    } else {
        reply.error("unknown op '" + op + "'");
    }
}

void Daemon::props(const nlohmann::json& request, ipc::Server::Reply reply) {
    std::string output = request.contains("output") && request["output"].is_string()
                             ? request["output"].get<std::string>()
                             : "";
    Screen* entry = screen(output);
    if (entry == nullptr) return reply.error("props needs an output");
    Wallpaper* wallpaper =
        entry->incoming && entry->incoming->ready() ? entry->incoming.get() : entry->current.get();
    if (wallpaper == nullptr) return reply.error("nothing on that output");
    if (!wallpaper->ready()) return reply.error("still loading");
    std::string path = wallpaper->path().string();

    auto changed_event = [&](const std::vector<std::string>& names) {
        nlohmann::json values = nlohmann::json::object();
        for (const Property& p : wallpaper->properties()) {
            if (std::ranges::find(names, p.name) == names.end()) continue;
            values[p.name] = nlohmann::json::parse(p.value, nullptr, false);
        }
        if (server_ && !names.empty()) {
            server_->broadcast("props", {{"output", output}, {"changed", values}});
        }
        entry->dirty = true;
    };

    if (request.contains("reset") && request["reset"].is_boolean() &&
        request["reset"].get<bool>()) {
        auto names = wallpaper->reset_properties();
        properties_.erase(path);
        save_state();
        changed_event(names);
        return reply.ok({{"changed", names}});
    }
    if (request.contains("set")) {
        if (!request["set"].is_object()) return reply.error("set is an object of name: value");
        // Every value is checked before any is applied: a request fails whole.
        for (const auto& [name, value] : request["set"].items()) {
            std::string why = wallpaper->check_property(name, value.dump());
            if (!why.empty()) {
                return reply.error("cannot set " + name + " to " + value.dump() + ": " + why);
            }
        }
        std::vector<std::string> changed;
        for (const auto& [name, value] : request["set"].items()) {
            auto names = wallpaper->set_property(name, value.dump());
            changed.insert(changed.end(), names.begin(), names.end());
            properties_[path][name] = value;
        }
        save_state();
        changed_event(changed);
        // Every value is a uniform or a visibility; nothing needs a recompile.
        return reply.ok({{"changed", changed}, {"recompiled", nlohmann::json::array()}});
    }
    nlohmann::json list = nlohmann::json::array();
    for (const Property& p : wallpaper->properties()) {
        nlohmann::json item = {{"name", p.name},
                               {"type", p.type},
                               {"text", p.text},
                               {"order", p.order},
                               {"visible", p.visible}};
        item["value"] = nlohmann::json::parse(p.value, nullptr, false);
        item["default"] = nlohmann::json::parse(p.def, nullptr, false);
        auto extra = nlohmann::json::parse(p.extra, nullptr, false);
        if (extra.is_object()) {
            for (const auto& [k, v] : extra.items()) item[k] = v;
        }
        list.push_back(item);
    }
    reply.ok(list);
}

}  // namespace glint

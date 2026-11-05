#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/capture.hpp"
#include "core/fd.hpp"
#include "core/loop.hpp"
#include "core/state.hpp"
#include "core/worker.hpp"
#include "gpu/blit.hpp"
#include "gpu/context.hpp"
#include "gpu/pictures.hpp"
#include "gpu/staging.hpp"
#include "gpu/surface.hpp"
#include "gpu/video_pass.hpp"
#include "input/hyprland.hpp"
#include "ipc/server.hpp"
#include "wallpaper/image/textures.hpp"
#include "wallpaper/scene/scene_wallpaper.hpp"
#include "wallpaper/video/source.hpp"
#include "wallpaper/wallpaper.hpp"
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

    struct SetResult {
        std::string error;  // empty on success
        std::string kind;
        double load_ms = 0;  // request to first frame with the new wallpaper on screen
    };
    using SetDone = std::function<void(const SetResult&)>;
    // output is a connector name or "*". done runs when every affected output shows the new
    // wallpaper (or is paused and has it ready), or with an error.
    void set(std::string_view output, const std::filesystem::path& path, const Options& options,
             SetDone done);

    // wl::Display::Listener
    void output_added(wl::Output& output) override;
    void output_removed(wl::Output& output) override;
    void output_resized(wl::Output& output) override;
    void output_frame(wl::Output& output, std::uint32_t time_ms) override;
    void output_covered(wl::Output& output, bool covered) override;
    void pointer(wl::Output& output, double x, double y, std::uint32_t buttons) override;

private:
    using Assignment = state::Entry;

    // A set request that spans one or more outputs.
    struct Request {
        SetDone done;
        std::int64_t start_ns = 0;
        int remaining = 0;
        std::string kind;
        std::string error;
        bool finished = false;
        // The assignments before this set, put back if it fails and nothing changed since.
        std::optional<Assignment> previous_all;
        std::map<std::string, Assignment> previous_outputs;
        std::uint64_t generation = 0;
    };

    // One per output with a configured layer surface.
    struct Screen {
        wl::Output* output = nullptr;
        std::unique_ptr<gpu::Surface> surface;

        std::unique_ptr<Wallpaper> current;   // on screen
        std::unique_ptr<Wallpaper> incoming;  // loading, or ready and fading in
        std::shared_ptr<Request> request;     // the set that brought incoming
        std::int64_t fade_start_ns = 0;       // 0: incoming not ready yet
        int fade_ms = 0;                      // crossfade length, 0 for a straight swap
        std::int64_t current_origin_ns = 0;   // when the current wallpaper's clock started
        std::int64_t incoming_origin_ns = 0;
        std::int64_t paused_at_ns = 0;
        std::int64_t last_frame_ns = 0;
        std::string error;  // last load error, for status

        bool dirty = true;         // something changed that is not on screen yet
        bool resized = true;       // the swapchain needs the new size
        std::uint32_t paused = 0;  // this output's own reasons; see reasons()

        // Statistics for status.
        std::uint64_t frames = 0;
        std::array<std::int64_t, 64> present_ns{};  // ring of recent present times
        std::int64_t last_present_ns = 0;
        double cpu_ms = 0;
        double load_ms = 0;
        double pointer[2] = {0, 0};
        std::string last_state;  // what the last "state" event said
    };

    bool setup_signals();
    Screen* screen(wl::Output& output);
    Screen* screen(std::string_view name);
    std::uint32_t reasons(const Screen& screen) const;
    bool drawable(const Screen& screen) const;
    void render(Screen& screen);
    void before_poll();
    void after_poll();
    void pause_changed(Screen& screen, std::uint32_t before);

    const Assignment* assignment(std::string_view name) const;
    void apply(Screen& screen, const Assignment& assignment, std::shared_ptr<Request> request);
    void loaded(Screen& screen, Wallpaper* wallpaper, const std::string& error);
    void finish(const std::shared_ptr<Request>& request, const std::string& error);

    // IPC (docs/IPC.md), the M1 subset.
    void handle(const nlohmann::json& request, ipc::Server::Reply reply);
    std::string unset(std::string_view output);
    std::string pause(std::string_view output, std::uint32_t reason, bool on);
    std::string state_of(const Screen& screen) const;
    void emit_states();
    nlohmann::json output_json(const Screen& screen) const;
    nlohmann::json status_json(Screen& screen);
    void save_state() const;
    void props(const nlohmann::json& request, ipc::Server::Reply reply);

    Loop loop_;
    wl::Display display_;
    std::unique_ptr<gpu::Context> gpu_;
    std::unique_ptr<gpu::StagingRing> staging_;
    std::unique_ptr<gpu::Pictures> pictures_;
    std::unique_ptr<Workers> workers_;
    std::unique_ptr<image::Textures> textures_;
    std::unique_ptr<gpu::Blit> blit_;
    std::unique_ptr<gpu::VideoPass> video_;
    std::unique_ptr<scene::SceneCache> scenes_;
    std::unique_ptr<video::Sources> videos_;
    std::unique_ptr<Services> services_;
    std::vector<std::unique_ptr<Screen>> screens_;

    std::optional<Assignment> all_;                     // "*"
    std::map<std::string, Assignment> outputs_;         // by connector name; wins over "*"
    std::map<std::string, nlohmann::json> properties_;  // scene settings by project path
    std::uint64_t generation_ = 0;                      // bumped on every change of the assignments
    std::uint32_t paused_all_ = 0;  // reasons set for "*", on every output including new ones
    std::unique_ptr<ipc::Server> server_;
    audio::Capture capture_;
    input::HyprlandCursor hyprland_;  // runs only while a shown, running scene wants audio
    bool force_toplevels_ = false;    // GLINT_TRACK_TOPLEVELS=1

    UniqueFd signal_fd_;
    int wayland_id_ = 0;
};

}  // namespace glint

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "core/fd.hpp"
#include "wallpaper/wallpaper.hpp"

namespace sdbus {
class IConnection;
class IProxy;
class Variant;
}  // namespace sdbus

namespace glint {
class Loop;
class Workers;
namespace gpu {
class Context;
class StagingRing;
}  // namespace gpu
}  // namespace glint

// What is playing, from the media players on the session bus (MPRIS, org.mpris.MediaPlayer2):
// title, artist, album, playback state, position, and the cover as a texture with the
// colours scripts ask for. One client for the daemon, started while a shown scene wants
// media and stopped when none does. The bus is polled from the daemon's loop.
namespace glint::media {

class Mpris {
public:
    Mpris(Loop& loop, Workers& workers, gpu::Context& gpu, gpu::StagingRing& staging);
    ~Mpris();
    Mpris(const Mpris&) = delete;
    Mpris& operator=(const Mpris&) = delete;

    // Connects to the session bus and finds the players. Returns an error message or empty.
    std::string start();
    void stop();
    bool running() const { return connection_ != nullptr; }
    // Refresh all sdbus poll descriptors and its absolute timeout before each poll.
    void prepare_poll();

    // The state with the position brought up to now.
    const MediaState& state();
    // Called on the main thread whenever a version changed.
    void set_listener(std::function<void()> listener) { listener_ = std::move(listener); }

private:
    struct Player;
    void refresh_players();
    void add_player(const std::string& name);
    void remove_player(const std::string& name);
    void read_player(Player& player, bool metadata, bool status, bool position);
    void update_metadata(Player& player, const sdbus::Variant& value);
    void update_playback(Player& player, const std::string& value);
    void choose_active();
    void apply_active();
    void fetch_thumbnail(const std::string& url);
    void clear_thumbnail();
    void bump();
    void poll_data();
    void process_events();

    Loop& loop_;
    Workers& workers_;
    gpu::Context& gpu_;
    gpu::StagingRing& staging_;
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IProxy> bus_;
    std::map<std::string, std::unique_ptr<Player>> players_;
    std::string active_;
    int loop_id_ = 0;
    int wake_id_ = 0, timer_id_ = 0;
    int bus_fd_ = -1, wake_fd_ = -1;
    UniqueFd timer_;
    std::int64_t armed_timeout_ = -1;
    std::function<void()> listener_;
    MediaState state_;
    std::uint64_t version_ = 1;
    double position_base_ = 0;  // seconds, at position_time_
    double position_time_ = 0;  // monotonic seconds
    std::string art_url_;       // of the thumbnail shown or being fetched
    std::string track_key_;
    std::uint64_t fetch_serial_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace glint::media

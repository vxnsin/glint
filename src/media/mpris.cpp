#include "media/mpris.hpp"

#include <sdbus-c++/sdbus-c++.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/loop.hpp"
#include "core/worker.hpp"
#include "gpu/texture.hpp"
#include "media/state.hpp"
#include "wallpaper/image/decode.hpp"

namespace glint::media {

namespace {

constexpr const char* mpris_prefix = "org.mpris.MediaPlayer2.";
constexpr const char* player_path = "/org/mpris/MediaPlayer2";
constexpr const char* player_iface = "org.mpris.MediaPlayer2.Player";
constexpr const char* properties_iface = "org.freedesktop.DBus.Properties";
std::atomic<std::uint64_t> next_generation{1};

double now_seconds() {
    return static_cast<double>(clock::now_ns()) / 1e9;
}

std::string variant_string(const sdbus::Variant& v) {
    if (v.containsValueOfType<std::string>()) return v.get<std::string>();
    if (v.containsValueOfType<std::vector<std::string>>()) {
        std::string out;
        for (const std::string& s : v.get<std::vector<std::string>>()) {
            if (!out.empty()) out += ", ";
            out += s;
        }
        return out;
    }
    if (v.containsValueOfType<sdbus::ObjectPath>()) return v.get<sdbus::ObjectPath>();
    return {};
}

double variant_number(const sdbus::Variant& v) {
    if (v.containsValueOfType<std::int64_t>()) return static_cast<double>(v.get<std::int64_t>());
    if (v.containsValueOfType<std::uint64_t>()) return static_cast<double>(v.get<std::uint64_t>());
    if (v.containsValueOfType<std::int32_t>()) return v.get<std::int32_t>();
    if (v.containsValueOfType<std::uint32_t>()) return v.get<std::uint32_t>();
    if (v.containsValueOfType<double>()) return v.get<double>();
    return 0;
}

// The colours of a cover: the three most common hues of some saturation, a text colour
// that reads on the first, and black or white, whichever contrasts more with it.
struct Colours {
    float primary[3], secondary[3], tertiary[3], text[3], high_contrast[3];
};

float luminance(const float* c) {
    auto lin = [](float v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c[0]) + 0.7152f * lin(c[1]) + 0.0722f * lin(c[2]);
}

float contrast(const float* a, const float* b) {
    float la = luminance(a), lb = luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05f) / (lb + 0.05f);
}

Colours colours_of(const std::byte* rgba, std::uint32_t width, std::uint32_t height) {
    // 4 bits per channel: 4096 bins, counted over a thinned grid of pixels.
    std::array<std::uint32_t, 4096> bins{};
    std::uint32_t step = std::max<std::uint32_t>(1, std::max(width, height) / 128);
    for (std::uint32_t y = 0; y < height; y += step) {
        for (std::uint32_t x = 0; x < width; x += step) {
            const auto* p = reinterpret_cast<const unsigned char*>(rgba) +
                            (static_cast<std::size_t>(y) * width + x) * 4;
            if (p[3] < 128) continue;
            ++bins[static_cast<std::size_t>((p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4))];
        }
    }
    struct Bin {
        float c[3];
        float score;
    };
    std::vector<Bin> ranked;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        if (bins[i] == 0) continue;
        Bin b;
        b.c[0] = (static_cast<float>(i >> 8 & 15) + 0.5f) / 16;
        b.c[1] = (static_cast<float>(i >> 4 & 15) + 0.5f) / 16;
        b.c[2] = (static_cast<float>(i & 15) + 0.5f) / 16;
        float mx = std::max({b.c[0], b.c[1], b.c[2]}), mn = std::min({b.c[0], b.c[1], b.c[2]});
        float saturation = mx > 0 ? (mx - mn) / mx : 0;
        // Common colours first, the saturated ones ahead of greys.
        b.score = static_cast<float>(bins[i]) * (0.25f + saturation);
        ranked.push_back(b);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Bin& a, const Bin& b) { return a.score > b.score; });
    Colours out{};
    float* slots[3] = {out.primary, out.secondary, out.tertiary};
    int filled = 0;
    for (const Bin& b : ranked) {
        bool distinct = true;
        for (int k = 0; k < filled; ++k) {
            float d = std::fabs(b.c[0] - slots[k][0]) + std::fabs(b.c[1] - slots[k][1]) +
                      std::fabs(b.c[2] - slots[k][2]);
            if (d < 0.35f) distinct = false;
        }
        if (!distinct) continue;
        std::copy_n(b.c, 3, slots[filled++]);
        if (filled == 3) break;
    }
    for (int k = filled; k < 3; ++k)
        std::copy_n(filled > 0 ? out.primary : out.primary, 3, slots[k]);
    float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
    const float* hc = contrast(out.primary, white) >= contrast(out.primary, black) ? white : black;
    std::copy_n(hc, 3, out.high_contrast);
    const float* text = hc;
    if (contrast(out.primary, out.secondary) >= 3) {
        text = out.secondary;
    } else if (contrast(out.primary, out.tertiary) >= 3) {
        text = out.tertiary;
    }
    std::copy_n(text, 3, out.text);
    return out;
}

}  // namespace

struct Mpris::Player {
    std::string name;
    std::string track_id;
    std::unique_ptr<sdbus::IProxy> proxy;
    std::string title, artist, album, album_artist, art_url;
    double duration = 0;
    int playback = 0;
    double position = 0;  // at position_time
    double position_time = 0;
    double last_playing = 0;  // when it was last seen playing
};

Mpris::Mpris(Loop& loop, Workers& workers, gpu::Context& gpu, gpu::StagingRing& staging)
    : loop_(loop), workers_(workers), gpu_(gpu), staging_(staging) {}

Mpris::~Mpris() {
    *alive_ = false;
    stop();
}

std::string Mpris::start() {
    if (connection_) return {};
    state_.source_generation = next_generation.fetch_add(1);
    try {
        connection_ = sdbus::createSessionBusConnection();
        bus_ = sdbus::createProxy(*connection_, sdbus::ServiceName{"org.freedesktop.DBus"},
                                  sdbus::ObjectPath{"/org/freedesktop/DBus"});
        bus_->uponSignal("NameOwnerChanged")
            .onInterface("org.freedesktop.DBus")
            .call([this](const std::string& name, const std::string& old_owner,
                         const std::string& new_owner) {
                if (!name.starts_with(mpris_prefix)) return;
                if (!old_owner.empty()) remove_player(name);
                if (!new_owner.empty()) add_player(name);
                choose_active();
            });
        refresh_players();
    } catch (const sdbus::Error& e) {
        stop();
        return std::string("session bus: ") + e.what();
    }
    timer_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK));
    if (timer_.get() < 0) {
        stop();
        return "cannot create media bus timeout timer";
    }
    timer_id_ = loop_.add(timer_.get(), POLLIN, [this](short) {
        std::uint64_t count = 0;
        [[maybe_unused]] auto n = ::read(timer_.get(), &count, sizeof(count));
        armed_timeout_ = -1;
        process_events();
    });
    poll_data();
    choose_active();
    return {};
}

void Mpris::poll_data() {
    if (!connection_) return;
    sdbus::IConnection::PollData pd = connection_->getEventLoopPollData();
    if (pd.fd != bus_fd_) {
        if (loop_id_ != 0) loop_.remove(loop_id_);
        bus_fd_ = pd.fd;
        loop_id_ =
            pd.fd >= 0 ? loop_.add(pd.fd, pd.events, [this](short) { process_events(); }) : 0;
    } else if (loop_id_ != 0) {
        loop_.modify(loop_id_, pd.events);
    }
    if (pd.eventFd != wake_fd_) {
        if (wake_id_ != 0) loop_.remove(wake_id_);
        wake_fd_ = pd.eventFd;
        // sdbus consumes its own eventfd when processing pending events.
        wake_id_ = pd.eventFd >= 0
                       ? loop_.add(pd.eventFd, POLLIN, [this](short) { process_events(); })
                       : 0;
    }
    const auto timeout = pd.timeout.count();
    if (timer_ && timeout != armed_timeout_) {
        itimerspec spec{};
        if (pd.timeout != std::chrono::microseconds::max()) {
            const auto us = std::max<std::int64_t>(timeout, 1);
            spec.it_value.tv_sec = static_cast<time_t>(us / 1'000'000);
            spec.it_value.tv_nsec = static_cast<long>((us % 1'000'000) * 1000);
        }
        ::timerfd_settime(timer_.get(), TFD_TIMER_ABSTIME, &spec, nullptr);
        armed_timeout_ = timeout;
    }
}

void Mpris::prepare_poll() {
    if (!connection_) return;
    try {
        poll_data();
    } catch (const sdbus::Error& e) {
        log::warn("media poll: {}", e.what());
    }
}

void Mpris::process_events() {
    try {
        while (connection_ && connection_->processPendingEvent()) {
        }
        poll_data();
    } catch (const sdbus::Error& e) {
        log::warn("media: {}", e.what());
    }
}

void Mpris::stop() {
    ++fetch_serial_;
    if (loop_id_ != 0) loop_.remove(loop_id_);
    if (wake_id_ != 0) loop_.remove(wake_id_);
    if (timer_id_ != 0) loop_.remove(timer_id_);
    loop_id_ = 0;
    wake_id_ = timer_id_ = 0;
    bus_fd_ = wake_fd_ = -1;
    timer_.reset();
    armed_timeout_ = -1;
    players_.clear();
    bus_.reset();
    connection_.reset();
    active_.clear();
    MediaState fresh;
    fresh.source_generation = state_.source_generation;
    fresh.properties_version = state_.properties_version + 1;
    fresh.playback_version = state_.playback_version + 1;
    fresh.thumbnail_version = state_.thumbnail_version + 1;
    state_ = fresh;
    art_url_.clear();
    track_key_.clear();
}

void Mpris::refresh_players() {
    std::vector<std::string> names;
    bus_->callMethod("ListNames").onInterface("org.freedesktop.DBus").storeResultsTo(names);
    for (const std::string& name : names) {
        if (name.starts_with(mpris_prefix)) add_player(name);
    }
}

void Mpris::add_player(const std::string& name) {
    if (players_.contains(name)) return;
    auto player = std::make_unique<Player>();
    player->name = name;
    try {
        player->proxy = sdbus::createProxy(*connection_, sdbus::ServiceName{name},
                                           sdbus::ObjectPath{player_path});
        Player* raw = player.get();
        player->proxy->uponSignal("PropertiesChanged")
            .onInterface(properties_iface)
            .call([this, raw](const std::string& iface,
                              const std::map<std::string, sdbus::Variant>& changed,
                              const std::vector<std::string>& invalidated) {
                if (iface != player_iface) return;
                // Use the signal's values directly. Re-reading them synchronously can
                // block the rendering thread and can observe a different update.
                if (auto m = changed.find("Metadata"); m != changed.end())
                    update_metadata(*raw, m->second);
                if (auto s = changed.find("PlaybackStatus"); s != changed.end())
                    update_playback(*raw, variant_string(s->second));
                auto invalid = [&](const char* property) {
                    return !changed.contains(property) &&
                           std::ranges::find(invalidated, property) != invalidated.end();
                };
                const bool metadata = invalid("Metadata"), status = invalid("PlaybackStatus");
                if (metadata || status) read_player(*raw, metadata, status, false);
                if (auto p = changed.find("Position"); p != changed.end()) {
                    raw->position = variant_number(p->second) / 1e6;
                    raw->position_time = now_seconds();
                }
                choose_active();
            });
        player->proxy->uponSignal("Seeked")
            .onInterface(player_iface)
            .call([this, raw](std::int64_t position) {
                raw->position = static_cast<double>(position) / 1e6;
                raw->position_time = now_seconds();
                if (raw->name == active_) apply_active();
            });
        read_player(*player, true, true, true);
    } catch (const sdbus::Error& e) {
        log::debug("media: {}: {}", name, e.what());
        return;
    }
    log::info("media: player {}", name.substr(std::strlen(mpris_prefix)));
    players_[name] = std::move(player);
}

void Mpris::remove_player(const std::string& name) {
    if (name == active_) track_key_.clear();
    if (players_.erase(name) > 0) {
        log::info("media: player {} gone", name.substr(std::strlen(mpris_prefix)));
    }
}

void Mpris::update_metadata(Player& player, const sdbus::Variant& value) {
    using Metadata = std::map<std::string, sdbus::Variant>;
    if (!value.containsValueOfType<Metadata>()) return;
    const auto map = value.get<Metadata>();
    auto text = [&](const char* key) {
        auto it = map.find(key);
        return it != map.end() ? variant_string(it->second) : std::string();
    };
    const auto title = text("xesam:title"), artist = text("xesam:artist");
    const auto track = text("mpris:trackid");
    if (track != player.track_id || title != player.title || artist != player.artist) {
        player.position = 0;
        player.position_time = now_seconds();
    }
    player.track_id = track;
    player.title = title;
    player.artist = artist;
    player.album = text("xesam:album");
    player.album_artist = text("xesam:albumArtist");
    player.art_url = text("mpris:artUrl");
    auto len = map.find("mpris:length");
    player.duration = len != map.end() ? variant_number(len->second) / 1e6 : 0;
}

void Mpris::update_playback(Player& player, const std::string& value) {
    const auto now = now_seconds();
    if (player.playback == 1) player.position += now - player.position_time;
    player.position_time = now;
    const int playback = value == "Playing" ? 1 : value == "Paused" ? 2 : 0;
    if (playback == 1 && player.playback != 1) player.last_playing = now;
    player.playback = playback;
}

void Mpris::read_player(Player& player, bool metadata, bool status, bool position) {
    try {
        if (metadata) {
            sdbus::Variant v = player.proxy->getProperty("Metadata").onInterface(player_iface);
            update_metadata(player, v);
        }
        if (status) {
            sdbus::Variant v =
                player.proxy->getProperty("PlaybackStatus").onInterface(player_iface);
            update_playback(player, variant_string(v));
        }
        if (position) {
            sdbus::Variant v = player.proxy->getProperty("Position").onInterface(player_iface);
            player.position = variant_number(v) / 1e6;
            player.position_time = now_seconds();
        }
    } catch (const sdbus::Error& e) {
        log::debug("media: {}: {}", player.name, e.what());
    }
}

// The player that is playing, else the one that played last.
void Mpris::choose_active() {
    std::vector<PlayerChoice> choices;
    for (const auto& [name, p] : players_) choices.push_back({name, p->playback, p->last_playing});
    active_ = select_player(choices, active_);
    apply_active();
}

void Mpris::apply_active() {
    auto it = players_.find(active_);
    if (it == players_.end()) {
        if (state_.available) {
            state_.available = false;
            state_.playback = 0;
            state_.title.clear();
            state_.artist.clear();
            state_.album.clear();
            state_.album_artist.clear();
            state_.content_type.clear();
            state_.position = state_.duration = 0;
            position_base_ = position_time_ = 0;
            ++state_.properties_version;
            ++state_.playback_version;
            art_url_.clear();
            track_key_.clear();
            fetch_thumbnail("");  // invalidate an in-flight cover from the removed player
            bump();
        }
        return;
    }
    const Player& p = *it->second;
    const auto key = p.name + "\n" + p.track_id + "\n" + p.title + "\n" + p.artist + "\n" + p.album;
    const bool track_changed = key != track_key_;
    if (track_changed)
        log::info("media: track updated from {}", p.name.substr(std::strlen(mpris_prefix)));
    bool changed = false;
    if (!state_.available || state_.title != p.title || state_.artist != p.artist ||
        state_.album != p.album || state_.album_artist != p.album_artist ||
        state_.duration != p.duration || track_changed) {
        state_.available = true;
        state_.title = p.title;
        state_.artist = p.artist;
        state_.album = p.album;
        state_.album_artist = p.album_artist;
        state_.content_type = "music";
        state_.duration = p.duration;
        ++state_.properties_version;
        changed = true;
    }
    if (state_.playback != p.playback) {
        state_.playback = p.playback;
        ++state_.playback_version;
        changed = true;
    }
    position_base_ = p.position;
    position_time_ = p.position_time;
    if (p.art_url != art_url_ || track_changed) {
        art_url_ = p.art_url;
        track_key_ = key;
        fetch_thumbnail(p.art_url);
    }
    if (changed) bump();
}

void Mpris::fetch_thumbnail(const std::string& url) {
    std::uint64_t serial = ++fetch_serial_;
    if (url.empty()) {
        clear_thumbnail();
        return;
    }
    std::string path = url.starts_with("file://") ? url.substr(7) : url;
    auto decoded = std::make_shared<image::Decoded>();
    auto error = std::make_shared<std::string>();
    auto colours = std::make_shared<Colours>();
    std::weak_ptr<bool> alive = alive_;
    gpu::StagingRing& staging = staging_;
    workers_.post(
        [path, decoded, error, colours, &staging] {
            *error = image::decode(path, 512, staging, *decoded);
            if (error->empty() && decoded->pixels.data != nullptr) {
                *colours = colours_of(decoded->pixels.data, decoded->width, decoded->height);
            }
        },
        [this, alive, serial, decoded, error, colours, url, &staging] {
            if (alive.expired() || serial != fetch_serial_) {
                staging.abandon(decoded->pixels);
                return;
            }
            if (!error->empty()) {
                staging.abandon(decoded->pixels);
                clear_thumbnail();
                log::warn("media: cover {}: {}", url, *error);
                return;
            }
            state_.thumbnail = gpu::Texture::upload_rgba(gpu_, staging_, decoded->pixels,
                                                         decoded->width, decoded->height);
            state_.has_thumbnail = state_.thumbnail != nullptr;
            state_.thumbnail_width = decoded->width;
            state_.thumbnail_height = decoded->height;
            std::copy_n(colours->primary, 3, state_.primary);
            std::copy_n(colours->secondary, 3, state_.secondary);
            std::copy_n(colours->tertiary, 3, state_.tertiary);
            std::copy_n(colours->text, 3, state_.text_color);
            std::copy_n(colours->high_contrast, 3, state_.high_contrast);
            ++state_.thumbnail_version;
            bump();
        });
}

void Mpris::clear_thumbnail() {
    state_.has_thumbnail = false;
    state_.thumbnail.reset();
    state_.thumbnail_width = state_.thumbnail_height = 0;
    for (float* color : {state_.primary, state_.secondary, state_.tertiary})
        std::fill_n(color, 3, 0.0f);
    std::fill_n(state_.text_color, 3, 1.0f);
    std::fill_n(state_.high_contrast, 3, 1.0f);
    ++state_.thumbnail_version;
    bump();
}

void Mpris::bump() {
    ++version_;
    if (listener_) listener_();
}

const MediaState& Mpris::state() {
    if (state_.playback == 1) {
        state_.position = position_base_ + (now_seconds() - position_time_);
        if (state_.duration > 0) state_.position = std::min(state_.position, state_.duration);
    } else {
        state_.position = position_base_;
    }
    return state_;
}

}  // namespace glint::media

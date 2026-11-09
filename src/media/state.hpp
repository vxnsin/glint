#pragma once

#include <span>
#include <string>
#include <string_view>
#include <tuple>

#include "wallpaper/wallpaper.hpp"

namespace glint::media {

struct PlayerChoice {
    std::string_view name;
    int playback = 0;
    double last_playing = 0;
};

inline std::string select_player(std::span<const PlayerChoice> players, std::string_view current) {
    const PlayerChoice* best = nullptr;
    auto rank = [current](const PlayerChoice& p) {
        return std::tuple{p.playback == 1, p.last_playing, p.name == current, p.playback != 0};
    };
    for (const auto& p : players)
        if (!best || rank(p) > rank(*best)) best = &p;
    return best ? std::string(best->name) : std::string{};
}

struct Updates {
    bool status = false, properties = false, playback = false, thumbnail = false, timeline = false;
};

inline bool thumbnail_changed(const MediaState& before, const MediaState& after) {
    return before.source_generation != after.source_generation ||
           before.thumbnail_version != after.thumbnail_version ||
           before.has_thumbnail != after.has_thumbnail || before.thumbnail != after.thumbnail ||
           before.thumbnail_width != after.thumbnail_width ||
           before.thumbnail_height != after.thumbnail_height;
}

// A reconnect may reuse local version numbers; it still needs fresh events and textures.
class StateTracker {
public:
    Updates consume(const MediaState& state) {
        const bool fresh = first_ || state.source_generation != previous_.source_generation;
        Updates updates;
        updates.status = fresh;
        updates.properties = fresh || state.properties_version != previous_.properties_version ||
                             state.available != previous_.available;
        updates.playback = fresh || state.playback_version != previous_.playback_version ||
                           state.playback != previous_.playback;
        updates.thumbnail = fresh || thumbnail_changed(previous_, state);
        updates.timeline =
            state.available && (state.playback == 1 || updates.properties || updates.playback ||
                                state.position != previous_.position);
        previous_ = state;
        first_ = false;
        return updates;
    }

private:
    bool first_ = true;
    MediaState previous_;
};

}  // namespace glint::media

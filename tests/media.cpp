#include <array>
#include <cstdio>

#include "media/state.hpp"

namespace {
int failures = 0;
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            ++failures;                                     \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x); \
        }                                                   \
    } while (false)

void selection() {
    using glint::media::PlayerChoice;
    using glint::media::select_player;
    CHECK(select_player({}, "").empty());
    std::array players{PlayerChoice{"browser", 1, 10}, PlayerChoice{"spotify", 1, 20}};
    CHECK(select_player(players, "browser") == "spotify");
    CHECK(select_player(players, "spotify") == "spotify");
    std::swap(players[0], players[1]);
    CHECK(select_player(players, "browser") == "spotify");
    players[0].last_playing = players[1].last_playing;
    CHECK(select_player(players, "browser") == "browser");
    players[0].playback = 2;
    players[0].last_playing = 100;
    CHECK(select_player(players, "spotify") == "browser");
    players[1].playback = 2;
    CHECK(select_player(players, "browser") == "spotify");
}

void changes() {
    glint::MediaState state;
    state.source_generation = 1;
    state.available = true;
    state.playback = 1;
    state.properties_version = state.playback_version = state.thumbnail_version = 1;
    glint::media::StateTracker tracker;
    auto u = tracker.consume(state);
    CHECK(u.status && u.properties && u.playback && u.thumbnail && u.timeline);
    u = tracker.consume(state);
    CHECK(!u.status && !u.properties && !u.playback && !u.thumbnail && u.timeline);
    state.title = "New song";
    ++state.properties_version;
    u = tracker.consume(state);
    CHECK(u.properties && !u.playback && !u.thumbnail);
    // Cover availability must invalidate the texture even with a reused version number.
    state.has_thumbnail = true;
    state.thumbnail_width = state.thumbnail_height = 512;
    u = tracker.consume(state);
    CHECK(u.thumbnail && !u.properties);
    state.playback = 2;
    ++state.playback_version;
    state.position = 10;
    CHECK(tracker.consume(state).timeline);
    CHECK(!tracker.consume(state).timeline);
    state.position = 20;  // a seek while paused
    CHECK(tracker.consume(state).timeline);
    // Client restart: its counters can coincide with those seen before the restart.
    state.source_generation = 2;
    u = tracker.consume(state);
    CHECK(u.status && u.properties && u.playback && u.thumbnail);
    state.available = false;
    state.has_thumbnail = false;
    u = tracker.consume(state);
    CHECK(u.properties && u.thumbnail && !u.timeline);
}
}  // namespace

int main() {
    selection();
    changes();
    std::printf("media regressions: %d failures\n", failures);
    return failures != 0;
}

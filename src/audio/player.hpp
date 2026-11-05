#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct pw_thread_loop;
struct pw_stream;

// A scene's sound objects, played through one PipeWire output stream. Each track decodes on
// a decoder thread with FFmpeg into a ring of about a second, the stream's process callback
// only mixes from the rings. Nothing exists before start() and after stop().
namespace glint::audio {

struct TrackSpec {
    std::vector<std::span<const std::byte>> files;  // encoded audio, alive while playing
    std::string mode = "loop";                      // loop, random, single
    float volume = 1;
    float min_pause = 1, max_pause = 5;  // random: seconds of silence between plays
    bool playing = true;                 // false: waits for a script to start it
};

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    std::string start(std::vector<TrackSpec> tracks, float volume);
    void stop();
    bool running() const { return loop_ != nullptr; }

    void set_volume(float volume) { volume_.store(volume); }
    // One track under a script: playing or holding, its own volume, or back to its start.
    void set_track(std::size_t index, bool playing, float volume);
    void restart_track(std::size_t index);
    // Paused, the stream is inactive: PipeWire stops calling it, nothing wakes.
    void set_paused(bool paused);

    // PipeWire's process callback, internal.
    void process();

private:
    struct Track;
    void decode();

    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::thread decoder_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_ = false;
    std::atomic<float> volume_ = 0;
    std::atomic<bool> paused_ = false;
};

}  // namespace glint::audio

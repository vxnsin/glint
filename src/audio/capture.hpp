#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>

#include "audio/spectrum.hpp"

struct pw_thread_loop;
struct pw_stream;

// What is playing, as audio-reactive scenes see it: a PipeWire capture stream on the monitor
// of the default output, analysed into the 64-band spectrum (audio/spectrum.hpp) on
// PipeWire's own thread. It exists only while a scene asks for it; stopped, it holds no
// thread and no stream, so it costs nothing.
namespace glint::audio {

struct Levels {
    Bands left{}, right{};
};

class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Returns an error message or empty; a second start is a no-op.
    std::string start();
    void stop();
    bool running() const { return loop_ != nullptr; }

    // The latest bands; silence while stopped. Any thread.
    Levels levels() const;

    // PipeWire's process callback, internal.
    void process();

private:
    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    std::unique_ptr<Spectrum> spectrum_;
    mutable std::mutex mutex_;
    Levels levels_;
};

}  // namespace glint::audio

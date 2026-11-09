#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Video decoding on a thread of its own: demux, decode (VA-API when asked and available,
// software otherwise), convert to planar 8-bit YUV 4:2:0, and keep a few frames ahead. The
// main thread takes the frame due at a time; the stream loops by seeking to the start.
namespace glint::video {

// A VA-API frame exported as a dma-buf: the luma plane and the interleaved chroma plane
// (NV12, or P010 for 10-bit), each with its fd, offset and pitch. The fds live as long as
// hold does; hold also keeps the decoder from reusing the surface.
struct Prime {
    struct Plane {
        int fd = -1;
        std::uint64_t offset = 0;
        std::uint64_t pitch = 0;
    };
    Plane planes[2];
    std::uint64_t modifier = 0;
    std::uint64_t key = 0;  // the buffer's inode: the same surface comes back with a new fd
    bool deep = false;      // 16 bits per sample (P010)
};

struct Picture {
    double time = 0;  // seconds on the video's own timeline, loops counted on
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> y, u, v;  // tightly packed planes, u and v at half size
    std::optional<Prime> prime;         // instead of the planes, without a copy
    std::shared_ptr<void> hold;
};

struct Info {
    std::uint32_t width = 0, height = 0;
    double fps = 0;
    double duration = 0;  // seconds, 0 when unknown
    std::string codec;
    bool hardware = false;  // VA-API decoded
    bool bt709 = true;      // the colour matrix (BT.601 otherwise)
    bool full_range = false;
};

class Decoder {
public:
    ~Decoder();

    // Opens the file and starts the thread. hwdec: "auto", "vaapi" or "no". prime: hand out
    // VA-API frames as dma-bufs (Picture::prime) instead of copying them out.
    static std::unique_ptr<Decoder> open(const std::filesystem::path& path,
                                         const std::string& hwdec, bool prime, std::string& error);

    const Info& info() const { return info_; }

    // The newest frame at or before time, removing older ones; nothing if none is decoded
    // yet. Main thread.
    std::optional<Picture> take(double time);

    // Blocks until the first frame is decoded (or decoding failed); worker thread.
    bool wait_first();

    // Paused, the thread stops decoding ahead and sleeps.
    void set_paused(bool paused);

    // Back to copied planes, for when the dma-bufs cannot be imported. Frames already queued
    // are dropped.
    void stop_prime();
    bool prime() const { return prime_.load(); }

private:
    Decoder() = default;
    void run();
    bool decode_one();  // false at a hard error

    struct State;
    std::unique_ptr<State> state_;
    Info info_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Picture> queue_;
    std::atomic<bool> stopping_ = false;
    std::atomic<bool> paused_ = false;
    std::atomic<bool> prime_ = false;
    bool failed_ = false;
    double loop_offset_ = 0;  // decoder thread: added to pts after each loop
    double last_time_ = 0;
};

}  // namespace glint::video

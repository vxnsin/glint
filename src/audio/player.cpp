#include "audio/player.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

#include "core/log.hpp"

namespace glint::audio {

namespace {

constexpr int rate = 48000;
constexpr int channels = 2;
constexpr std::size_t ring_frames = rate;  // a second per track

void on_process(void* data) {
    static_cast<Player*>(data)->process();
}

const pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = on_process;
    return e;
}();

// An AVIOContext over bytes in memory (a file in scene.pkg).
struct Memory {
    std::span<const std::byte> data;
    std::int64_t pos = 0;

    static int read(void* opaque, std::uint8_t* buf, int size) {
        auto* m = static_cast<Memory*>(opaque);
        std::int64_t left = static_cast<std::int64_t>(m->data.size()) - m->pos;
        if (left <= 0) return AVERROR_EOF;
        int n = static_cast<int>(std::min<std::int64_t>(size, left));
        std::memcpy(buf, m->data.data() + m->pos, static_cast<std::size_t>(n));
        m->pos += n;
        return n;
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto* m = static_cast<Memory*>(opaque);
        auto size = static_cast<std::int64_t>(m->data.size());
        if (whence == AVSEEK_SIZE) return size;
        if ((whence & 3) == SEEK_SET) m->pos = offset;
        if ((whence & 3) == SEEK_CUR) m->pos += offset;
        if ((whence & 3) == SEEK_END) m->pos = size + offset;
        m->pos = std::clamp<std::int64_t>(m->pos, 0, size);
        return m->pos;
    }
};

}  // namespace

struct Player::Track {
    TrackSpec spec;
    // Single producer (the decoder), single consumer (the process callback).
    std::vector<float> ring = std::vector<float>(ring_frames * channels);
    std::atomic<std::size_t> head = 0;  // frames written
    std::atomic<std::size_t> tail = 0;  // frames read
    bool finished = false;              // single: played once

    // Decoder state for the file being played.
    std::size_t file = 0;
    Memory memory;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    int stream = -1;
    std::size_t silence = 0;  // frames of silence still to write (random pauses)

    ~Track() { close(); }

    void close() {
        if (swr != nullptr) swr_free(&swr);
        if (codec != nullptr) avcodec_free_context(&codec);
        if (format != nullptr) avformat_close_input(&format);
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        stream = -1;
    }

    bool open(std::size_t index) {
        close();
        if (index >= spec.files.size()) return false;
        file = index;
        memory = {spec.files[index], 0};
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(32768));
        io = avio_alloc_context(buffer, 32768, 0, &memory, &Memory::read, nullptr, &Memory::seek);
        format = avformat_alloc_context();
        format->pb = io;
        if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0) {
            format = nullptr;
            return false;
        }
        avformat_find_stream_info(format, nullptr);
        const AVCodec* decoder = nullptr;
        stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (stream < 0 || decoder == nullptr) return false;
        codec = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(codec, format->streams[stream]->codecpar);
        if (avcodec_open2(codec, decoder, nullptr) < 0) return false;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, rate, &codec->ch_layout,
                                codec->sample_fmt, codec->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr) < 0) {
            return false;
        }
        return true;
    }

    std::size_t space() const { return ring_frames - (head.load() - tail.load()); }

    void push(const float* frames, std::size_t count) {
        std::size_t h = head.load();
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t at = ((h + i) % ring_frames) * channels;
            ring[at] = frames[i * 2];
            ring[at + 1] = frames[i * 2 + 1];
        }
        head.store(h + count);
    }
};

Player::Player() {
    pw_init(nullptr, nullptr);
}

Player::~Player() {
    stop();
}

std::string Player::start(std::vector<TrackSpec> tracks, float volume) {
    if (loop_ != nullptr) return {};
    if (tracks.empty()) return "no tracks";
    volume_.store(volume);
    stopping_.store(false);
    for (auto& spec : tracks) {
        auto track = std::make_unique<Track>();
        track->spec = std::move(spec);
        tracks_.push_back(std::move(track));
    }
    decoder_ = std::thread([this] { decode(); });

    loop_ = pw_thread_loop_new("glint-sound", nullptr);
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "glint", PW_KEY_APP_NAME, "glint", nullptr);
    if (const char* target = std::getenv("GLINT_AUDIO_SINK"); target != nullptr && *target) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);  // tests
    }
    stream_ =
        pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "glint sound", props, &events, this);
    std::uint8_t buffer[1024];
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = rate;
    info.channels = channels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (stream_ == nullptr ||
        pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0 ||
        pw_thread_loop_start(loop_) < 0) {
        stop();
        return "cannot open a PipeWire playback stream";
    }
    return {};
}

void Player::stop() {
    if (loop_ != nullptr) {
        pw_thread_loop_stop(loop_);
        if (stream_ != nullptr) pw_stream_destroy(stream_);
        pw_thread_loop_destroy(loop_);
        stream_ = nullptr;
        loop_ = nullptr;
    }
    stopping_.store(true);
    wake_.notify_all();
    if (decoder_.joinable()) decoder_.join();
    tracks_.clear();
}

void Player::set_paused(bool paused) {
    if (paused_.exchange(paused) == paused || loop_ == nullptr) return;
    wake_.notify_all();
    pw_thread_loop_lock(loop_);
    pw_stream_set_active(stream_, !paused);
    pw_thread_loop_unlock(loop_);
}

void Player::decode() {
    std::mt19937 random(std::random_device{}());
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> out(8192 * channels);
    for (Track* t : [this] {
             std::vector<Track*> v;
             for (auto& t : tracks_) v.push_back(t.get());
             return v;
         }()) {
        if (!t->open(0)) {
            log::warn("sound: cannot decode a track");
            t->finished = true;
        }
    }
    while (!stopping_.load()) {
        bool busy = false;
        for (auto& owned : tracks_) {
            Track& t = *owned;
            if (t.finished) continue;
            // Fill while there is room for a decoded frame's worth.
            while (t.space() > 8192 && !stopping_.load()) {
                busy = true;
                if (t.silence > 0) {
                    std::size_t n = std::min<std::size_t>(t.silence, 4096);
                    std::fill_n(out.begin(), n * channels, 0.0f);
                    t.push(out.data(), n);
                    t.silence -= n;
                    continue;
                }
                int r = av_read_frame(t.format, packet);
                if (r < 0) {
                    // End of the file: loop it, pick another after a pause, or stop.
                    if (t.spec.mode == "single") {
                        t.finished = true;
                        break;
                    }
                    std::size_t next = t.file;
                    if (t.spec.mode == "random") {
                        std::uniform_real_distribution<float> pause(t.spec.min_pause,
                                                                    t.spec.max_pause);
                        t.silence = static_cast<std::size_t>(pause(random) * rate);
                        std::uniform_int_distribution<std::size_t> pick(0, t.spec.files.size() - 1);
                        next = pick(random);
                    }
                    if (!t.open(next)) {
                        t.finished = true;
                        break;
                    }
                    continue;
                }
                if (packet->stream_index == t.stream && avcodec_send_packet(t.codec, packet) >= 0) {
                    while (avcodec_receive_frame(t.codec, frame) == 0) {
                        int max = static_cast<int>(out.size() / channels);
                        auto* dst = reinterpret_cast<std::uint8_t*>(out.data());
                        int n = swr_convert(t.swr, &dst, max,
                                            const_cast<const std::uint8_t**>(frame->extended_data),
                                            frame->nb_samples);
                        if (n > 0) {
                            std::size_t count =
                                std::min<std::size_t>(static_cast<std::size_t>(n), t.space());
                            t.push(out.data(), count);
                        }
                    }
                }
                av_packet_unref(packet);
            }
        }
        // Rings full: wait until the mixer has taken some. A short sleep, only while sound
        // plays, when PipeWire wakes the process at its quantum anyway.
        std::unique_lock lock(mutex_);
        if (paused_.load()) {
            // Paused: nothing is taken from the rings; sleep until resumed or stopped.
            wake_.wait(lock, [this] { return stopping_.load() || !paused_.load(); });
        } else {
            wake_.wait_for(lock, std::chrono::milliseconds(busy ? 5 : 50),
                           [this] { return stopping_.load(); });
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
}

void Player::process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (b == nullptr) return;
    spa_buffer* buf = b->buffer;
    auto* dst = static_cast<float*>(buf->datas[0].data);
    if (dst == nullptr) {
        pw_stream_queue_buffer(stream_, b);
        return;
    }
    std::uint32_t frames = buf->datas[0].maxsize / (sizeof(float) * channels);
    if (b->requested > 0)
        frames = std::min<std::uint32_t>(frames, static_cast<std::uint32_t>(b->requested));
    std::fill_n(dst, frames * channels, 0.0f);
    float master = paused_.load() ? 0.0f : volume_.load();
    for (auto& owned : tracks_) {
        Track& t = *owned;
        std::size_t tail = t.tail.load();
        std::size_t available = t.head.load() - tail;
        std::size_t n = std::min<std::size_t>(available, frames);
        float gain = master * t.spec.volume;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t at = ((tail + i) % ring_frames) * channels;
            dst[i * 2] += t.ring[at] * gain;
            dst[i * 2 + 1] += t.ring[at + 1] * gain;
        }
        // Paused: the rings stand still, the music continues where it was.
        if (!paused_.load()) t.tail.store(tail + n);
    }
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * channels;
    buf->datas[0].chunk->size = frames * sizeof(float) * channels;
    pw_stream_queue_buffer(stream_, b);
}

}  // namespace glint::audio

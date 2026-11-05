#include "audio/capture.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <cstdlib>
#include <cstring>
#include <span>

namespace glint::audio {

namespace {

constexpr unsigned rate = 48000;
constexpr unsigned channels = 2;

void on_process(void* data) {
    static_cast<Capture*>(data)->process();
}

const pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = on_process;
    return e;
}();

}  // namespace

Capture::Capture() {
    pw_init(nullptr, nullptr);
}

Capture::~Capture() {
    stop();
}

std::string Capture::start() {
    if (loop_ != nullptr) return {};
    SpectrumOptions options;
    options.sample_rate = rate;
    options.channels = channels;
    spectrum_ = std::make_unique<Spectrum>(options);

    loop_ = pw_thread_loop_new("glint-audio", nullptr);
    if (loop_ == nullptr) return "cannot create a PipeWire loop";
    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Music", PW_KEY_STREAM_CAPTURE_SINK, "true",
                          PW_KEY_NODE_NAME, "glint", PW_KEY_APP_NAME, "glint", nullptr);
    // For tests: another sink than the default one, by node name.
    if (const char* target = std::getenv("GLINT_AUDIO_TARGET"); target != nullptr && *target) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
    }
    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_), "glint spectrum", props, &events,
                                   this);
    if (stream_ == nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return "cannot create a PipeWire stream";
    }

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
    if (pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_stream_destroy(stream_);
        pw_thread_loop_destroy(loop_);
        stream_ = nullptr;
        loop_ = nullptr;
        return "cannot connect to PipeWire";
    }
    if (pw_thread_loop_start(loop_) < 0) {
        stop();
        return "cannot start the PipeWire thread";
    }
    return {};
}

void Capture::stop() {
    if (loop_ == nullptr) return;
    pw_thread_loop_stop(loop_);
    if (stream_ != nullptr) pw_stream_destroy(stream_);
    pw_thread_loop_destroy(loop_);
    stream_ = nullptr;
    loop_ = nullptr;
    std::lock_guard lock(mutex_);
    levels_ = {};
}

void Capture::process() {
    pw_buffer* b = pw_stream_dequeue_buffer(stream_);
    if (b == nullptr) return;
    spa_buffer* buf = b->buffer;
    if (buf->n_datas > 0 && buf->datas[0].data != nullptr && buf->datas[0].chunk != nullptr) {
        const auto* bytes = static_cast<const std::byte*>(buf->datas[0].data);
        std::uint32_t offset = buf->datas[0].chunk->offset;
        std::uint32_t size = buf->datas[0].chunk->size;
        if (offset + size <= buf->datas[0].maxsize) {
            std::span<const float> samples(reinterpret_cast<const float*>(bytes + offset),
                                           size / sizeof(float));
            if (spectrum_->feed(samples)) {
                std::lock_guard lock(mutex_);
                levels_.left = spectrum_->left();
                levels_.right = spectrum_->right();
            }
        }
    }
    pw_stream_queue_buffer(stream_, b);
}

Levels Capture::levels() const {
    std::lock_guard lock(mutex_);
    return levels_;
}

}  // namespace glint::audio

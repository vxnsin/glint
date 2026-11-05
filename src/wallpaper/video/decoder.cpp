#include "wallpaper/video/decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <sys/stat.h>

#include <cstring>

#include "core/log.hpp"

namespace glint::video {

namespace {

constexpr std::size_t queue_depth = 4;

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b) << 8 |
           static_cast<std::uint32_t>(c) << 16 | static_cast<std::uint32_t>(d) << 24;
}
constexpr std::uint32_t drm_r8 = fourcc('R', '8', ' ', ' ');
constexpr std::uint32_t drm_gr88 = fourcc('G', 'R', '8', '8');
constexpr std::uint32_t drm_r16 = fourcc('R', '1', '6', ' ');
constexpr std::uint32_t drm_gr1616 = fourcc('G', 'R', '3', '2');
constexpr std::uint32_t drm_nv12 = fourcc('N', 'V', '1', '2');
constexpr std::uint32_t drm_p010 = fourcc('P', '0', '1', '0');

// The two planes out of the descriptor, whether the exporter wrote them as separate layers
// (R8 + GR88) or as one layer with two planes (NV12). False for anything else.
bool describe(const AVDRMFrameDescriptor& d, Prime& prime) {
    struct Found {
        const AVDRMPlaneDescriptor* plane;
        std::uint32_t format;
    };
    Found found[2];
    int count = 0;
    for (int l = 0; l < d.nb_layers; ++l) {
        const AVDRMLayerDescriptor& layer = d.layers[l];
        for (int p = 0; p < layer.nb_planes; ++p) {
            if (count == 2) return false;
            found[count++] = {&layer.planes[p], layer.format};
        }
    }
    if (count != 2) return false;
    std::uint32_t f0 = found[0].format, f1 = found[1].format;
    if ((f0 == drm_r8 && f1 == drm_gr88) || (f0 == drm_nv12 && f1 == drm_nv12)) {
        prime.deep = false;
    } else if ((f0 == drm_r16 && f1 == drm_gr1616) || (f0 == drm_p010 && f1 == drm_p010)) {
        prime.deep = true;
    } else {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const AVDRMPlaneDescriptor& plane = *found[i].plane;
        if (plane.object_index < 0 || plane.object_index >= d.nb_objects) return false;
        const AVDRMObjectDescriptor& object = d.objects[plane.object_index];
        if (i > 0 && object.format_modifier != prime.modifier) return false;
        prime.modifier = object.format_modifier;
        prime.planes[i] = {object.fd, static_cast<std::uint64_t>(plane.offset),
                           static_cast<std::uint64_t>(plane.pitch)};
    }
    struct stat st{};
    if (::fstat(prime.planes[0].fd, &st) != 0) return false;
    prime.key = static_cast<std::uint64_t>(st.st_ino);
    return true;
}

AVPixelFormat pick_vaapi(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_VAAPI) return *f;
    }
    return formats[0];
}

}  // namespace

struct Decoder::State {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVBufferRef* device = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* software = nullptr;
    int stream = -1;
    double time_base = 0;
    double frame_duration = 1.0 / 30;
    double last_pts = 0;

    ~State() {
        if (sws != nullptr) sws_freeContext(sws);
        if (software != nullptr) av_frame_free(&software);
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (codec != nullptr) avcodec_free_context(&codec);
        if (device != nullptr) av_buffer_unref(&device);
        if (format != nullptr) avformat_close_input(&format);
    }
};

Decoder::~Decoder() {
    stopping_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

std::unique_ptr<Decoder> Decoder::open(const std::filesystem::path& path, const std::string& hwdec,
                                       bool prime, std::string& error) {
    av_log_set_level(AV_LOG_ERROR);
    std::unique_ptr<Decoder> self(new Decoder());
    self->state_ = std::make_unique<State>();
    State& s = *self->state_;
    if (avformat_open_input(&s.format, path.c_str(), nullptr, nullptr) < 0) {
        error = "cannot open the video";
        return nullptr;
    }
    avformat_find_stream_info(s.format, nullptr);
    const AVCodec* codec = nullptr;
    s.stream = av_find_best_stream(s.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (s.stream < 0 || codec == nullptr) {
        error = "no video stream";
        return nullptr;
    }
    AVStream* stream = s.format->streams[s.stream];
    s.codec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(s.codec, stream->codecpar);
    s.codec->thread_count = 0;

    Info& info = self->info_;
    if (hwdec != "no" &&
        av_hwdevice_ctx_create(&s.device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) == 0) {
        s.codec->hw_device_ctx = av_buffer_ref(s.device);
        s.codec->get_format = pick_vaapi;
        // Handed out as dma-bufs, the queue and the frame on screen hold surfaces beyond what
        // decoding needs.
        if (prime) s.codec->extra_hw_frames = static_cast<int>(queue_depth) + 3;
        info.hardware = true;
        self->prime_.store(prime);
    } else if (hwdec == "vaapi") {
        log::warn("video: VA-API not available, decoding in software");
    }
    if (avcodec_open2(s.codec, codec, nullptr) < 0) {
        error = "cannot decode the video";
        return nullptr;
    }
    s.packet = av_packet_alloc();
    s.frame = av_frame_alloc();
    s.software = av_frame_alloc();
    s.time_base = av_q2d(stream->time_base);
    AVRational rate =
        stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    info.fps = rate.num > 0 ? av_q2d(rate) : 30.0;
    s.frame_duration = 1.0 / info.fps;
    info.width = static_cast<std::uint32_t>(s.codec->width);
    info.height = static_cast<std::uint32_t>(s.codec->height);
    info.codec = codec->name;
    info.duration =
        s.format->duration > 0 ? static_cast<double>(s.format->duration) / AV_TIME_BASE : 0;
    info.bt709 = s.codec->colorspace != AVCOL_SPC_BT470BG &&
                 s.codec->colorspace != AVCOL_SPC_SMPTE170M &&
                 (s.codec->colorspace == AVCOL_SPC_BT709 || info.height >= 720);
    info.full_range = s.codec->color_range == AVCOL_RANGE_JPEG;
    if (info.width == 0 || info.height == 0) {
        error = "video without a size";
        return nullptr;
    }
    self->thread_ = std::thread([raw = self.get()] { raw->run(); });
    return self;
}

bool Decoder::wait_first() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [this] { return !queue_.empty() || failed_ || stopping_.load(); });
    return !queue_.empty();
}

void Decoder::stop_prime() {
    std::lock_guard lock(mutex_);
    prime_.store(false);
    queue_.clear();
    wake_.notify_all();
}

void Decoder::set_paused(bool paused) {
    paused_.store(paused);
    wake_.notify_all();
}

std::optional<Picture> Decoder::take(double time) {
    std::lock_guard lock(mutex_);
    std::optional<Picture> picked;
    while (!queue_.empty() && queue_.front().time <= time) {
        picked = std::move(queue_.front());
        queue_.pop_front();
    }
    if (picked) wake_.notify_all();
    return picked;
}

bool Decoder::decode_one() {
    State& s = *state_;
    for (;;) {
        int r = avcodec_receive_frame(s.codec, s.frame);
        if (r == 0) break;
        if (r != AVERROR(EAGAIN)) {
            if (r == AVERROR_EOF) {
                // The end: start over, with the timeline carried on.
                loop_offset_ = s.last_pts + s.frame_duration;
                av_seek_frame(s.format, s.stream, 0, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(s.codec);
                continue;
            }
            return false;
        }
        int rr = av_read_frame(s.format, s.packet);
        if (rr < 0) {
            avcodec_send_packet(s.codec, nullptr);  // drain, then EOF
            continue;
        }
        if (s.packet->stream_index == s.stream) avcodec_send_packet(s.codec, s.packet);
        av_packet_unref(s.packet);
    }

    double pts = s.frame->best_effort_timestamp != AV_NOPTS_VALUE
                     ? static_cast<double>(s.frame->best_effort_timestamp) * s.time_base
                     : s.last_pts + s.frame_duration;
    s.last_pts = pts;

    if (s.frame->format == AV_PIX_FMT_VAAPI && prime_.load()) {
        // Mapped, not copied: the mapping holds the surface until the picture is dropped.
        AVFrame* mapped = av_frame_alloc();
        mapped->format = AV_PIX_FMT_DRM_PRIME;
        Prime prime;
        if (av_hwframe_map(mapped, s.frame, AV_HWFRAME_MAP_READ) == 0 &&
            describe(*reinterpret_cast<const AVDRMFrameDescriptor*>(mapped->data[0]), prime)) {
            Picture p;
            p.time = loop_offset_ + pts;
            p.width = static_cast<std::uint32_t>(s.frame->width);
            p.height = static_cast<std::uint32_t>(s.frame->height);
            p.prime = prime;
            p.hold = std::shared_ptr<AVFrame>(mapped, [](AVFrame* f) { av_frame_free(&f); });
            av_frame_unref(s.frame);
            std::lock_guard lock(mutex_);
            if (!prime_.load()) return true;  // switched off meanwhile; the frame is dropped
            queue_.push_back(std::move(p));
            wake_.notify_all();
            return true;
        }
        av_frame_free(&mapped);
        log::warn("video: cannot export the frame as a dma-buf, copying instead");
        prime_.store(false);
    }

    AVFrame* source = s.frame;
    if (s.frame->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(s.software);
        if (av_hwframe_transfer_data(s.software, s.frame, 0) < 0) return false;
        source = s.software;
    }
    Picture p;
    p.time = loop_offset_ + pts;
    p.width = static_cast<std::uint32_t>(source->width);
    p.height = static_cast<std::uint32_t>(source->height);
    std::uint32_t cw = (p.width + 1) / 2, ch = (p.height + 1) / 2;
    p.y.resize(static_cast<std::size_t>(p.width) * p.height);
    p.u.resize(static_cast<std::size_t>(cw) * ch);
    p.v.resize(static_cast<std::size_t>(cw) * ch);
    if (source->format == AV_PIX_FMT_YUV420P || source->format == AV_PIX_FMT_YUVJ420P) {
        // The common case: the planes as they are.
        av_image_copy_plane(p.y.data(), static_cast<int>(p.width), source->data[0],
                            source->linesize[0], static_cast<int>(p.width),
                            static_cast<int>(p.height));
        av_image_copy_plane(p.u.data(), static_cast<int>(cw), source->data[1], source->linesize[1],
                            static_cast<int>(cw), static_cast<int>(ch));
        av_image_copy_plane(p.v.data(), static_cast<int>(cw), source->data[2], source->linesize[2],
                            static_cast<int>(cw), static_cast<int>(ch));
    } else {
        s.sws = sws_getCachedContext(s.sws, source->width, source->height,
                                     static_cast<AVPixelFormat>(source->format), source->width,
                                     source->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
        if (s.sws == nullptr) return false;
        std::uint8_t* planes[] = {p.y.data(), p.u.data(), p.v.data(), nullptr};
        int strides[] = {static_cast<int>(p.width), static_cast<int>(cw), static_cast<int>(cw), 0};
        sws_scale(s.sws, source->data, source->linesize, 0, source->height, planes, strides);
    }
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(p));
    wake_.notify_all();
    return true;
}

void Decoder::run() {
    while (!stopping_.load()) {
        {
            std::unique_lock lock(mutex_);
            // Sleep while paused or full; the main thread wakes it when it takes a frame.
            wake_.wait(lock, [this] {
                return stopping_.load() || (!paused_.load() && queue_.size() < queue_depth);
            });
        }
        if (stopping_.load()) break;
        if (!decode_one()) {
            log::warn("video: decoding stopped");
            std::unique_lock lock(mutex_);
            failed_ = true;
            wake_.notify_all();
            wake_.wait(lock, [this] { return stopping_.load(); });
        }
    }
}

}  // namespace glint::video

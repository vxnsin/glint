#include "wallpaper/image/decode.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "core/clock.hpp"

namespace glint::image {

namespace {

std::string av_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

struct FormatCloser {
    void operator()(AVFormatContext* f) const { avformat_close_input(&f); }
};
struct CodecFreer {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameFreer {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketFreer {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct ScaleFreer {
    void operator()(SwsContext* s) const { sws_free_context(&s); }
};

// Converts a decoded frame to RGBA8 in staging memory, scaled down to max_dimension.

std::string convert(const AVFrame* frame, std::uint32_t max_dimension, gpu::StagingRing& staging,
                    Decoded& out) {
    auto src_w = static_cast<std::uint32_t>(frame->width);
    auto src_h = static_cast<std::uint32_t>(frame->height);
    if (src_w == 0 || src_h == 0) return "empty picture";
    std::uint32_t w = src_w, h = src_h;
    if (std::max(w, h) > max_dimension) {
        double k = static_cast<double>(max_dimension) / std::max(w, h);
        w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(w * k));
        h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(h * k));
    }

    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    out.pixels = staging.allocate(bytes);
    if (out.pixels.data == nullptr) return "out of staging memory";

    // Dynamic mode: range, colour space and chroma siting come from the frames themselves.
    std::unique_ptr<SwsContext, ScaleFreer> scaler(sws_alloc_context());
    scaler->flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    scaler->threads = 0;
    std::unique_ptr<AVFrame, FrameFreer> rgba(av_frame_alloc());
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = static_cast<int>(w);
    rgba->height = static_cast<int>(h);
    rgba->color_range = AVCOL_RANGE_JPEG;
    rgba->colorspace = AVCOL_SPC_RGB;
    rgba->color_primaries = frame->color_primaries;
    rgba->color_trc = frame->color_trc;
    rgba->data[0] = reinterpret_cast<std::uint8_t*>(out.pixels.data);
    rgba->linesize[0] = static_cast<int>(w * 4);
    if (int r = sws_scale_frame(scaler.get(), rgba.get(), frame); r < 0) {
        staging.abandon(out.pixels);
        out.pixels = {};
        return "cannot convert: " + av_error(r);
    }
    // The frame does not own the staging memory.
    rgba->data[0] = nullptr;

    out.width = w;
    out.height = h;
    return {};
}

}  // namespace

std::string decode(const std::filesystem::path& path, std::uint32_t max_dimension,
                   gpu::StagingRing& staging, Decoded& out) {
    std::int64_t start = clock::now_ns();
    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext* raw_format = nullptr;
    if (path.string().starts_with("http://") || path.string().starts_with("https://")) {
        // Album covers come as URLs; FFmpeg fetches them itself.
        static std::once_flag network;
        std::call_once(network, [] { avformat_network_init(); });
    }
    if (int r = avformat_open_input(&raw_format, path.c_str(), nullptr, nullptr); r < 0) {
        return "cannot open: " + av_error(r);
    }
    std::unique_ptr<AVFormatContext, FormatCloser> format(raw_format);
    if (int r = avformat_find_stream_info(format.get(), nullptr); r < 0) {
        return "cannot read: " + av_error(r);
    }
    const AVCodec* codec = nullptr;
    int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0 || codec == nullptr) return "not a picture";

    std::unique_ptr<AVCodecContext, CodecFreer> decoder(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(decoder.get(), format->streams[stream]->codecpar);
    decoder->thread_count = 0;  // as many as useful
    if (int r = avcodec_open2(decoder.get(), codec, nullptr); r < 0) {
        return "cannot decode: " + av_error(r);
    }

    std::unique_ptr<AVFrame, FrameFreer> frame(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketFreer> packet(av_packet_alloc());
    bool got = false;
    bool flushing = false;
    while (!got) {
        if (!flushing) {
            int r = av_read_frame(format.get(), packet.get());
            if (r < 0) {
                flushing = true;
                avcodec_send_packet(decoder.get(), nullptr);
            } else if (packet->stream_index == stream) {
                avcodec_send_packet(decoder.get(), packet.get());
                av_packet_unref(packet.get());
            } else {
                av_packet_unref(packet.get());
                continue;
            }
        }
        int r = avcodec_receive_frame(decoder.get(), frame.get());
        if (r == 0) {
            got = true;
        } else if (r == AVERROR(EAGAIN) && !flushing) {
            continue;
        } else {
            return r == AVERROR_EOF ? "no picture in the file" : "cannot decode: " + av_error(r);
        }
    }

    if (std::string error = convert(frame.get(), max_dimension, staging, out); !error.empty()) {
        return error;
    }
    out.decode_ms = clock::ms_since(start);
    return {};
}

std::string decode_memory(std::span<const std::byte> data, int image_format,
                          std::uint32_t max_dimension, gpu::StagingRing& staging, Decoded& out) {
    std::int64_t start = clock::now_ns();
    av_log_set_level(AV_LOG_ERROR);
    AVCodecID id = image_format == 13  ? AV_CODEC_ID_PNG
                   : image_format == 2 ? AV_CODEC_ID_MJPEG
                                       : AV_CODEC_ID_NONE;
    // Some files mislabel; the first bytes say what it is.
    if (data.size() >= 4) {
        auto b = reinterpret_cast<const unsigned char*>(data.data());
        if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') id = AV_CODEC_ID_PNG;
        if (b[0] == 0xFF && b[1] == 0xD8) id = AV_CODEC_ID_MJPEG;
    }
    const AVCodec* codec = avcodec_find_decoder(id);
    if (codec == nullptr) return "embedded image of unknown type " + std::to_string(image_format);
    std::unique_ptr<AVCodecContext, CodecFreer> decoder(avcodec_alloc_context3(codec));
    decoder->thread_count = 0;
    if (int r = avcodec_open2(decoder.get(), codec, nullptr); r < 0) {
        return "cannot decode: " + av_error(r);
    }
    std::unique_ptr<AVPacket, PacketFreer> packet(av_packet_alloc());
    if (av_new_packet(packet.get(), static_cast<int>(data.size())) < 0) return "out of memory";
    std::memcpy(packet->data, data.data(), data.size());
    std::unique_ptr<AVFrame, FrameFreer> frame(av_frame_alloc());
    if (int r = avcodec_send_packet(decoder.get(), packet.get()); r < 0) {
        return "cannot decode: " + av_error(r);
    }
    avcodec_send_packet(decoder.get(), nullptr);
    if (int r = avcodec_receive_frame(decoder.get(), frame.get()); r < 0) {
        return "cannot decode: " + av_error(r);
    }
    if (std::string error = convert(frame.get(), max_dimension, staging, out); !error.empty()) {
        return error;
    }
    out.decode_ms = clock::ms_since(start);
    return {};
}

std::string write_png(const std::filesystem::path& path, std::span<const std::byte> pixels,
                      std::uint32_t width, std::uint32_t height, bool bgra) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr) return "no PNG encoder";
    std::unique_ptr<AVCodecContext, CodecFreer> encoder(avcodec_alloc_context3(codec));
    encoder->width = static_cast<int>(width);
    encoder->height = static_cast<int>(height);
    encoder->pix_fmt = AV_PIX_FMT_RGB24;
    encoder->time_base = {1, 1};
    if (int r = avcodec_open2(encoder.get(), codec, nullptr); r < 0) return av_error(r);

    // Drop the alpha (the swapchain's is meaningless) and swap to RGB.
    std::unique_ptr<AVFrame, FrameFreer> frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = static_cast<int>(width);
    frame->height = static_cast<int>(height);
    if (av_frame_get_buffer(frame.get(), 0) < 0) return "out of memory";
    auto src = reinterpret_cast<const std::uint8_t*>(pixels.data());
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t* p = src + (static_cast<std::size_t>(y) * width + x) * 4;
            row[x * 3 + 0] = bgra ? p[2] : p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = bgra ? p[0] : p[2];
        }
    }
    std::unique_ptr<AVPacket, PacketFreer> packet(av_packet_alloc());
    if (int r = avcodec_send_frame(encoder.get(), frame.get()); r < 0) return av_error(r);
    if (int r = avcodec_receive_packet(encoder.get(), packet.get()); r < 0) return av_error(r);
    FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) return "cannot write " + path.string();
    std::fwrite(packet->data, 1, static_cast<std::size_t>(packet->size), out);
    std::fclose(out);
    return {};
}

}  // namespace glint::image

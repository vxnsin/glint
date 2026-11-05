#include "wallpaper/video/source.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vk_mem_alloc.h>

#include <cstdlib>
#include <cstring>
#include <format>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/worker.hpp"
#include "gpu/context.hpp"
#include "gpu/surface.hpp"
#include "gpu/video_pass.hpp"

namespace glint::video {

namespace {

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace

Source::Source(std::filesystem::path path, const Options& options, Services& services)
    : path_(std::move(path)), options_(options), services_(services) {}

Source::~Source() {
    player_.reset();
    decoder_.reset();
    gpu::Context& gpu = services_.gpu;
    if (shown_hold_) gpu.defer([hold = std::move(shown_hold_)]() mutable { hold.reset(); });
    for (Plane& p : planes_) {
        if (p.image == VK_NULL_HANDLE) continue;
        gpu.defer([&gpu, i = p.image, a = p.allocation, v = p.view] {
            vkDestroyImageView(gpu.device(), v, nullptr);
            vmaDestroyImage(gpu.allocator(), i, a);
        });
    }
    for (gpu::Buffer& b : staging_) {
        auto held = std::make_shared<gpu::Buffer>(std::move(b));
        gpu.defer([held]() mutable { held.reset(); });
    }
    if (mapped_ != nullptr) ::munmap(const_cast<std::byte*>(mapped_), mapped_size_);
}

void Source::load(std::function<void(std::string error)> done) {
    if (decoder_ || !error_.empty()) {
        done(error_);
        return;
    }
    waiting_.push_back(std::move(done));
    if (loading_) return;
    loading_ = true;
    std::weak_ptr<bool> alive = alive_;
    struct Result {
        std::unique_ptr<Decoder> decoder;
        std::optional<Picture> first;
        std::string error;
    };
    auto result = std::make_shared<Result>();
    Result* raw = result.get();
    std::filesystem::path path = path_;
    std::string hwdec = options_.hwdec;
    bool prime = services_.gpu.dmabuf() && std::getenv("GLINT_VIDEO_COPY") == nullptr;
    services_.workers.post(
        [raw, path, hwdec, prime] {
            raw->decoder = Decoder::open(path, hwdec, prime, raw->error);
            if (!raw->decoder) return;
            if (!raw->decoder->wait_first()) {
                raw->error = "no frame in the video";
                raw->decoder.reset();
                return;
            }
            raw->first = raw->decoder->take(1e300);
        },
        [this, alive, result] {
            if (alive.expired()) return;
            loading_ = false;
            auto waiting = std::move(waiting_);
            waiting_.clear();
            if (!result->decoder || !result->first) {
                error_ = result->error.empty() ? "cannot play the video" : result->error;
                for (auto& callback : waiting) callback(error_);
                return;
            }
            decoder_ = std::move(result->decoder);
            first_ = std::move(result->first);
            const Info& info = decoder_->info();
            prime_ = first_->prime.has_value();
            if (!prime_) make_planes(first_->width, first_->height);
            log::info("video {}: {}x{} {} at {:.2f} fps, {}", path_.filename().string(), info.width,
                      info.height, info.codec, info.fps,
                      prime_          ? "VA-API, dma-buf"
                      : info.hardware ? "VA-API, copied"
                                      : "software");
            decoder_->set_paused(runners_ == 0);
            start_sound();
            for (auto& callback : waiting) callback({});
        });
}

void Source::make_planes(std::uint32_t width, std::uint32_t height) {
    std::uint32_t sizes[3][2] = {
        {width, height}, {(width + 1) / 2, (height + 1) / 2}, {(width + 1) / 2, (height + 1) / 2}};
    gpu::Context& gpu = services_.gpu;
    VkDeviceSize bytes = 0;
    for (int i = 0; i < 3; ++i) {
        Plane& p = planes_[static_cast<std::size_t>(i)];
        p.width = sizes[i][0];
        p.height = sizes[i][1];
        bytes += static_cast<VkDeviceSize>(p.width) * p.height;
        VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8_UNORM,
            .extent = {p.width, p.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VmaAllocationCreateInfo alloc{.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                      .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VmaAllocationInfo result{};
        if (vmaCreateImage(gpu.allocator(), &info, &alloc, &p.image, &p.allocation, &result) !=
            VK_SUCCESS) {
            p.image = VK_NULL_HANDLE;
            return;
        }
        p.memory = result.size;
        VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = p.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCreateImageView(gpu.device(), &view, nullptr, &p.view);
    }
    // Two staging buffers: one is being written while the GPU may still copy from the other.
    for (gpu::Buffer& b : staging_) {
        b = gpu::Buffer(gpu.allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }
}

void Source::upload(const Picture& picture, VkCommandBuffer cmd) {
    if (picture.width != planes_[0].width || picture.height != planes_[0].height) return;
    gpu::Buffer& buffer = staging_[slot_];
    slot_ ^= 1u;
    if (!buffer) return;
    const std::vector<std::uint8_t>* data[] = {&picture.y, &picture.u, &picture.v};
    VkDeviceSize offset = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        Plane& p = planes_[i];
        std::memcpy(buffer.mapped() + offset, data[i]->data(), data[i]->size());
        barrier(cmd, p.image,
                p.sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{
            .bufferOffset = offset,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {p.width, p.height, 1},
        };
        vkCmdCopyBufferToImage(cmd, buffer.get(), p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);
        barrier(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        p.sampled = true;
        offset += data[i]->size();
    }
    buffer.flush(0, VK_WHOLE_SIZE);
}

void Source::show(Picture& picture, VkCommandBuffer cmd) {
    const Prime& prime = *picture.prime;
    auto it = imports_.find(prime.key);
    if (it == imports_.end()) {
        if (imports_.size() >= 32) {
            // More surfaces than a pool holds: the decoder made new ones. Start over.
            shown_ = nullptr;
            imports_.clear();
        }
        std::uint32_t cw = (picture.width + 1) / 2, ch = (picture.height + 1) / 2;
        gpu::DmabufPlane y{prime.planes[0].fd,
                           prime.modifier,
                           prime.planes[0].offset,
                           prime.planes[0].pitch,
                           picture.width,
                           picture.height,
                           prime.deep ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM};
        gpu::DmabufPlane uv{prime.planes[1].fd,
                            prime.modifier,
                            prime.planes[1].offset,
                            prime.planes[1].pitch,
                            cw,
                            ch,
                            prime.deep ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM};
        std::string error;
        Imported imported;
        imported.y = gpu::DmabufImage::import(services_.gpu, y, error);
        if (imported.y) imported.uv = gpu::DmabufImage::import(services_.gpu, uv, error);
        if (!imported.y || !imported.uv) {
            log::warn("video: cannot import the frame ({}), copying instead", error);
            prime_ = false;
            decoder_->stop_prime();
            make_planes(picture.width, picture.height);
            return;
        }
        it = imports_.emplace(prime.key, std::move(imported)).first;
    }
    // Written by the video decoder outside Vulkan: taken over from the foreign queue.
    VkImageMemoryBarrier2 barriers[2];
    VkImage images[] = {it->second.y->image(), it->second.uv->image()};
    for (int i = 0; i < 2; ++i) {
        barriers[i] = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = services_.gpu.queue_family(),
            .image = images[i],
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
    }
    VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 2,
        .pImageMemoryBarriers = barriers,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
    shown_ = &it->second;
    // The surface shown before stays held until the GPU is done sampling it.
    if (shown_hold_) {
        services_.gpu.defer([hold = std::move(shown_hold_)]() mutable { hold.reset(); });
    }
    shown_hold_ = std::move(picture.hold);
}

void Source::present(Picture& picture, VkCommandBuffer cmd) {
    if (picture.prime && prime_) {
        show(picture, cmd);
        return;
    }
    if (picture.prime) return;  // left over from before the switch to copies
    if (prime_) {
        // The decoder gave up on dma-bufs by itself.
        prime_ = false;
        make_planes(picture.width, picture.height);
    }
    upload(picture, cmd);
}

void Source::prepare(VkCommandBuffer cmd) {
    if (!decoder_) return;
    double time = clock_;
    if (runners_ > 0) time += static_cast<double>(clock::now_ns() - since_) / 1e9;
    if (first_) {
        present(*first_, cmd);
        first_.reset();
    }
    if (auto picture = decoder_->take(time * static_cast<double>(options_.speed))) {
        present(*picture, cmd);
    }
}

bool Source::render(const gpu::Target& target, const Options& options, float opacity) {
    const Info& info = decoder_->info();
    gpu::VideoPass::Frame f{};
    if (prime_) {
        if (shown_ == nullptr) return false;
        f = {shown_->y->view(), shown_->uv->view(), VK_NULL_HANDLE,  info.width,
             info.height,       info.bt709,         info.full_range, true};
    } else {
        if (!planes_[0].sampled) return false;
        f = {planes_[0].view,   planes_[1].view, planes_[2].view, planes_[0].width,
             planes_[0].height, info.bt709,      info.full_range};
    }
    services_.video.draw(target, f, options.scaling, options.linear, opacity);
    return true;
}

void Source::add_runner(int delta) {
    bool was = runners_ > 0;
    runners_ += delta;
    if (!was && runners_ > 0) since_ = clock::now_ns();
    if (was && runners_ <= 0) clock_ += static_cast<double>(clock::now_ns() - since_) / 1e9;
    bool paused = runners_ <= 0;
    if (decoder_) decoder_->set_paused(paused);
    if (player_) player_->set_paused(paused);
}

std::uint64_t Source::memory() const {
    std::uint64_t total = 0;
    for (const Plane& p : planes_) total += p.memory;
    // dma-buf frames: the decoder's surfaces, both planes in one buffer object or two.
    for (const auto& [key, imported] : imports_) {
        total += imported.y->size();
        if (imported.uv->size() != imported.y->size()) total += imported.uv->size();
    }
    return total;
}

void Source::start_sound() {
    float volume = options_.mute ? 0.0f : options_.volume;
    if (volume <= 0) return;  // no stream, no decoding of the sound track
    int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st{};
    ::fstat(fd, &st);
    void* map =
        ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return;
    mapped_ = static_cast<const std::byte*>(map);
    mapped_size_ = static_cast<std::size_t>(st.st_size);
    audio::TrackSpec track;
    track.files.push_back({mapped_, mapped_size_});
    player_ = std::make_unique<audio::Player>();
    if (std::string error = player_->start({track}, volume); !error.empty()) {
        log::warn("video sound: {}", error);
        player_.reset();
        return;
    }
    if (runners_ <= 0) player_->set_paused(true);
}

std::shared_ptr<Source> Sources::get(const std::filesystem::path& path, const Options& options,
                                     Services& services) {
    float volume = options.mute ? 0.0f : options.volume;
    std::string key =
        std::format("{}\n{}\n{}\n{}", path.string(), options.hwdec, options.speed, volume);
    for (auto it = sources_.begin(); it != sources_.end();) {
        it = it->second.expired() ? sources_.erase(it) : std::next(it);
    }
    if (auto existing = sources_[key].lock()) return existing;
    auto source = std::make_shared<Source>(path, options, services);
    sources_[key] = source;
    return source;
}

}  // namespace glint::video

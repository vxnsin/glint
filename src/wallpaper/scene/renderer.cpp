#include "wallpaper/scene/renderer.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>

#include "core/log.hpp"
#include "gpu/blit.hpp"
#include "gpu/context.hpp"
#include "gpu/surface.hpp"
#include "gpu/texture.hpp"

namespace glint::scene {

namespace {

constexpr VkFormat target_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint32_t max_target = 8192;

std::vector<float> floats_of(const nlohmann::json& j) {
    std::vector<float> out;
    if (j.is_number()) {
        out.push_back(j.get<float>());
    } else if (j.is_boolean()) {
        out.push_back(j.get<bool>() ? 1.0f : 0.0f);
    } else if (j.is_string()) {
        std::istringstream in(j.get<std::string>());
        float f;
        while (in >> f) out.push_back(f);
    } else if (j.is_array()) {
        for (const auto& e : j) {
            if (e.is_number()) out.push_back(e.get<float>());
        }
    } else if (j.is_object() && j.contains("value")) {
        return floats_of(j["value"]);
    }
    return out;
}

// Components per element and columns, from the GLSL type.
void shape(const std::string& type, int& rows, int& cols, bool& integer) {
    integer = type.starts_with("int") || type.starts_with("ivec") || type.starts_with("uint") ||
              type.starts_with("uvec") || type.starts_with("bool") || type.starts_with("bvec");
    rows = 1;
    cols = 1;
    if (type.ends_with("vec2")) rows = 2;
    if (type.ends_with("vec3")) rows = 3;
    if (type.ends_with("vec4")) rows = 4;
    if (type.starts_with("mat")) {
        // matCxR in GLSL: C columns, R rows; matN is square.
        std::string dims = type.substr(3);
        if (dims.size() == 1) {
            cols = rows = dims[0] - '0';
        } else if (dims.size() == 3) {
            cols = dims[0] - '0';
            rows = dims[2] - '0';
        }
    }
}

void write(std::vector<std::byte>& block, const ShaderUniform& u, const std::vector<float>& v) {
    if (v.empty()) return;
    int rows, cols;
    bool integer;
    shape(u.type, rows, cols, integer);
    auto put = [&](std::uint32_t offset, float f) {
        if (offset + 4 > block.size()) return;
        if (integer) {
            auto i = static_cast<std::int32_t>(std::lround(f));
            std::memcpy(block.data() + offset, &i, 4);
        } else {
            std::memcpy(block.data() + offset, &f, 4);
        }
    };
    if (cols > 1) {
        // Matrices: column-major, each column 16 bytes apart (std140). A single number
        // means a scaled identity.
        std::size_t needed = static_cast<std::size_t>(rows * cols);
        for (int c = 0; c < cols; ++c) {
            for (int r = 0; r < rows; ++r) {
                std::size_t i = static_cast<std::size_t>(c * rows + r);
                float f = v.size() >= needed ? v[i] : (r == c ? v[0] : 0.0f);
                put(u.offset + static_cast<std::uint32_t>(c * 16 + r * 4), f);
            }
        }
        return;
    }
    std::uint32_t stride = u.count > 1 ? 16 : 0;
    for (std::uint32_t e = 0; e < u.count; ++e) {
        for (int r = 0; r < rows; ++r) {
            std::size_t i = static_cast<std::size_t>(e * static_cast<std::uint32_t>(rows)) +
                            static_cast<std::size_t>(r);
            // A scalar fills every component ("1" for a vec3 colour).
            float f = i < v.size() ? v[i] : (v.size() == 1 ? v[0] : 0.0f);
            if (e > 0 && i >= v.size()) return;
            put(u.offset + e * stride + static_cast<std::uint32_t>(r * 4), f);
        }
    }
}

void write_matrix(std::vector<std::byte>& block, std::uint32_t offset, const math::Mat4& m) {
    if (offset + 64 <= block.size()) std::memcpy(block.data() + offset, m.m, 64);
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
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

SceneRenderer::SceneRenderer(gpu::Context& gpu, gpu::Blit& blit,
                             std::shared_ptr<SceneAssets> assets)
    : gpu_(gpu), blit_(blit), assets_(std::move(assets)) {
    values_ = default_values(assets_->project());
    const SceneGeneral& general = assets_->scene().general;
    scripts_ =
        std::make_unique<ScriptHost>(assets_->scene(), assets_->project(), assets_->project().title,
                                     assets_->vfs().item().filename().string(), &assets_->vfs());
    if (scripts_->empty()) {
        scripts_.reset();
    } else {
        values_.cells = &scripts_->cells();
        scripts_->set_size_callback([this](int index) {
            const SceneDesc& scene = scene_desc();
            auto size_of = [this](const std::string& name) { return picture_size_of(name); };
            return object_size(scene.images[static_cast<std::size_t>(index)], scene.general,
                               values_, size_of);
        });
        log::info("scene {}: {} scripts", assets_->project().title, scripts_->count());
        if (scripts_->uses_media()) uses_media_ = true;
    }
    for (const ImageObject& im : scene_desc().images) {
        if (im.media_thumbnail) uses_media_ = true;
    }
    auto clear = parse_vec3(general.clear_color.resolve(values_), {0, 0, 0});
    clear_[0] = clear[0];
    clear_[1] = clear[1];
    clear_[2] = clear[2];
    clear_enabled_ = general.clear_enabled;
    parallax_ = general.camera_parallax;
    parallax_amount_ = general.parallax_amount;
    zeros_ = gpu::Buffer(gpu_.allocator(), 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    if (zeros_) std::memset(zeros_.mapped(), 0, 64);
}

SceneRenderer::~SceneRenderer() {
    release();
    auto zeros = std::make_shared<gpu::Buffer>(std::move(zeros_));
    gpu_.defer([zeros]() mutable { zeros.reset(); });
}

void SceneRenderer::set_values(const Values& values) {
    Values before = values_;
    values_ = values;
    values_.cells = scripts_ ? &scripts_->cells() : nullptr;
    if (scripts_ && scripts_started_) scripts_->values_changed(before, values_);
    stale_ = true;
}

void SceneRenderer::set_media(const MediaState& state) {
    bool thumbnail = state.thumbnail_version != media_.thumbnail_version;
    media_ = state;
    // A new cover: the draws hold its view.
    if (thumbnail) stale_ = true;
}

const SceneTexture& SceneRenderer::texture_of(const std::string& name) {
    if (name.starts_with("$mediaThumbnail:")) {
        if (media_.has_thumbnail && media_.thumbnail) {
            static thread_local SceneTexture cover;
            cover.texture = media_.thumbnail;
            cover.tex_width = cover.img_width = media_.thumbnail_width;
            cover.tex_height = cover.img_height = media_.thumbnail_height;
            cover.clamp = true;
            return cover;
        }
        return assets_->texture(name.substr(16));
    }
    if (name.starts_with("_text_")) {
        int id = std::atoi(name.c_str() + 6);
        if (auto it = texts_.find(id); it != texts_.end()) return it->second.texture;
    }
    if (scripts_) assets_->ensure_texture(name);  // a layer a script made may name a new file
    return assets_->texture(name);
}

std::optional<Vec2> SceneRenderer::picture_size_of(const std::string& name) {
    if (name.starts_with("$mediaThumbnail:")) {
        if (media_.has_thumbnail) {
            return Vec2{static_cast<float>(media_.thumbnail_width),
                        static_cast<float>(media_.thumbnail_height)};
        }
        return assets_->picture_size(name.substr(16));
    }
    if (name.starts_with("_text_")) {
        int id = std::atoi(name.c_str() + 6);
        if (auto it = texts_.find(id); it != texts_.end()) {
            const SceneTexture& t = it->second.texture;
            return Vec2{static_cast<float>(t.img_width) * t.units,
                        static_cast<float>(t.img_height) * t.units};
        }
    }
    if (scripts_) assets_->ensure_texture(name);
    return assets_->picture_size(name);
}

void SceneRenderer::update_text(int id, double time) {
    const SceneDesc& scene = scene_desc();
    for (const TextObject& text : scene.texts) {
        if (text.id != id) continue;
        nlohmann::json value = text.text.at(values_, time);
        std::string str = value.is_string() ? value.get<std::string>()
                          : value.is_null() ? ""
                                            : value.dump();
        TextOverride over;
        if (!assets_->make_text(text, str, over.texture, over.center)) return;
        // The old texture goes once the frames that read it are done.
        if (auto it = texts_.find(id); it != texts_.end()) {
            auto old = std::make_shared<SceneTexture>(std::move(it->second.texture));
            gpu_.defer([old]() mutable { old.reset(); });
        }
        texts_[id] = std::move(over);
        stale_ = true;
        return;
    }
}

ScriptFrame SceneRenderer::script_frame(const FrameInputs& inputs) const {
    const SceneGeneral& general = scene_desc().general;
    ScriptFrame f;
    f.time = inputs.time;
    f.delta = inputs.delta;
    f.daytime = inputs.daytime;
    f.canvas[0] = general.ortho[0];
    f.canvas[1] = general.ortho[1];
    f.screen[0] = static_cast<float>(extent_.width);
    f.screen[1] = static_cast<float>(extent_.height);
    f.pointer_world[0] = canvas_left_ + inputs.pointer[0] * canvas_width_;
    f.pointer_world[1] = canvas_bottom_ + (1 - inputs.pointer[1]) * canvas_height_;
    f.pointer_screen[0] = inputs.pointer[0] * static_cast<float>(extent_.width);
    f.pointer_screen[1] = inputs.pointer[1] * static_cast<float>(extent_.height);
    f.left_down = (inputs.buttons & 1u) != 0;
    f.audio_left = inputs.audio_left;
    f.audio_right = inputs.audio_right;
    return f;
}

SceneRenderer::Image SceneRenderer::make_image(VkExtent2D extent, VkFormat format) {
    Image image;
    image.extent = extent;
    VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo alloc{.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo result{};
    if (vmaCreateImage(gpu_.allocator(), &info, &alloc, &image.image, &image.allocation, &result) !=
        VK_SUCCESS) {
        log::error("scene target {}x{} failed", extent.width, extent.height);
        image.image = VK_NULL_HANDLE;
        return image;
    }
    image.memory = result.size;
    VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCreateImageView(gpu_.device(), &view, nullptr, &image.view);
    return image;
}

void SceneRenderer::destroy_image(Image& image) {
    if (image.image == VK_NULL_HANDLE) return;
    gpu::Context& gpu = gpu_;
    gpu.defer([&gpu, i = image.image, a = image.allocation, v = image.view] {
        vkDestroyImageView(gpu.device(), v, nullptr);
        vmaDestroyImage(gpu.allocator(), i, a);
    });
    image = {};
}

void SceneRenderer::release() {
    destroy_image(scene_);
    destroy_image(framebuffer_);
    for (auto& t : images_) destroy_image(*t);
    images_.clear();
    targets_.clear();
    draws_.clear();
    // Buffers may be read by frames in flight.
    auto vertices = std::make_shared<gpu::Buffer>(std::move(vertices_));
    auto indices = std::make_shared<gpu::Buffer>(std::move(indices_));
    auto uniforms = std::make_shared<gpu::Buffer>(std::move(uniforms_));
    auto pv0 = std::make_shared<gpu::Buffer>(std::move(particle_vertices_[0]));
    auto pv1 = std::make_shared<gpu::Buffer>(std::move(particle_vertices_[1]));
    auto pi = std::make_shared<gpu::Buffer>(std::move(particle_indices_));
    auto sv0 = std::make_shared<gpu::Buffer>(std::move(skinned_vertices_[0]));
    auto sv1 = std::make_shared<gpu::Buffer>(std::move(skinned_vertices_[1]));
    gpu_.defer([vertices, indices, uniforms, pv0, pv1, pi, sv0, sv1]() mutable {
        vertices.reset();
        indices.reset();
        uniforms.reset();
        pv0.reset();
        pv1.reset();
        pi.reset();
        sv0.reset();
        sv1.reset();
    });
    extent_ = {0, 0};
}

math::Mat4 SceneRenderer::model_matrix(int object, std::vector<int>& visiting, double time) const {
    const SceneDesc& scene = scene_desc();
    const ImageObject& image = scene.images[static_cast<std::size_t>(object)];
    Vec3 origin = parse_vec3(image.origin.at(values_, time), {0, 0, 0});
    Vec3 scale = parse_vec3(image.scale.at(values_, time), {1, 1, 1});
    Vec3 angles = parse_vec3(image.angles.at(values_, time), {0, 0, 0});
    // scene.json keeps angles in radians; z turns clockwise on the canvas.
    math::Mat4 local = math::Mat4::translate(origin[0], origin[1], origin[2]) *
                       math::Mat4::rotate_z(-angles[2]) * math::Mat4::rotate_y(angles[1]) *
                       math::Mat4::rotate_x(angles[0]) *
                       math::Mat4::scale(scale[0], scale[1], scale[2]);
    if (image.text >= 0) {
        // A text block sits around the origin by its alignment, not centred on it.
        Vec2 offset = image.text_offset;
        if (auto it = texts_.find(image.id); it != texts_.end()) offset = it->second.center;
        local = local * math::Mat4::translate(offset[0], offset[1], 0);
    }
    if (image.parent < 0) return local;
    if (std::ranges::find(visiting, object) != visiting.end()) return local;  // a cycle
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        if (scene.images[i].id == image.parent) {
            visiting.push_back(object);
            math::Mat4 parent = model_matrix(static_cast<int>(i), visiting, time);
            visiting.pop_back();
            // Hung from a point on the parent puppet's skeleton.
            const ImageObject& owner = scene.images[i];
            if (!image.attachment.empty() && owner.puppet) {
                if (const Mesh* mesh = assets_->mesh(owner.puppet_path)) {
                    std::vector<math::Mat4> locals;
                    pose_of(owner, *mesh, time, locals);
                    math::Mat4 attach;
                    if (mesh->attachment_matrix(image.attachment, locals, attach)) {
                        return parent * attach * local;
                    }
                }
            }
            return parent * local;
        }
    }
    // The parent is a particle object, or nothing.
    visiting.push_back(object);
    math::Mat4 parent = transform_of_id(image.parent, visiting, time);
    visiting.pop_back();
    return parent * local;
}

math::Mat4 SceneRenderer::particle_matrix(int particle, std::vector<int>& visiting,
                                          double time) const {
    const SceneDesc& scene = scene_desc();
    const ParticleObject& object = scene.particles[static_cast<std::size_t>(particle)];
    Vec3 origin = parse_vec3(object.origin.at(values_, time), {0, 0, 0});
    Vec3 scale = parse_vec3(object.scale.at(values_, time), {1, 1, 1});
    Vec3 angles = parse_vec3(object.angles.at(values_, time), {0, 0, 0});
    math::Mat4 local = math::Mat4::translate(origin[0], origin[1], origin[2]) *
                       math::Mat4::rotate_z(-angles[2]) * math::Mat4::rotate_y(angles[1]) *
                       math::Mat4::rotate_x(angles[0]) *
                       math::Mat4::scale(scale[0], scale[1], scale[2]);
    if (object.parent < 0) return local;
    // Particles are numbered after the images in the cycle guard.
    int key = static_cast<int>(scene.images.size()) + particle;
    if (std::ranges::find(visiting, key) != visiting.end()) return local;
    visiting.push_back(key);
    math::Mat4 parent = transform_of_id(object.parent, visiting, time);
    visiting.pop_back();
    return parent * local;
}

void SceneRenderer::pose_of(const ImageObject& object, const Mesh& mesh, double time,
                            std::vector<math::Mat4>& locals) const {
    std::vector<math::Mat4> layer_locals, rest;
    mesh.rest_pose(rest);
    bool first = true;
    int image_index = -1;
    const std::vector<AnimationLayer>* layers = &object.animation_layers;
    if (scripts_) {
        const SceneDesc& scene = scene_desc();
        for (std::size_t i = 0; i < scene.images.size(); ++i) {
            if (&scene.images[i] == &object) image_index = static_cast<int>(i);
        }
        if (const auto* scripted = scripts_->animation_layers(image_index)) layers = scripted;
    }
    for (std::size_t l = 0; l < layers->size(); ++l) {
        const AnimationLayer& layer = (*layers)[l];
        const Mesh::Animation* animation =
            layer.visible ? mesh.animation(layer.animation) : nullptr;
        if (animation == nullptr) continue;
        double t = scripts_ ? scripts_->layer_time(image_index, static_cast<int>(l), time) : time;
        double frame = Mesh::frame_at(*animation, t, layer.rate);
        mesh.pose(*animation, frame, layer_locals);
        if (first) {
            locals = layer_locals;
            first = false;
            continue;
        }
        // Later layers blend in by their weight: additive ones add their change from the
        // rest pose, the others move toward their pose.
        float w = std::clamp(layer.blend, 0.0f, 1.0f);
        for (std::size_t b = 0; b < locals.size() && b < layer_locals.size(); ++b) {
            for (int k = 0; k < 16; ++k) {
                float v = layer.additive ? layer_locals[b].m[k] - rest[b].m[k]
                                         : layer_locals[b].m[k] - locals[b].m[k];
                locals[b].m[k] += v * w;
            }
        }
    }
    if (first) locals = rest;
}

math::Mat4 SceneRenderer::transform_of_id(int id, std::vector<int>& visiting, double time) const {
    const SceneDesc& scene = scene_desc();
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        if (scene.images[i].id == id) return model_matrix(static_cast<int>(i), visiting, time);
    }
    for (std::size_t i = 0; i < scene.particles.size(); ++i) {
        if (scene.particles[i].id == id)
            return particle_matrix(static_cast<int>(i), visiting, time);
    }
    for (std::size_t i = 0; i < scene.groups.size(); ++i) {
        const GroupObject& group = scene.groups[i];
        if (group.id != id) continue;
        Vec3 origin = parse_vec3(group.origin.at(values_, time), {0, 0, 0});
        Vec3 scale = parse_vec3(group.scale.at(values_, time), {1, 1, 1});
        Vec3 angles = parse_vec3(group.angles.at(values_, time), {0, 0, 0});
        math::Mat4 local = math::Mat4::translate(origin[0], origin[1], origin[2]) *
                           math::Mat4::rotate_z(-angles[2]) * math::Mat4::rotate_y(angles[1]) *
                           math::Mat4::rotate_x(angles[0]) *
                           math::Mat4::scale(scale[0], scale[1], scale[2]);
        if (group.parent < 0) return local;
        // Groups come after the images and particles in the cycle guard.
        int key = static_cast<int>(scene.images.size() + scene.particles.size() + i);
        if (std::ranges::find(visiting, key) != visiting.end()) return local;
        visiting.push_back(key);
        math::Mat4 parent = transform_of_id(group.parent, visiting, time);
        visiting.pop_back();
        return parent * local;
    }
    return {};
}

void SceneRenderer::build(VkExtent2D extent) {
    release();
    const SceneDesc& scene = scene_desc();
    const SceneGeneral& general = scene.general;
    extent_ = extent;

    // The canvas fills the output: the larger scale wins and the rest is cropped, centred.
    float w = general.ortho[0], h = general.ortho[1];
    canvas_scale_ =
        std::max(static_cast<float>(extent.width) / w, static_cast<float>(extent.height) / h);
    float vw = static_cast<float>(extent.width) / canvas_scale_;
    float vh = static_cast<float>(extent.height) / canvas_scale_;
    float left = (w - vw) * 0.5f, bottom = (h - vh) * 0.5f;
    canvas_left_ = left;
    canvas_bottom_ = bottom;
    canvas_width_ = vw;
    canvas_height_ = vh;
    // Depth covers what scenes use: objects sit at z = -100 and similar, WE's farz is 10000.
    projection_ = math::Mat4::ortho(left, left + vw, bottom, bottom + vh, -10000, 10000);

    auto size_of = [this](const std::string& name) { return picture_size_of(name); };
    auto info_of = [this](const std::string& name) { return texture_of(name).plan_info(); };
    plan_ = build_plan(scene, values_, size_of, false, info_of);
    built_structure_ = scripts_ ? scripts_->structure_version() : 0;
    for (const auto& w2 : plan_.warnings) log::warn("scene: {}", w2);

    scene_ = make_image(extent, target_format);
    if (plan_.reads_framebuffer) framebuffer_ = make_image(extent, target_format);
    // When each target is first and last touched in the frame.
    std::size_t draw_count = plan_.draws.size();
    std::vector<std::size_t> first(plan_.targets.size(), draw_count);
    std::vector<std::size_t> last(plan_.targets.size(), 0);
    auto touch = [&](int target, std::size_t at) {
        if (target < 0) return;
        auto i = static_cast<std::size_t>(target);
        first[i] = std::min(first[i], at);
        last[i] = std::max(last[i], at);
    };
    for (std::size_t d = 0; d < draw_count; ++d) {
        const PlanDraw& p = plan_.draws[d];
        touch(p.target, d);
        for (const PlanTexture& t : p.textures) {
            if (t.kind == PlanTexture::Kind::target) touch(t.target, d);
        }
    }

    struct Want {
        std::size_t target;
        VkExtent2D extent;
        bool repeat;
    };
    std::vector<Want> wants;
    for (std::size_t i = 0; i < plan_.targets.size(); ++i) {
        const PlanTarget& t = plan_.targets[i];
        float sx = t.fixed ? t.size[0] : t.size[0] * canvas_scale_;
        float sy = t.fixed ? t.size[1] : t.size[1] * canvas_scale_;
        VkExtent2D e{std::clamp(static_cast<std::uint32_t>(std::lround(sx)), 1u, max_target),
                     std::clamp(static_cast<std::uint32_t>(std::lround(sy)), 1u, max_target)};
        bool repeat = t.repeat;
        // A layer wraps like the picture it holds: a scrolling effect moves a repeating
        // picture across its edge, and clamping would smear the edge into long bars.
        if (t.name == "layer a" || t.name == "layer b") {
            const ImageObject& owner = scene.images[static_cast<std::size_t>(t.object)];
            if (!owner.material.passes.empty() && !owner.material.passes[0].textures.empty() &&
                owner.material.passes[0].textures[0]) {
                repeat = !texture_of(*owner.material.passes[0].textures[0]).clamp;
            }
        }
        wants.push_back({i, e, repeat});
    }
    std::ranges::stable_sort(
        wants, [&](const Want& x, const Want& y) { return first[x.target] < first[y.target]; });
    targets_.assign(plan_.targets.size(), {});
    struct Holder {  // an image and the target using it
        Image* image;
        std::size_t target;
    };
    std::vector<Holder> holders;
    for (const Want& want : wants) {
        Image* found = nullptr;
        for (Holder& held : holders) {
            if (held.image->extent.width == want.extent.width &&
                held.image->extent.height == want.extent.height &&
                last[held.target] < first[want.target]) {
                found = held.image;
                held.target = want.target;
                break;
            }
        }
        if (found == nullptr) {
            images_.push_back(std::make_unique<Image>(make_image(want.extent, target_format)));
            found = images_.back().get();
            holders.push_back({found, want.target});
        }
        targets_[want.target] = {found, want.repeat};
        log::debug("scene: target {} ({}) {}x{}, draws {} to {}, image {}", want.target,
                   plan_.targets[want.target].name, want.extent.width, want.extent.height,
                   first[want.target], last[want.target], static_cast<const void*>(found));
    }

    // Particle systems keep running across rebuilds (a property change must not restart
    // the snow); they are made once per object and seeded by it, and remade only when a
    // property changes one of their overrides.
    if (systems_.size() != scene.particles.size()) systems_.clear();
    systems_.resize(scene.particles.size());
    for (std::size_t i = 0; i < scene.particles.size(); ++i) {
        const ParticleObject& object = scene.particles[i];
        ParticleOverride over = resolve_override(object.overrides, values_);
        if (systems_[i] && systems_[i]->override_values() == over) continue;
        if (systems_[i] && systems_[i]->override_values().count == over.count) {
            // The same particles, new numbers: no restart.
            systems_[i]->set_override(over);
            continue;
        }
        auto seed = static_cast<std::uint32_t>(object.id * 2654435761u) ^
                    static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this));
        systems_[i] = std::make_unique<ParticleSystem>(object.desc, over, seed);
        if (follows_pointer(*object.desc)) uses_pointer_ = true;
    }
    particle_models_.clear();
    particle_moving_.assign(scene.particles.size(), false);
    for (std::size_t i = 0; i < scene.particles.size(); ++i) {
        std::vector<int> visiting;
        particle_models_.push_back(particle_matrix(static_cast<int>(i), visiting, 0));
        const ParticleObject& p = scene.particles[i];
        if (p.origin.live() || p.scale.live() || p.angles.live()) particle_moving_[i] = true;
    }

    models_.clear();
    moving_.assign(scene.images.size(), false);
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        // A transform with a timeline, its own or an ancestor's, moves every frame.
        for (int o = static_cast<int>(i), guard = 0; o >= 0 && guard < 64; ++guard) {
            const ImageObject& im = scene.images[static_cast<std::size_t>(o)];
            if (im.origin.live() || im.scale.live() || im.angles.live()) {
                moving_[i] = true;
                break;
            }
            int next = -1;
            for (std::size_t k = 0; k < scene.images.size(); ++k) {
                if (scene.images[k].id == im.parent) next = static_cast<int>(k);
            }
            // Hung on a bone of a puppet that an animation moves.
            if (next >= 0 && !im.attachment.empty() &&
                scene.images[static_cast<std::size_t>(next)].puppet) {
                for (const AnimationLayer& layer :
                     scene.images[static_cast<std::size_t>(next)].animation_layers) {
                    if (layer.visible) moving_[i] = true;
                }
                if (moving_[i]) break;
            }
            o = next;
        }
    }
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        std::vector<int> visiting;
        models_.push_back(model_matrix(static_cast<int>(i), visiting, 0));
    }

    // Vertices: one quad (triangle strip, position and texcoord) per draw.
    std::vector<float> vertices;
    std::vector<std::uint16_t> indices;
    VkDeviceSize alignment = 256;
    VkDeviceSize uniform_bytes = 0;
    std::uint32_t particle_vertices = 0;
    std::uint32_t skinned_total = 0;
    for (const PlanDraw& p : plan_.draws) {
        Draw draw;
        draw.plan = &p;
        draw.target = p.target >= 0 ? targets_[static_cast<std::size_t>(p.target)].image : nullptr;
        draw.first_vertex = static_cast<std::uint32_t>(vertices.size() / 5);
        draw.first_index = static_cast<std::uint32_t>(indices.size());
        float u1 = 1, v1 = 1;
        if (p.particle >= 0) {
            // The system or one of its children, depth first, as the plan numbered them.
            ParticleSystem* system = systems_[static_cast<std::size_t>(p.particle)].get();
            if (p.child >= 0) {
                // -1 is the root; child k is the k-th system in depth-first order after it.
                int remaining = p.child;
                std::function<ParticleSystem*(ParticleSystem&)> walk =
                    [&](ParticleSystem& root) -> ParticleSystem* {
                    for (auto& child : root.children()) {
                        if (remaining == 0) return child.get();
                        --remaining;
                        if (ParticleSystem* found = walk(*child)) return found;
                    }
                    return nullptr;
                };
                system = walk(*system);
            }
            if (system == nullptr) continue;
            draw.particles = system;
            draw.particle_offset = particle_vertices;
            particle_vertices += system->capacity() * 4;
            const ParticleDesc& desc = system->desc();
            draw.extra["g_OrientationRight"] = {1, 0, 0};
            draw.extra["g_OrientationUp"] = {0, 1, 0};
            draw.extra["g_OrientationForward"] = {0, 0, 1};
            draw.extra["g_ViewRight"] = {1, 0, 0};
            draw.extra["g_ViewUp"] = {0, 1, 0};
            draw.extra["g_EyePosition"] = {0, 0, 100000};
            float trail_length = 0.05f, trail_max = 1e6f, trail_min = 0;
            for (const ParticleModule& r : desc.renderers) {
                if (r.name == "spritetrail" || r.name == "ropetrail" || r.name == "rope") {
                    auto get = [&](const char* key, float fallback) {
                        return r.json.contains(key) && r.json[key].is_number()
                                   ? r.json[key].get<float>()
                                   : fallback;
                    };
                    trail_length = get("length", trail_length);
                    trail_max = get("maxlength", trail_max);
                    trail_min = get("minlength", trail_min);
                }
            }
            draw.extra["g_RenderVar0"] = {trail_length, trail_max, trail_min, 0};
            float fw = 1, fh = 1, frames = 1, ratio = 1;
            if (p.textures[0].kind == PlanTexture::Kind::file) {
                const SceneTexture& t = texture_of(p.textures[0].file);
                ratio = static_cast<float>(t.tex_height) / static_cast<float>(t.tex_width);
                if (t.frames > 1) {
                    fw = t.frame_width;
                    fh = t.frame_height;
                    frames = static_cast<float>(t.frames);
                    ratio = (fh * static_cast<float>(t.tex_height)) /
                            (fw * static_cast<float>(t.tex_width));
                }
            }
            draw.extra["g_RenderVar1"] = {fw, fh, frames, ratio};
        }
        // A puppet's layer holds the whole stored texture, padding included: its mesh uvs
        // index the stored texture and reach past the picture's edge (a few texels of
        // padding), which a cropped layer would answer with its last row, a thin streak.
        bool puppet_object =
            p.object >= 0 && scene.images[static_cast<std::size_t>(p.object)].puppet &&
            assets_->mesh(scene.images[static_cast<std::size_t>(p.object)].puppet_path) != nullptr;
        if (p.crop && !puppet_object && p.textures[0].kind == PlanTexture::Kind::file) {
            const SceneTexture& t = texture_of(p.textures[0].file);
            u1 = static_cast<float>(t.img_width) / static_cast<float>(t.tex_width);
            v1 = static_cast<float>(t.img_height) / static_cast<float>(t.tex_height);
        }
        auto quad = [&](float x0, float y0, float x1, float y1) {
            // Top-left, top-right, bottom-left, bottom-right; two triangles.
            float q[] = {x0, y0, 0, 0, 0, x1, y0, 0, u1, 0, x0, y1, 0, 0, v1, x1, y1, 0, u1, v1};
            vertices.insert(vertices.end(), std::begin(q), std::end(q));
            std::uint16_t i[] = {0, 1, 2, 2, 1, 3};
            indices.insert(indices.end(), std::begin(i), std::end(i));
            draw.index_count = 6;
        };
        static const ImageObject no_object;
        const ImageObject& object =
            p.object >= 0 ? scene.images[static_cast<std::size_t>(p.object)] : no_object;
        const Mesh* mesh = p.geometry == Geometry::object && object.puppet
                               ? assets_->mesh(object.puppet_path)
                               : nullptr;
        if (p.particle >= 0) {
            // The quads come from the simulation each frame.
        } else if (mesh != nullptr && !p.request.name.empty()) {
            // The rest mesh: its positions are already relative to the object's origin. Its
            // uvs index the picture; drawn from a layer they are scaled to the layer's crop.
            // The mesh uvs index the stored texture, and so does its layer (see the copy
            // above), so they pass unchanged.
            float su = 1, sv = 1;
            std::size_t base = vertices.size();
            for (std::size_t i = 0; i < mesh->vertices.size(); i += 5) {
                vertices.push_back(mesh->vertices[i]);
                vertices.push_back(mesh->vertices[i + 1]);
                vertices.push_back(0);
                vertices.push_back(mesh->vertices[i + 3] * su);
                vertices.push_back(mesh->vertices[i + 4] * sv);
            }
            indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());
            draw.index_count = static_cast<std::uint32_t>(mesh->indices.size());
            // Posed by an animation layer that names one of the mesh's animations: skinned
            // on the CPU every frame into a buffer of its own.
            bool posed = false;
            for (const AnimationLayer& layer : object.animation_layers) {
                if (layer.visible && mesh->animation(layer.animation) != nullptr) posed = true;
            }
            if (posed && !mesh->bones.empty() &&
                mesh->bone_indices.size() * 5 == mesh->vertices.size()) {
                draw.skin = mesh;
                draw.skin_base.assign(vertices.begin() + static_cast<std::ptrdiff_t>(base),
                                      vertices.end());
                draw.skin_offset = skinned_total;
                skinned_total += static_cast<std::uint32_t>(draw.skin_base.size() / 5);
            }
        } else if (p.geometry == Geometry::object) {
            Vec2 size = object_size(object, general, values_, size_of);
            float x = size[0] * 0.5f, y = size[1] * 0.5f;
            // Where the origin sits in the quad: its centre, or an edge or corner.
            float ox = 0, oy = 0;
            const std::string& a = object.alignment;
            if (a.find("left") != std::string::npos) ox = x;
            if (a.find("right") != std::string::npos) ox = -x;
            if (a.find("top") != std::string::npos) oy = -y;
            if (a.find("bottom") != std::string::npos) oy = y;
            quad(-x + ox, y + oy, x + ox, -y + oy);
        } else {
            quad(-1, -1, 1, 1);
        }

        if (p.request.name.empty()) {
            // glint's copy: the picture (or a target) into the layer.
            draw.copy = true;
            draw.copy_uv[0] = u1;
            draw.copy_uv[1] = v1;
            const PlanTexture& t = p.textures[0];
            if (t.kind == PlanTexture::Kind::file) {
                const SceneTexture& tex = texture_of(t.file);
                draw.copy_view = tex.texture ? tex.texture->view() : VK_NULL_HANDLE;
                draw.copy_sampler = assets_->sampler(true, tex.nearest);
            } else if (t.kind == PlanTexture::Kind::framebuffer) {
                draw.copy_view = framebuffer_.view;
                draw.framebuffer = true;
                draw.copy_sampler = assets_->sampler(true, false);
            } else {
                const SceneTexture& white = assets_->texture("");
                draw.copy_view = white.texture ? white.texture->view() : VK_NULL_HANDLE;
                draw.copy_sampler = assets_->sampler(true, false);
            }
            draws_.push_back(std::move(draw));
            continue;
        }

        draw.program = assets_->program(p.request);
        if (draw.program == nullptr && scripts_) {
            // A variant a made layer needs that the load did not see.
            draw.program = assets_->compile_program(p.request);
        }
        if (draw.program == nullptr) continue;  // the shader did not compile: skipped
        draw.pipeline =
            assets_->pipeline(*draw.program, p.blending, target_format,
                              p.particle >= 0 ? VertexLayout::particle : VertexLayout::image);
        if (draw.pipeline == VK_NULL_HANDLE) continue;
        for (std::size_t i = 0; i < max_texture_slots; ++i) {
            const PlanTexture& t = p.textures[i];
            const SceneTexture& white = assets_->texture("");
            draw.views[i] = white.texture ? white.texture->view() : VK_NULL_HANDLE;
            draw.samplers[i] = assets_->sampler(false, false);
            if (t.kind == PlanTexture::Kind::file) {
                const SceneTexture& tex = texture_of(t.file);
                if (tex.texture) draw.views[i] = tex.texture->view();
                draw.samplers[i] = assets_->sampler(
                    tex.clamp, tex.nearest || std::getenv("GLINT_NEAREST") != nullptr);
            } else if (t.kind == PlanTexture::Kind::target) {
                const TargetUse& use = targets_[static_cast<std::size_t>(t.target)];
                draw.views[i] = use.image->view;
                draw.sampled[i] = use.image;
                draw.samplers[i] = assets_->sampler(!use.repeat, false);
            } else if (t.kind == PlanTexture::Kind::framebuffer) {
                draw.views[i] = framebuffer_.view;
                draw.sampled[i] = &framebuffer_;
                draw.framebuffer = true;
                draw.samplers[i] = assets_->sampler(true, false);
            }
        }
        // A sampler's annotated default fills a slot the material leaves empty: the scene so
        // far (_rt_FullFrameBuffer, which refraction reads) or a texture file.
        for (const ShaderSampler& sampler : draw.program->shader.samplers) {
            if (sampler.slot < 0 || static_cast<std::size_t>(sampler.slot) >= max_texture_slots ||
                sampler.fallback.empty()) {
                continue;
            }
            auto slot = static_cast<std::size_t>(sampler.slot);
            if (p.textures[slot].kind != PlanTexture::Kind::none) continue;
            if (sampler.fallback == "_rt_FullFrameBuffer") {
                if (framebuffer_.image == VK_NULL_HANDLE) {
                    framebuffer_ = make_image(extent, target_format);
                }
                draw.views[slot] = framebuffer_.view;
                draw.sampled[slot] = &framebuffer_;
                draw.framebuffer = true;
                draw.samplers[slot] = assets_->sampler(true, false);
            } else if (!sampler.fallback.starts_with("_rt_") &&
                       !sampler.fallback.starts_with("_alias_")) {
                const SceneTexture& tex = texture_of(sampler.fallback);
                if (tex.texture) {
                    draw.views[slot] = tex.texture->view();
                    draw.samplers[slot] = assets_->sampler(tex.clamp, tex.nearest);
                }
            }
        }
        draw.block.assign(std::max<std::uint32_t>(draw.program->shader.block_size, 16), {});
        draw.block_offset = uniform_bytes;
        uniform_bytes += (draw.block.size() + alignment - 1) / alignment * alignment;
        fill_block(draw);
        if (log::enabled(log::Level::debug)) {
            std::string attrs;
            for (const auto& [name, loc] : draw.program->shader.attributes) {
                attrs += name + "@" + std::to_string(loc) + " ";
            }
            log::debug("scene draw {}: {} indices from {} at vertex {}, target {}, {}",
                       p.request.name, draw.index_count, draw.first_index, draw.first_vertex,
                       p.target, attrs);
        }
        draws_.push_back(std::move(draw));
    }
    vertices_ = gpu::Buffer(gpu_.allocator(), std::max<VkDeviceSize>(vertices.size() * 4, 64),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    if (vertices_ && !vertices.empty()) {
        std::memcpy(vertices_.mapped(), vertices.data(), vertices.size() * 4);
        vertices_.flush(0, VK_WHOLE_SIZE);
    }
    indices_ = gpu::Buffer(gpu_.allocator(), std::max<VkDeviceSize>(indices.size() * 2, 64),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    if (indices_ && !indices.empty()) {
        std::memcpy(indices_.mapped(), indices.data(), indices.size() * 2);
        indices_.flush(0, VK_WHOLE_SIZE);
    }
    // Skinned puppet vertices: a slot per frame in flight.
    skinned_capacity_ = skinned_total;
    for (gpu::Buffer& b : skinned_vertices_) {
        b = skinned_total > 0
                ? gpu::Buffer(gpu_.allocator(), static_cast<VkDeviceSize>(skinned_total) * 20,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)
                : gpu::Buffer();
    }
    // The particle quads: a slot per frame in flight, and u32 indices for every quad.
    particle_capacity_ = particle_vertices / 4;
    for (gpu::Buffer& b : particle_vertices_) b = gpu::Buffer();
    particle_indices_ = gpu::Buffer();
    if (particle_capacity_ > 0) {
        VkDeviceSize bytes = static_cast<VkDeviceSize>(particle_vertices) * sizeof(ParticleVertex);
        for (gpu::Buffer& b : particle_vertices_) {
            b = gpu::Buffer(gpu_.allocator(), bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        }
        std::vector<std::uint32_t> quads;
        quads.reserve(static_cast<std::size_t>(particle_capacity_) * 6);
        for (std::uint32_t q = 0; q < particle_capacity_; ++q) {
            std::uint32_t b = q * 4;
            quads.insert(quads.end(), {b, b + 1, b + 2, b + 2, b + 1, b + 3});
        }
        particle_indices_ =
            gpu::Buffer(gpu_.allocator(), quads.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        if (particle_indices_) {
            std::memcpy(particle_indices_.mapped(), quads.data(), quads.size() * 4);
            particle_indices_.flush(0, VK_WHOLE_SIZE);
        }
    }
    uniform_slot_ = std::max<VkDeviceSize>(uniform_bytes, alignment);
    uniforms_ =
        gpu::Buffer(gpu_.allocator(), uniform_slot_ * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    stale_ = false;
    log::debug("scene: {} draws, {} targets in {} images, {} KB of uniforms per frame",
               draws_.size(), targets_.size(), images_.size(), uniform_slot_ / 1024);
}

void SceneRenderer::fill_block(Draw& draw) {
    const PlanDraw& p = *draw.plan;
    const CompiledShader& shader = draw.program->shader;
    const SceneDesc& scene = scene_desc();
    std::vector<std::byte>& block = draw.block;

    bool object = p.geometry == Geometry::object;
    math::Mat4 model = !object           ? math::Mat4{}
                       : p.particle >= 0 ? particle_models_[static_cast<std::size_t>(p.particle)]
                                         : models_[static_cast<std::size_t>(p.object)];
    math::Mat4 view_projection = object ? projection_ : math::Mat4{};
    math::Mat4 mvp = view_projection * model;

    VkExtent2D size = draw.target != nullptr ? draw.target->extent : extent_;
    static const ImageObject no_object;
    const ImageObject& image =
        p.object >= 0 ? scene.images[static_cast<std::size_t>(p.object)] : no_object;
    bool material = p.effect < 0 && p.particle < 0;
    bool moving = p.object >= 0 && (parallax_ || moving_[static_cast<std::size_t>(p.object)]);
    if (p.particle >= 0)
        moving = parallax_ || particle_moving_[static_cast<std::size_t>(p.particle)];
    Vec3 color = parse_vec3(image.color.resolve(values_), {1, 1, 1});
    float alpha = image.alpha.resolve_float(values_, 1);
    float brightness = image.brightness.resolve_float(values_, 1);

    for (const ShaderUniform& u : shader.uniforms) {
        // Defaults from the annotation, then the material, then the effect's overrides,
        // then the property it is bound to.
        nlohmann::json value = u.fallback;
        if (!u.material.empty()) {
            if (auto it = p.constants.find(u.material); it != p.constants.end()) value = it->second;
            if (auto it = p.bound.find(u.material); it != p.bound.end()) {
                value = it->second.resolve(values_);
            }
        }
        auto user = p.user.find(u.material.empty() ? u.name : u.material);
        if (user == p.user.end()) user = p.user.find(u.name);
        if (user != p.user.end()) {
            if (auto v = values_.find(user->second); v != values_.end()) value = v->second;
        }
        write(block, u, floats_of(value));
        if (!u.material.empty()) {
            if (auto it = p.bound.find(u.material); it != p.bound.end() && it->second.live()) {
                Dynamic d{Dynamic::What::timeline, u.offset};
                d.bound = &it->second;
                d.uniform = &u;
                draw.dynamics.push_back(d);
            }
        }

        const std::string& n = u.name;
        auto vec = [&](std::initializer_list<float> f) { write(block, u, std::vector<float>(f)); };
        if (auto extra = draw.extra.find(n); extra != draw.extra.end()) {
            write(block, u, extra->second);
        } else if (n == "g_ModelViewProjectionMatrix") {
            write_matrix(block, u.offset, mvp);
            if (object && moving) draw.dynamics.push_back({Dynamic::What::mvp, u.offset});
        } else if (n == "g_ModelViewProjectionMatrixInverse") {
            write_matrix(block, u.offset, mvp.inverse());
            if (object && moving) draw.dynamics.push_back({Dynamic::What::mvp_inverse, u.offset});
        } else if (n == "g_ModelMatrix") {
            write_matrix(block, u.offset, model);
            if (object && moving) draw.dynamics.push_back({Dynamic::What::model, u.offset});
        } else if (n == "g_ModelMatrixInverse") {
            write_matrix(block, u.offset, model.inverse());
            if (object && moving) draw.dynamics.push_back({Dynamic::What::model_inverse, u.offset});
        } else if (n == "g_ViewProjectionMatrix") {
            write_matrix(block, u.offset, view_projection);
        } else if (n == "g_EffectTextureProjectionMatrix" ||
                   n == "g_EffectTextureProjectionMatrixInverse" || n == "g_AltModelMatrix" ||
                   n == "g_AltViewProjectionMatrix") {
            write_matrix(block, u.offset, {});
        } else if (n == "g_NormalModelMatrix" || n == "g_AltNormalModelMatrix") {
            vec({1, 0, 0, 0, 1, 0, 0, 0, 1});
        } else if (n == "g_Time") {
            draw.dynamics.push_back({Dynamic::What::time, u.offset});
        } else if (n == "g_Daytime") {
            draw.dynamics.push_back({Dynamic::What::daytime, u.offset});
        } else if (n == "g_Frametime") {
            draw.dynamics.push_back({Dynamic::What::frametime, u.offset});
        } else if (n == "g_PointerPosition") {
            draw.dynamics.push_back({Dynamic::What::pointer, u.offset});
            uses_pointer_ = true;
        } else if (n == "g_PointerPositionLast") {
            draw.dynamics.push_back({Dynamic::What::pointer_last, u.offset});
        } else if (n.starts_with("g_AudioSpectrum")) {
            // g_AudioSpectrum64Left and the like: arrays filled every frame.
            std::string rest = n.substr(15);
            int bands = rest.starts_with("64") ? 64 : rest.starts_with("32") ? 32 : 16;
            draw.dynamics.push_back(
                {Dynamic::What::audio, u.offset, bands, rest.ends_with("Right")});
            uses_audio_ = true;
        } else if (n == "g_ParallaxPosition") {
            draw.dynamics.push_back({Dynamic::What::parallax, u.offset});
        } else if (n == "g_Screen") {
            auto sw = static_cast<float>(extent_.width), sh = static_cast<float>(extent_.height);
            vec({sw, sh, sw / sh});
        } else if (n == "g_TexelSize") {
            vec({1.0f / static_cast<float>(size.width), 1.0f / static_cast<float>(size.height)});
        } else if (n == "g_TexelSizeHalf") {
            vec({0.5f / static_cast<float>(size.width), 0.5f / static_cast<float>(size.height)});
        } else if (n == "g_Color4") {
            if (material) {
                vec({color[0] * brightness, color[1] * brightness, color[2] * brightness, alpha});
                if (image.color.live() || image.brightness.live()) {
                    Dynamic d{Dynamic::What::color4, u.offset};
                    d.image = &image;
                    draw.dynamics.push_back(d);
                } else if (image.alpha.live()) {
                    // Only the alpha moves; it is the fourth component.
                    Dynamic d{Dynamic::What::alpha, u.offset + 12};
                    d.bound = &image.alpha;
                    draw.dynamics.push_back(d);
                }
            } else {
                vec({1, 1, 1, 1});
            }
        } else if (material && n == "g_Color") {
            vec({color[0], color[1], color[2]});
            if (image.color.live()) {
                Dynamic d{Dynamic::What::color, u.offset};
                d.image = &image;
                draw.dynamics.push_back(d);
            }
        } else if (material && (n == "g_UserAlpha" || n == "g_Alpha")) {
            vec({alpha});
            if (image.alpha.live()) {
                Dynamic d{Dynamic::What::alpha, u.offset};
                d.bound = &image.alpha;
                draw.dynamics.push_back(d);
            }
        } else if (material && n == "g_Brightness") {
            vec({brightness});
            if (image.brightness.live()) {
                Dynamic d{Dynamic::What::brightness, u.offset};
                d.image = &image;
                draw.dynamics.push_back(d);
            }
        } else if (n.starts_with("g_Texture") && n.size() > 9 && std::isdigit(n[9])) {
            std::size_t slot = static_cast<std::size_t>(n[9] - '0');
            std::string rest = n.substr(10);
            if (slot >= max_texture_slots) continue;
            const PlanTexture& t = p.textures[slot];
            float tw = 1, th = 1, iw = 1, ih = 1, levels = 1;
            if (t.kind == PlanTexture::Kind::file) {
                const SceneTexture& tex = texture_of(t.file);
                tw = static_cast<float>(tex.tex_width);
                th = static_cast<float>(tex.tex_height);
                iw = static_cast<float>(tex.img_width);
                ih = static_cast<float>(tex.img_height);
                levels = tex.texture ? static_cast<float>(tex.texture->levels()) : 1;
            } else if (t.kind == PlanTexture::Kind::target) {
                const Image& image2 = *targets_[static_cast<std::size_t>(t.target)].image;
                tw = iw = static_cast<float>(image2.extent.width);
                th = ih = static_cast<float>(image2.extent.height);
            } else if (t.kind == PlanTexture::Kind::framebuffer) {
                tw = iw = static_cast<float>(extent_.width);
                th = ih = static_cast<float>(extent_.height);
            }
            if (rest == "Resolution") {
                vec({tw, th, iw, ih});
            } else if (rest == "Rotation") {
                vec({1, 0, 0, 1});
            } else if (rest == "Translation") {
                vec({0, 0});
            } else if (rest == "MipMapInfo") {
                vec({levels});
            } else if (rest == "Texel") {
                vec({1.0f / tw, 1.0f / th, tw, th});
            }
        }
    }
}

math::Mat4 SceneRenderer::particle_model(int particle, const FrameInputs& inputs) const {
    const ParticleObject& object = scene_desc().particles[static_cast<std::size_t>(particle)];
    const Vec2& canvas = scene_desc().general.ortho;
    float dx = -(inputs.parallax[0] - 0.5f) * parallax_amount_ * object.parallax_depth[0] *
               canvas[0] * 0.2f;
    float dy = (inputs.parallax[1] - 0.5f) * parallax_amount_ * object.parallax_depth[1] *
               canvas[1] * 0.2f;
    if (!parallax_) dx = dy = 0;
    math::Mat4 base = particle_models_[static_cast<std::size_t>(particle)];
    if (particle_moving_[static_cast<std::size_t>(particle)]) {
        std::vector<int> visiting;
        base = particle_matrix(particle, visiting, inputs.time);
    }
    return math::Mat4::translate(dx, dy, 0) * base;
}

math::Mat4 SceneRenderer::parallax_model(int object, const FrameInputs& inputs) const {
    const ImageObject& image = scene_desc().images[static_cast<std::size_t>(object)];
    const Vec2& canvas = scene_desc().general.ortho;
    // The layer moves against the pointer by the scene's amount times its depth. The scale
    // (a tenth of the canvas at full amount) is glint's reading of how far Wallpaper Engine
    // moves layers; it is checked by eye, not against a reference.
    float dx = -(inputs.parallax[0] - 0.5f) * parallax_amount_ * image.parallax_depth[0] *
               canvas[0] * 0.2f;
    float dy =
        (inputs.parallax[1] - 0.5f) * parallax_amount_ * image.parallax_depth[1] * canvas[1] * 0.2f;
    if (!parallax_) {
        dx = 0;
        dy = 0;
    }
    math::Mat4 base = models_[static_cast<std::size_t>(object)];
    if (moving_[static_cast<std::size_t>(object)]) {
        std::vector<int> visiting;
        base = model_matrix(object, visiting, inputs.time);
    }
    return math::Mat4::translate(dx, dy, 0) * base;
}

void SceneRenderer::transition(VkCommandBuffer cmd, Image& image, VkImageLayout layout) {
    if (image.image == VK_NULL_HANDLE || image.layout == layout) return;
    barrier(cmd, image.image, image.layout, layout);
    image.layout = layout;
}

void SceneRenderer::record(VkCommandBuffer cmd, VkExtent2D extent, const FrameInputs& inputs) {
    if (stale_ || extent.width != extent_.width || extent.height != extent_.height) {
        build(extent);
    }
    if (scripts_) {
        // The scripts run first: what they write this frame is drawn this frame. What
        // changes the plan (visibility, a text) rebuilds it before the draws.
        ScriptFrame f = script_frame(inputs);
        if (!scripts_started_) {
            scripts_->start(values_, f);
            scripts_started_ = true;
        }
        bool down_edge = (inputs.buttons & 1u) != 0 && (last_buttons_ & 1u) == 0;
        bool up_edge = (inputs.buttons & 1u) == 0 && (last_buttons_ & 1u) != 0;
        last_buttons_ = inputs.buttons;
        scripts_->frame(values_, f);
        if (scripts_->uses_pointer()) scripts_->cursor(values_, f, down_edge, up_edge);
        if (scripts_->uses_media()) {
            ScriptHost::MediaEvents events;
            events.status = !media_status_sent_;
            events.properties = media_.properties_version != media_seen_properties_;
            events.playback = media_.playback_version != media_seen_playback_;
            events.thumbnail = media_.thumbnail_version != media_seen_thumbnail_;
            events.timeline = media_.available && media_.playback == 1;
            media_status_sent_ = true;
            media_seen_properties_ = media_.properties_version;
            media_seen_playback_ = media_.playback_version;
            media_seen_thumbnail_ = media_.thumbnail_version;
            scripts_->media(values_, media_, events);
        }
        for (int id : scripts_->take_changed_texts()) update_text(id, inputs.time);
        if (scripts_->structure_version() != built_structure_) stale_ = true;
        if (stale_) build(extent);
    }
    if (scene_.image == VK_NULL_HANDLE || !uniforms_) return;

    // Uniform blocks: the static part as built, the dynamic part patched for this frame.
    frame_ ^= 1u;
    VkDeviceSize slot_base = uniform_slot_ * frame_;
    for (Draw& draw : draws_) {
        if (draw.copy) continue;
        math::Mat4 model, mvp;
        bool moved = false;
        for (const Dynamic& d : draw.dynamics) {
            auto put = [&](std::initializer_list<float> f) {
                std::size_t i = 0;
                for (float v : f) {
                    if (d.offset + i * 4 + 4 <= draw.block.size()) {
                        std::memcpy(draw.block.data() + d.offset + i * 4, &v, 4);
                    }
                    ++i;
                }
            };
            if (!moved && d.what >= Dynamic::What::mvp && d.what <= Dynamic::What::model_inverse) {
                model = draw.plan->particle >= 0 ? particle_model(draw.plan->particle, inputs)
                                                 : parallax_model(draw.plan->object, inputs);
                mvp = projection_ * model;
                moved = true;
            }
            switch (d.what) {
                case Dynamic::What::time: put({static_cast<float>(inputs.time)}); break;
                case Dynamic::What::daytime: put({static_cast<float>(inputs.daytime)}); break;
                case Dynamic::What::frametime: put({static_cast<float>(inputs.delta)}); break;
                case Dynamic::What::pointer: put({inputs.pointer[0], inputs.pointer[1]}); break;
                case Dynamic::What::pointer_last:
                    put({inputs.pointer_last[0], inputs.pointer_last[1]});
                    break;
                case Dynamic::What::parallax: put({inputs.parallax[0], inputs.parallax[1]}); break;
                case Dynamic::What::mvp: write_matrix(draw.block, d.offset, mvp); break;
                case Dynamic::What::mvp_inverse:
                    write_matrix(draw.block, d.offset, mvp.inverse());
                    break;
                case Dynamic::What::model: write_matrix(draw.block, d.offset, model); break;
                case Dynamic::What::model_inverse:
                    write_matrix(draw.block, d.offset, model.inverse());
                    break;
                case Dynamic::What::timeline:
                    write(draw.block, *d.uniform, floats_of(d.bound->at(values_, inputs.time)));
                    break;
                case Dynamic::What::alpha: {
                    float a = d.bound->at(values_, inputs.time).is_number()
                                  ? d.bound->at(values_, inputs.time).get<float>()
                                  : 1.0f;
                    if (d.offset + 4 <= draw.block.size()) {
                        std::memcpy(draw.block.data() + d.offset, &a, 4);
                    }
                    break;
                }
                case Dynamic::What::color4:
                case Dynamic::What::color:
                case Dynamic::What::brightness: {
                    const ImageObject& im = *d.image;
                    Vec3 c = parse_vec3(im.color.at(values_, inputs.time), {1, 1, 1});
                    nlohmann::json bj = im.brightness.at(values_, inputs.time);
                    float b = bj.is_number() ? bj.get<float>() : 1.0f;
                    nlohmann::json aj = im.alpha.at(values_, inputs.time);
                    float a = aj.is_number() ? aj.get<float>() : 1.0f;
                    if (d.what == Dynamic::What::color4) {
                        put({c[0] * b, c[1] * b, c[2] * b, a});
                    } else if (d.what == Dynamic::What::color) {
                        put({c[0], c[1], c[2]});
                    } else {
                        put({b});
                    }
                    break;
                }
                case Dynamic::What::audio: {
                    const float* source = d.right ? inputs.audio_right : inputs.audio_left;
                    for (int k = 0; k < d.bands; ++k) {
                        float v = 0;
                        if (source != nullptr) {
                            // 32 and 16 bands are means of 2 and 4 neighbouring ones.
                            int group = 64 / d.bands;
                            for (int g = 0; g < group; ++g) v += source[k * group + g];
                            v /= static_cast<float>(group);
                        }
                        std::size_t at = d.offset + static_cast<std::size_t>(k) * 16;
                        if (at + 4 <= draw.block.size()) std::memcpy(draw.block.data() + at, &v, 4);
                    }
                    break;
                }
            }
        }
        std::memcpy(uniforms_.mapped() + slot_base + draw.block_offset, draw.block.data(),
                    draw.block.size());
    }
    uniforms_.flush(slot_base, uniform_slot_);

    // The particles: each root system steps once (its children with it), then every draw
    // writes its quads into this frame's slot.
    if (particle_capacity_ > 0 && particle_vertices_[frame_]) {
        const SceneDesc& scene = scene_desc();
        for (std::size_t i = 0; i < systems_.size(); ++i) {
            ParticleEnv env;
            env.delta = inputs.delta;
            env.time = inputs.time;
            if (live_override(scene.particles[i].overrides)) {
                ParticleOverride over = resolve_override(scene.particles[i].overrides, values_);
                if (!(systems_[i]->override_values() == over)) systems_[i]->set_override(over);
            }
            Vec3 origin = parse_vec3(scene.particles[i].origin.resolve(values_), {0, 0, 0});
            env.pointer = {canvas_left_ + inputs.pointer[0] * canvas_width_ - origin[0],
                           canvas_bottom_ + (1 - inputs.pointer[1]) * canvas_height_ - origin[1],
                           0};
            systems_[i]->step(env);
        }
        auto* base = reinterpret_cast<ParticleVertex*>(particle_vertices_[frame_].mapped());
        for (Draw& draw : draws_) {
            if (draw.particles == nullptr) continue;
            float ratio = 1;
            if (auto it = draw.extra.find("g_RenderVar1"); it != draw.extra.end()) {
                ratio = it->second[3];
            }
            std::uint32_t count = draw.particles->write(base + draw.particle_offset,
                                                        draw.particles->capacity(), ratio);
            draw.index_count = count * 6;
        }
        particle_vertices_[frame_].flush(0, VK_WHOLE_SIZE);
    }

    // Puppets: the animation layers' pose, blended, skins the rest-pose vertices.
    if (skinned_capacity_ > 0 && skinned_vertices_[frame_]) {
        const SceneDesc& scene = scene_desc();
        auto* out = reinterpret_cast<float*>(skinned_vertices_[frame_].mapped());
        std::vector<math::Mat4> locals, skin;
        for (Draw& draw : draws_) {
            if (draw.skin == nullptr) continue;
            const Mesh& mesh = *draw.skin;
            const ImageObject& object = scene.images[static_cast<std::size_t>(draw.plan->object)];
            pose_of(object, mesh, inputs.time, locals);
            mesh.skin_matrices(locals, skin);
            const std::vector<float>& base = draw.skin_base;
            float* dst = out + static_cast<std::size_t>(draw.skin_offset) * 5;
            for (std::size_t v = 0; v < base.size() / 5; ++v) {
                float x = base[v * 5], y = base[v * 5 + 1];
                float px = 0, py = 0;
                const auto& bi = mesh.bone_indices[v];
                const auto& bw = mesh.bone_weights[v];
                for (int k = 0; k < 4; ++k) {
                    float w = bw[static_cast<std::size_t>(k)];
                    auto bone = static_cast<std::size_t>(bi[static_cast<std::size_t>(k)]);
                    if (w == 0 || bone >= skin.size()) continue;
                    const math::Mat4& m = skin[bone];
                    px += w * (m.m[0] * x + m.m[4] * y + m.m[12]);
                    py += w * (m.m[1] * x + m.m[5] * y + m.m[13]);
                }
                dst[v * 5] = px;
                dst[v * 5 + 1] = py;
                dst[v * 5 + 2] = 0;
                dst[v * 5 + 3] = base[v * 5 + 3];
                dst[v * 5 + 4] = base[v * 5 + 4];
            }
        }
        skinned_vertices_[frame_].flush(0, VK_WHOLE_SIZE);
    }

    // The scene target starts as the clear colour.
    transition(cmd, scene_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    {
        VkRenderingAttachmentInfo color{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = scene_.view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        float c[4] = {clear_[0], clear_[1], clear_[2], 1};
        if (!clear_enabled_) c[0] = c[1] = c[2] = 0;
        std::memcpy(color.clearValue.color.float32, c, sizeof(c));
        VkRenderingInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, extent_},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
        };
        vkCmdBeginRendering(cmd, &info);
        vkCmdEndRendering(cmd);
    }

    for (Draw& draw : draws_) {
        if (!draw.copy && draw.pipeline == VK_NULL_HANDLE) continue;

        if (draw.framebuffer && framebuffer_.image != VK_NULL_HANDLE) {
            // _rt_FullFrameBuffer: what the scene holds so far.
            transition(cmd, scene_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transition(cmd, framebuffer_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .extent = {extent_.width, extent_.height, 1},
            };
            vkCmdCopyImage(cmd, scene_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           framebuffer_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition(cmd, framebuffer_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        for (Image* sampled : draw.sampled) {
            if (sampled != nullptr)
                transition(cmd, *sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        Image& target = draw.target != nullptr ? *draw.target : scene_;
        transition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo color{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = target.view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = draw.plan->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        VkRenderingInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, target.extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
        };
        vkCmdBeginRendering(cmd, &info);
        VkViewport viewport{
            0, 0, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height),
            0, 1};
        VkRect2D scissor{{0, 0}, target.extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (draw.copy) {
            if (draw.copy_view != VK_NULL_HANDLE) {
                gpu::Blit::Params params{static_cast<float>(target.extent.width),
                                         static_cast<float>(target.extent.height), draw.copy_uv[0],
                                         draw.copy_uv[1]};
                blit_.draw(cmd, target_format, draw.copy_view, draw.copy_sampler, params);
            }
            vkCmdEndRendering(cmd);
            continue;
        }

        if (draw.particles != nullptr && draw.index_count == 0) {
            vkCmdEndRendering(cmd);
            continue;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
        bool particle = draw.particles != nullptr;
        bool skinned = draw.skin != nullptr && skinned_vertices_[frame_];
        VkBuffer buffers[] = {particle  ? particle_vertices_[frame_].get()
                              : skinned ? skinned_vertices_[frame_].get()
                                        : vertices_.get(),
                              zeros_.get()};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        if (particle) {
            vkCmdBindIndexBuffer(cmd, particle_indices_.get(), 0, VK_INDEX_TYPE_UINT32);
        } else {
            vkCmdBindIndexBuffer(cmd, indices_.get(), 0, VK_INDEX_TYPE_UINT16);
        }

        VkDescriptorBufferInfo block{uniforms_.get(), slot_base + draw.block_offset,
                                     draw.block.size()};
        std::array<VkDescriptorImageInfo, max_texture_slots + 7> images{};
        std::vector<VkWriteDescriptorSet> writes;
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = 0,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                          .pBufferInfo = &block});
        std::size_t used = 0;
        for (const ShaderSampler& s : draw.program->shader.samplers) {
            if (used >= images.size()) break;
            VkImageView view = assets_->texture("").texture ? assets_->texture("").texture->view()
                                                            : VK_NULL_HANDLE;
            VkSampler sampler = assets_->sampler(false, false);
            if (s.slot >= 0 && static_cast<std::size_t>(s.slot) < max_texture_slots) {
                view = draw.views[static_cast<std::size_t>(s.slot)];
                sampler = draw.samplers[static_cast<std::size_t>(s.slot)];
            }
            if (view == VK_NULL_HANDLE) continue;
            images[used] = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstBinding = s.binding,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              .pImageInfo = &images[used]});
            ++used;
        }
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, assets_->layout(), 0,
                                  static_cast<std::uint32_t>(writes.size()), writes.data());
        if (particle) {
            vkCmdDrawIndexed(cmd, draw.index_count, 1, 0,
                             static_cast<std::int32_t>(draw.particle_offset), 0);
        } else {
            vkCmdDrawIndexed(
                cmd, draw.index_count, 1, draw.first_index,
                static_cast<std::int32_t>(skinned ? draw.skin_offset : draw.first_vertex), 0);
        }
        vkCmdEndRendering(cmd);
    }
    transition(cmd, scene_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void SceneRenderer::composite(const gpu::Target& target, float opacity) {
    if (scene_.view == VK_NULL_HANDLE) return;
    gpu::Blit::Params params{static_cast<float>(target.extent.width),
                             static_cast<float>(target.extent.height)};
    params.opacity = opacity;
    params.decode = !target.encode_srgb;  // an sRGB swapchain: undo its encode
    params.blend = opacity < 1.0f;
    blit_.draw(target.cmd, target.format, scene_.view, blit_.linear(), params);
}

std::uint64_t SceneRenderer::memory() const {
    std::uint64_t total = scene_.memory + framebuffer_.memory;
    for (const auto& t : images_) total += t->memory;
    return total;
}

}  // namespace glint::scene

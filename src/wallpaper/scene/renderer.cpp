#include "wallpaper/scene/renderer.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
}

void SceneRenderer::set_values(const Values& values) {
    values_ = values;
    stale_ = true;
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
    for (Image& t : targets_) destroy_image(t);
    targets_.clear();
    draws_.clear();
    // Buffers may be read by frames in flight.
    auto vertices = std::make_shared<gpu::Buffer>(std::move(vertices_));
    auto indices = std::make_shared<gpu::Buffer>(std::move(indices_));
    auto uniforms = std::make_shared<gpu::Buffer>(std::move(uniforms_));
    gpu_.defer([vertices, indices, uniforms]() mutable {
        vertices.reset();
        indices.reset();
        uniforms.reset();
    });
    extent_ = {0, 0};
}

math::Mat4 SceneRenderer::model_matrix(int object, std::vector<int>& visiting) const {
    const SceneDesc& scene = assets_->scene();
    const ImageObject& image = scene.images[static_cast<std::size_t>(object)];
    Vec3 origin = parse_vec3(image.origin.resolve(values_), {0, 0, 0});
    Vec3 scale = parse_vec3(image.scale.resolve(values_), {1, 1, 1});
    Vec3 angles = parse_vec3(image.angles.resolve(values_), {0, 0, 0});
    // scene.json keeps angles in radians; z turns clockwise on the canvas.
    math::Mat4 local = math::Mat4::translate(origin[0], origin[1], origin[2]) *
                       math::Mat4::rotate_z(-angles[2]) * math::Mat4::rotate_y(angles[1]) *
                       math::Mat4::rotate_x(angles[0]) *
                       math::Mat4::scale(scale[0], scale[1], scale[2]);
    if (image.parent < 0) return local;
    if (std::ranges::find(visiting, object) != visiting.end()) return local;  // a cycle
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        if (scene.images[i].id == image.parent) {
            visiting.push_back(object);
            math::Mat4 parent = model_matrix(static_cast<int>(i), visiting);
            visiting.pop_back();
            // Hung from a point on the parent puppet's skeleton.
            const ImageObject& owner = scene.images[i];
            if (!image.attachment.empty() && owner.puppet) {
                if (const Mesh* mesh = assets_->mesh(owner.puppet_path)) {
                    float ax = 0, ay = 0;
                    if (mesh->attachment_position(image.attachment, ax, ay)) {
                        return parent * math::Mat4::translate(ax, ay, 0) * local;
                    }
                }
            }
            return parent * local;
        }
    }
    return local;  // the parent is not an image (a particle, say): ignored
}

void SceneRenderer::build(VkExtent2D extent) {
    release();
    const SceneDesc& scene = assets_->scene();
    const SceneGeneral& general = scene.general;
    extent_ = extent;

    // The canvas fills the output: the larger scale wins and the rest is cropped, centred.
    float w = general.ortho[0], h = general.ortho[1];
    canvas_scale_ =
        std::max(static_cast<float>(extent.width) / w, static_cast<float>(extent.height) / h);
    float vw = static_cast<float>(extent.width) / canvas_scale_;
    float vh = static_cast<float>(extent.height) / canvas_scale_;
    float left = (w - vw) * 0.5f, bottom = (h - vh) * 0.5f;
    // Depth covers what scenes use: objects sit at z = -100 and similar, WE's farz is 10000.
    projection_ = math::Mat4::ortho(left, left + vw, bottom, bottom + vh, -10000, 10000);

    auto size_of = [this](const std::string& name) { return assets_->picture_size(name); };
    plan_ = build_plan(scene, values_, size_of);
    for (const auto& w2 : plan_.warnings) log::warn("scene: {}", w2);

    scene_ = make_image(extent, target_format);
    if (plan_.reads_framebuffer) framebuffer_ = make_image(extent, target_format);
    for (const PlanTarget& t : plan_.targets) {
        float sx = t.fixed ? t.size[0] : t.size[0] * canvas_scale_;
        float sy = t.fixed ? t.size[1] : t.size[1] * canvas_scale_;
        VkExtent2D e{std::clamp(static_cast<std::uint32_t>(std::lround(sx)), 1u, max_target),
                     std::clamp(static_cast<std::uint32_t>(std::lround(sy)), 1u, max_target)};
        Image image = make_image(e, target_format);
        image.repeat = t.repeat;
        targets_.push_back(image);
    }

    models_.clear();
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        std::vector<int> visiting;
        models_.push_back(model_matrix(static_cast<int>(i), visiting));
    }

    // Vertices: one quad (triangle strip, position and texcoord) per draw.
    std::vector<float> vertices;
    std::vector<std::uint16_t> indices;
    VkDeviceSize alignment = 256;
    VkDeviceSize uniform_bytes = 0;
    for (const PlanDraw& p : plan_.draws) {
        Draw draw;
        draw.plan = &p;
        draw.target = p.target >= 0 ? &targets_[static_cast<std::size_t>(p.target)] : nullptr;
        draw.first_vertex = static_cast<std::uint32_t>(vertices.size() / 5);
        draw.first_index = static_cast<std::uint32_t>(indices.size());
        float u1 = 1, v1 = 1;
        if (p.crop && p.textures[0].kind == PlanTexture::Kind::file) {
            const SceneTexture& t = assets_->texture(p.textures[0].file);
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
        const ImageObject& object = scene.images[static_cast<std::size_t>(p.object)];
        const Mesh* mesh = p.geometry == Geometry::object && object.puppet
                               ? assets_->mesh(object.puppet_path)
                               : nullptr;
        if (mesh != nullptr && !p.request.name.empty()) {
            // The rest mesh: its positions are already relative to the object's origin. Its
            // uvs index the picture; drawn from a layer they are scaled to the layer's crop.
            float su = 1, sv = 1;
            if (p.textures[0].kind == PlanTexture::Kind::target &&
                !object.material.passes.empty() && !object.material.passes[0].textures.empty() &&
                object.material.passes[0].textures[0]) {
                const SceneTexture& t = assets_->texture(*object.material.passes[0].textures[0]);
                su = static_cast<float>(t.tex_width) / static_cast<float>(t.img_width);
                sv = static_cast<float>(t.tex_height) / static_cast<float>(t.img_height);
            }
            for (std::size_t i = 0; i < mesh->vertices.size(); i += 5) {
                vertices.push_back(mesh->vertices[i]);
                vertices.push_back(mesh->vertices[i + 1]);
                vertices.push_back(0);
                vertices.push_back(mesh->vertices[i + 3] * su);
                vertices.push_back(mesh->vertices[i + 4] * sv);
            }
            indices.insert(indices.end(), mesh->indices.begin(), mesh->indices.end());
            draw.index_count = static_cast<std::uint32_t>(mesh->indices.size());
        } else if (p.geometry == Geometry::object) {
            Vec2 size = object_size(object, general, values_, size_of);
            float x = size[0] * 0.5f, y = size[1] * 0.5f;
            quad(-x, y, x, -y);
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
                const SceneTexture& tex = assets_->texture(t.file);
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
        if (draw.program == nullptr) continue;  // the shader did not compile: skipped
        draw.pipeline = assets_->pipeline(*draw.program, p.blending, target_format);
        if (draw.pipeline == VK_NULL_HANDLE) continue;
        for (std::size_t i = 0; i < max_texture_slots; ++i) {
            const PlanTexture& t = p.textures[i];
            const SceneTexture& white = assets_->texture("");
            draw.views[i] = white.texture ? white.texture->view() : VK_NULL_HANDLE;
            draw.samplers[i] = assets_->sampler(false, false);
            if (t.kind == PlanTexture::Kind::file) {
                const SceneTexture& tex = assets_->texture(t.file);
                if (tex.texture) draw.views[i] = tex.texture->view();
                draw.samplers[i] = assets_->sampler(tex.clamp, tex.nearest);
            } else if (t.kind == PlanTexture::Kind::target) {
                Image& image = targets_[static_cast<std::size_t>(t.target)];
                draw.views[i] = image.view;
                draw.sampled[i] = &image;
                draw.samplers[i] = assets_->sampler(!image.repeat, false);
            } else if (t.kind == PlanTexture::Kind::framebuffer) {
                draw.views[i] = framebuffer_.view;
                draw.sampled[i] = &framebuffer_;
                draw.framebuffer = true;
                draw.samplers[i] = assets_->sampler(true, false);
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
    uniform_slot_ = std::max<VkDeviceSize>(uniform_bytes, alignment);
    uniforms_ =
        gpu::Buffer(gpu_.allocator(), uniform_slot_ * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    stale_ = false;
    log::debug("scene: {} draws, {} targets, {} KB of uniforms per frame", draws_.size(),
               targets_.size(), uniform_slot_ / 1024);
}

void SceneRenderer::fill_block(Draw& draw) {
    const PlanDraw& p = *draw.plan;
    const CompiledShader& shader = draw.program->shader;
    const SceneDesc& scene = assets_->scene();
    std::vector<std::byte>& block = draw.block;

    bool object = p.geometry == Geometry::object;
    math::Mat4 model = object ? models_[static_cast<std::size_t>(p.object)] : math::Mat4{};
    math::Mat4 view_projection = object ? projection_ : math::Mat4{};
    math::Mat4 mvp = view_projection * model;

    VkExtent2D size = draw.target != nullptr ? draw.target->extent : extent_;
    const ImageObject& image = scene.images[static_cast<std::size_t>(p.object)];
    bool material = p.effect < 0;
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

        const std::string& n = u.name;
        auto vec = [&](std::initializer_list<float> f) { write(block, u, std::vector<float>(f)); };
        if (n == "g_ModelViewProjectionMatrix") {
            write_matrix(block, u.offset, mvp);
            if (object && parallax_) draw.dynamics.push_back({Dynamic::What::mvp, u.offset});
        } else if (n == "g_ModelViewProjectionMatrixInverse") {
            write_matrix(block, u.offset, mvp.inverse());
            if (object && parallax_)
                draw.dynamics.push_back({Dynamic::What::mvp_inverse, u.offset});
        } else if (n == "g_ModelMatrix") {
            write_matrix(block, u.offset, model);
            if (object && parallax_) draw.dynamics.push_back({Dynamic::What::model, u.offset});
        } else if (n == "g_ModelMatrixInverse") {
            write_matrix(block, u.offset, model.inverse());
            if (object && parallax_)
                draw.dynamics.push_back({Dynamic::What::model_inverse, u.offset});
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
            } else {
                vec({1, 1, 1, 1});
            }
        } else if (material && n == "g_Color") {
            vec({color[0], color[1], color[2]});
        } else if (material && (n == "g_UserAlpha" || n == "g_Alpha")) {
            vec({alpha});
        } else if (material && n == "g_Brightness") {
            vec({brightness});
        } else if (n.starts_with("g_Texture") && n.size() > 9 && std::isdigit(n[9])) {
            std::size_t slot = static_cast<std::size_t>(n[9] - '0');
            std::string rest = n.substr(10);
            if (slot >= max_texture_slots) continue;
            const PlanTexture& t = p.textures[slot];
            float tw = 1, th = 1, iw = 1, ih = 1, levels = 1;
            if (t.kind == PlanTexture::Kind::file) {
                const SceneTexture& tex = assets_->texture(t.file);
                tw = static_cast<float>(tex.tex_width);
                th = static_cast<float>(tex.tex_height);
                iw = static_cast<float>(tex.img_width);
                ih = static_cast<float>(tex.img_height);
                levels = tex.texture ? static_cast<float>(tex.texture->levels()) : 1;
            } else if (t.kind == PlanTexture::Kind::target) {
                const Image& image2 = targets_[static_cast<std::size_t>(t.target)];
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

math::Mat4 SceneRenderer::parallax_model(int object, const FrameInputs& inputs) const {
    const ImageObject& image = assets_->scene().images[static_cast<std::size_t>(object)];
    const Vec2& canvas = assets_->scene().general.ortho;
    // The layer moves against the pointer by the scene's amount times its depth. The scale
    // (a tenth of the canvas at full amount) is glint's reading of how far Wallpaper Engine
    // moves layers; it is checked by eye, not against a reference.
    float dx = -(inputs.parallax[0] - 0.5f) * parallax_amount_ * image.parallax_depth[0] *
               canvas[0] * 0.2f;
    float dy =
        (inputs.parallax[1] - 0.5f) * parallax_amount_ * image.parallax_depth[1] * canvas[1] * 0.2f;
    return math::Mat4::translate(dx, dy, 0) * models_[static_cast<std::size_t>(object)];
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
                model = parallax_model(draw.plan->object, inputs);
                mvp = projection_ * model;
                moved = true;
            }
            switch (d.what) {
                case Dynamic::What::time: put({static_cast<float>(inputs.time)}); break;
                case Dynamic::What::daytime: put({static_cast<float>(inputs.daytime)}); break;
                case Dynamic::What::frametime: put({static_cast<float>(inputs.delta)}); break;
                case Dynamic::What::pointer:
                case Dynamic::What::pointer_last:
                    put({inputs.pointer[0], inputs.pointer[1]});
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

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
        VkBuffer buffers[] = {vertices_.get(), zeros_.get()};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, indices_.get(), 0, VK_INDEX_TYPE_UINT16);

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
        vkCmdDrawIndexed(cmd, draw.index_count, 1, draw.first_index,
                         static_cast<std::int32_t>(draw.first_vertex), 0);
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
    for (const Image& t : targets_) total += t.memory;
    return total;
}

}  // namespace glint::scene

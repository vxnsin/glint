#include "wallpaper/scene/assets.hpp"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <set>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "gpu/context.hpp"
#include "gpu/pictures.hpp"
#include "gpu/texture.hpp"
#include "gpu/video_pass.hpp"
#include "wallpaper/image/decode.hpp"
#include "wallpaper/scene/tex.hpp"

namespace glint::scene {

namespace {

constexpr std::uint32_t binding_count = 16;  // block, eight slots, other samplers

VkFormat format_of(const Tex& tex) {
    switch (tex.format) {
        case 0: return VK_FORMAT_R8G8B8A8_UNORM;
        case 1:
        case 2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case 4: return VK_FORMAT_BC3_UNORM_BLOCK;
        case 7: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case 8: return VK_FORMAT_R8G8_UNORM;
        case 9: return VK_FORMAT_R8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkShaderModule module(VkDevice device, const std::vector<std::uint32_t>& code) {
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * 4,
        .pCode = code.data(),
    };
    VkShaderModule result = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &result);
    return result;
}

VkPipelineColorBlendAttachmentState blend_state(const std::string& blending) {
    VkPipelineColorBlendAttachmentState s{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    if (blending == "normal" || blending == "disabled" || blending == "opaque") {
        s.blendEnable = VK_FALSE;
    } else if (blending == "additive") {
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (blending == "premultiplied" || blending == "translucentpremultiplied") {
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    return s;
}

}  // namespace

std::string texture_path(const std::string& name) {
    if (name.ends_with(".tex")) return name;
    return "materials/" + name + ".tex";
}

std::shared_ptr<SceneAssets> SceneAssets::load(const std::filesystem::path& item,
                                               const std::filesystem::path& assets,
                                               const std::filesystem::path& shader_cache,
                                               gpu::Context& gpu, gpu::StagingRing& staging,
                                               std::string& error) {
    std::int64_t start = clock::now_ns();
    std::shared_ptr<SceneAssets> self(new SceneAssets(gpu, staging, Vfs(item, assets)));
    if (std::string e = self->vfs_.open_package(); !e.empty()) {
        error = "scene.pkg: " + e;
        return nullptr;
    }
    auto project_file = self->vfs_.read("project.json");
    if (!project_file) {
        error = "no project.json";
        return nullptr;
    }
    if (std::string e = parse_project(project_file->text(), self->project_); !e.empty()) {
        error = e;
        return nullptr;
    }
    if (self->project_.type != "scene") {
        error = "a " + self->project_.type + " item, not a scene";
        return nullptr;
    }
    if (std::string e = load_scene(self->vfs_, self->project_.file, self->scene_); !e.empty()) {
        error = e;
        return nullptr;
    }
    self->warnings_ = self->scene_.warnings;
    if (assets.empty()) self->warnings_.push_back("Wallpaper Engine's assets folder not found");

    // Every texture any material or effect names, read and decoded now.
    Values values = default_values(self->project_);
    auto no_size = [](const std::string&) -> std::optional<Vec2> { return std::nullopt; };
    Plan all = build_plan(self->scene_, values, no_size, true);
    std::set<std::string> names;
    for (const PlanDraw& draw : all.draws) {
        for (const PlanTexture& t : draw.textures) {
            if (t.kind == PlanTexture::Kind::file) names.insert(t.file);
        }
    }
    for (const std::string& name : names) self->read_texture(name);
    for (const TextObject& text : self->scene_.texts) self->read_text(text, values);
    // Now that the textures are read, the plan knows which particle draws need the sprite
    // sheet variant of their shader.
    auto info_of = [self](const std::string& name) -> PlanTextureInfo {
        for (const PendingTexture& p : self->pending_) {
            if (p.name == name) return p.info.plan_info();
        }
        return {};
    };
    all = build_plan(self->scene_, values, no_size, true, info_of);

    // Puppet meshes, drawn in their rest pose until bones arrive in M5.
    for (const ImageObject& image : self->scene_.images) {
        if (image.puppet_path.empty() || self->meshes_.contains(image.puppet_path)) continue;
        auto file = self->vfs_.read(image.puppet_path);
        Mesh mesh;
        std::string e = file ? parse_mdl(file->bytes(), mesh) : "not found";
        if (!e.empty()) {
            self->warnings_.push_back(image.puppet_path + ": " + e + ", drawn flat");
            continue;
        }
        self->meshes_[image.puppet_path] = std::move(mesh);
    }

    // Every shader, also those of hidden objects and effects, so a property change never
    // waits for a compile.
    self->shader_cache_ = shader_cache;
    ShaderCompiler compiler(self->vfs_, shader_cache);
    for (const PlanDraw& draw : all.draws) {
        if (draw.request.name.empty()) continue;
        std::string key = request_key(draw.request);
        if (self->programs_.contains(key)) continue;
        SceneProgram program;
        program.shader = compiler.compile(draw.request);
        if (!program.shader.error.empty()) {
            self->warnings_.push_back("shader " + draw.request.name + ": " +
                                      program.shader.error.substr(0, 300));
        } else if (program.shader.cached) {
            ++self->cached_;
        } else {
            ++self->compiled_;
        }
        for (const auto& u : program.shader.uniforms) {
            if (u.name == "g_Time" || u.name == "g_Daytime") self->animated_ = true;
        }
        // Texture files the shader names as a sampler's default (util/white and the like).
        for (const ShaderSampler& sampler : program.shader.samplers) {
            const std::string& f = sampler.fallback;
            if (f.empty() || f.starts_with("_rt_") || f.starts_with("_alias_")) continue;
            if (!self->textures_.contains(f) && !self->vfs_.exists(texture_path(f))) continue;
            bool pending = false;
            for (const PendingTexture& t : self->pending_) pending = pending || t.name == f;
            if (!pending) self->read_texture(f);
        }
        self->programs_[key] = std::move(program);
    }
    // Timelines move a scene even when no shader reads the time.
    for (const ImageObject& image : self->scene_.images) {
        if (image.origin.animated() || image.scale.animated() || image.angles.animated() ||
            image.alpha.animated()) {
            self->animated_ = true;
        }
        for (const Effect& effect : image.effects) {
            for (const auto& o : effect.overrides) {
                for (const auto& [key, bound] : o.constants) {
                    if (bound.animated()) self->animated_ = true;
                }
            }
        }
    }
    self->load_ms_ = clock::ms_since(start);
    return self;
}

bool SceneAssets::rasterize_text(const TextObject& text, const std::string& value,
                                 PendingTexture& pending, Vec2& center) {
    TextSpec spec;
    spec.text = value;
    spec.font = text.font;
    spec.pointsize = text.pointsize;
    spec.horizontal_align = text.horizontal_align;
    spec.vertical_align = text.vertical_align;
    spec.padding = text.padding;
    spec.spacing = text.spacing;
    spec.max_width = text.max_width;
    spec.max_rows = text.max_rows;
    spec.limit_rows = text.limit_rows;
    spec.limit_width = text.limit_width;
    spec.ellipsis = text.ellipsis;
    spec.block_align = text.block_align;
    spec.background = text.background;
    spec.opaque_background = text.opaque_background;
    // White glyphs: the object's color, alpha and brightness tint them like a picture. Twice
    // the canvas resolution keeps the edges clean where the canvas is shown enlarged.
    spec.pixels_per_unit = 2;
    TextBitmap bitmap = text_.render(spec, vfs_, warnings_);
    if (bitmap.width == 0 || bitmap.height == 0) return false;
    center = bitmap.center;
    pending.name = "_text_" + std::to_string(text.id);
    pending.format = VK_FORMAT_R8G8B8A8_UNORM;
    pending.info.tex_width = pending.info.img_width = bitmap.width;
    pending.info.tex_height = pending.info.img_height = bitmap.height;
    pending.info.clamp = true;
    pending.info.units = 1 / spec.pixels_per_unit;
    pending.generate = true;
    pending.span = staging_.allocate(bitmap.rgba.size());
    if (pending.span.data == nullptr) {
        warnings_.push_back("text " + text.name + ": out of staging memory");
        return false;
    }
    std::memcpy(pending.span.data, bitmap.rgba.data(), bitmap.rgba.size());
    pending.levels.push_back({0, {bitmap.width, bitmap.height}});
    return true;
}

void SceneAssets::read_text(const TextObject& text, const Values& values) {
    std::string str = text_value(text.text.resolve(values));
    PendingTexture pending;
    Vec2 center{0, 0};
    if (!rasterize_text(text, str, pending, center)) return;
    for (ImageObject& image : scene_.images) {
        if (image.id != text.id || image.text < 0) continue;
        image.text_offset = center;
    }
    pending_.push_back(std::move(pending));
}

bool SceneAssets::make_text(const TextObject& text, const std::string& value, SceneTexture& out,
                            Vec2& center) {
    PendingTexture p;
    if (!rasterize_text(text, value, p, center)) return false;
    std::vector<gpu::Texture::Level> levels;
    for (const auto& [offset, size] : p.levels) levels.push_back({offset, size.first, size.second});
    p.info.texture = gpu::Texture::upload(gpu_, staging_, p.span, p.format, levels, p.generate);
    if (!p.info.texture) return false;
    out = p.info;
    return true;
}

void SceneAssets::read_texture(const std::string& name) {
    if (name.starts_with("$mediaThumbnail:")) return read_texture(name.substr(16));
    if (name.starts_with("_rt_") || name.starts_with("_text_")) return;  // not files
    std::string path = texture_path(name);
    auto file = vfs_.read(path);
    if (!file) {
        warnings_.push_back("texture " + name + ": not found");
        return;
    }
    Tex tex;
    if (std::string error = parse_tex(file->bytes(), tex); !error.empty()) {
        warnings_.push_back("texture " + name + ": " + error);
        return;
    }
    if (tex.images.empty() || tex.images[0].empty()) return;
    if (tex.video) {
        // An mp4 inside the .tex: written to the cache once, decoded from there on the
        // decoder's own thread, and drawn into a picture every frame (finish() opens it).
        std::vector<std::byte> payload;
        if (!mip_bytes(tex.images[0][0], payload) || payload.empty()) {
            warnings_.push_back("texture " + name + ": video payload unreadable");
            return;
        }
        std::uint64_t hash = 1469598103934665603ull;
        for (char c : name) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        for (std::size_t i = 0; i < payload.size(); i += 4096) {
            hash = (hash ^ static_cast<unsigned char>(payload[i])) * 1099511628211ull;
        }
        hash = (hash ^ payload.size()) * 1099511628211ull;
        std::filesystem::path dir = paths::cache_dir() / "video";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path cached = dir / (std::format("{:016x}", hash) + ".mp4");
        if (!std::filesystem::exists(cached, ec) ||
            std::filesystem::file_size(cached, ec) != payload.size()) {
            std::ofstream out(cached, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
            if (!out) {
                warnings_.push_back("texture " + name + ": cannot write the video to the cache");
                return;
            }
        }
        // Opened here on the worker, with the first frame decoded, so the main thread only
        // makes the images.
        std::string error;
        auto decoder = video::Decoder::open(cached, "auto", false, error);
        if (!decoder) {
            warnings_.push_back("texture " + name + ": video: " + error);
            return;
        }
        decoder->wait_first();
        pending_videos_.push_back(
            {name, cached, tex.img_width, tex.img_height, std::move(decoder)});
        animated_ = true;
        return;
    }

    PendingTexture pending;
    pending.name = name;
    pending.info.tex_width = tex.tex_width;
    pending.info.tex_height = tex.tex_height;
    pending.info.img_width = tex.img_width;
    pending.info.img_height = tex.img_height;
    pending.info.clamp = (tex.flags & 6u) != 0;
    pending.info.nearest = tex.no_interpolation();
    switch (tex.format) {  // the shaders' FORMAT_ codes
        case 1:
        case 2: pending.info.format = 6; break;
        case 4: pending.info.format = 4; break;
        case 7: pending.info.format = 7; break;
        case 8: pending.info.format = 8; break;
        case 9: pending.info.format = 9; break;
        default: pending.info.format = 0; break;
    }
    if (tex.sprite_sheet() && !tex.frames.empty()) {
        // The frames as a grid, which is how the particle shader steps through them.
        const TexFrame& first = tex.frames[0];
        pending.info.frames = static_cast<std::uint32_t>(tex.frames.size());
        pending.info.frame_width =
            first.width > 0 ? first.width / static_cast<float>(tex.tex_width) : 1.0f;
        pending.info.frame_height =
            first.height > 0 ? first.height / static_cast<float>(tex.tex_height) : 1.0f;
    }
    const auto& mips = tex.images[0];

    if (tex.embedded()) {
        // A PNG or JPEG per mip: decode the largest, make the rest on the GPU.
        std::vector<std::byte> bytes;
        if (!mip_bytes(mips[0], bytes)) {
            warnings_.push_back("texture " + name + ": corrupt");
            return;
        }
        image::Decoded decoded;
        if (std::string error = image::decode_memory(bytes, tex.image_format,
                                                     gpu_.max_image_dimension(), staging_, decoded);
            !error.empty()) {
            warnings_.push_back("texture " + name + ": " + error);
            return;
        }
        pending.format = VK_FORMAT_R8G8B8A8_UNORM;
        pending.info.format = 0;
        pending.span = decoded.pixels;
        pending.levels.push_back({0, {decoded.width, decoded.height}});
        pending.generate = true;
        // The embedded file is the stored size; the picture is the same part of it.
        pending.info.tex_width = decoded.width;
        pending.info.tex_height = decoded.height;
        pending.info.img_width = std::min(tex.img_width, decoded.width);
        pending.info.img_height = std::min(tex.img_height, decoded.height);
        pending_.push_back(std::move(pending));
        return;
    }

    pending.format = format_of(tex);
    if (pending.format == VK_FORMAT_UNDEFINED) {
        warnings_.push_back("texture " + name + ": format " + std::to_string(tex.format));
        return;
    }
    // Stored mips as they are; stop where the chain stops halving.
    std::vector<std::vector<std::byte>> payloads;
    std::uint32_t w = mips[0].width, h = mips[0].height;
    for (const TexMip& mip : mips) {
        if (mip.width != w || mip.height != h) break;
        std::vector<std::byte> bytes;
        if (!mip_bytes(mip, bytes)) break;
        payloads.push_back(std::move(bytes));
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (payloads.empty()) {
        warnings_.push_back("texture " + name + ": corrupt");
        return;
    }
    VkDeviceSize total = 0;
    for (const auto& p : payloads) total += (p.size() + 15) / 16 * 16;
    pending.span = staging_.allocate(total);
    if (pending.span.data == nullptr) {
        warnings_.push_back("texture " + name + ": out of staging memory");
        return;
    }
    VkDeviceSize offset = 0;
    w = mips[0].width;
    h = mips[0].height;
    for (const auto& p : payloads) {
        std::memcpy(pending.span.data + offset, p.data(), p.size());
        pending.levels.push_back({offset, {w, h}});
        offset += (p.size() + 15) / 16 * 16;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    pending_.push_back(std::move(pending));
}

bool SceneAssets::upload_pending() {
    bool ok = true;
    for (PendingTexture& p : pending_) {
        std::vector<gpu::Texture::Level> levels;
        for (const auto& [offset, size] : p.levels)
            levels.push_back({offset, size.first, size.second});
        p.info.texture = gpu::Texture::upload(gpu_, staging_, p.span, p.format, levels, p.generate);
        if (p.info.texture) {
            textures_[p.name] = p.info;
        } else {
            warnings_.push_back("texture " + p.name + ": upload failed");
            ok = false;
        }
    }
    pending_.clear();
    return ok;
}

bool SceneAssets::ensure_texture(const std::string& name) {
    if (textures_.contains(name)) return true;
    if (name.empty() || name.starts_with("_") || name.starts_with("$") || missing_.contains(name)) {
        return false;
    }
    read_texture(name);
    upload_pending();
    if (!textures_.contains(name)) {
        missing_.insert(name);
        return false;
    }
    return true;
}

SceneProgram* SceneAssets::compile_program(const ShaderRequest& request) {
    std::string key = request_key(request);
    if (auto it = programs_.find(key); it != programs_.end()) {
        return it->second.shader.error.empty() ? &it->second : nullptr;
    }
    ShaderCompiler compiler(vfs_, shader_cache_);
    SceneProgram program;
    program.shader = compiler.compile(request);
    if (!program.shader.error.empty()) {
        warnings_.push_back("shader " + request.name + ": " + program.shader.error.substr(0, 300));
        programs_[key] = std::move(program);
        return nullptr;
    }
    for (const ShaderSampler& sampler : program.shader.samplers) {
        const std::string& f = sampler.fallback;
        if (!f.empty() && !f.starts_with("_rt_") && !f.starts_with("_alias_")) ensure_texture(f);
    }
    VkDevice device = gpu_.device();
    program.vertex = module(device, program.shader.vertex);
    program.fragment = module(device, program.shader.fragment);
    SceneProgram& stored = programs_[key] = std::move(program);
    return &stored;
}

bool SceneAssets::finish() {
    VkDevice device = gpu_.device();
    upload_pending();
    for (PendingVideo& p : pending_videos_) start_video(p);
    pending_videos_.clear();

    // A white pixel for every slot nothing is bound to.
    auto span = staging_.allocate(4);
    if (span.data != nullptr) {
        std::memset(span.data, 0xff, 4);
        gpu::Texture::Level level{0, 1, 1};
        white_.texture = gpu::Texture::upload(gpu_, staging_, span, VK_FORMAT_R8G8B8A8_UNORM,
                                              {&level, 1}, false);
    }
    auto none = staging_.allocate(4);
    if (none.data != nullptr) {
        std::memset(none.data, 0, 4);
        gpu::Texture::Level level{0, 1, 1};
        clear_.texture = gpu::Texture::upload(gpu_, staging_, none, VK_FORMAT_R8G8B8A8_UNORM,
                                              {&level, 1}, false);
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (std::uint32_t i = 1; i < binding_count; ++i) {
        bindings.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    }
    VkDescriptorSetLayoutCreateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_);
    VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout_,
    };
    vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_);

    for (int i = 0; i < 4; ++i) {
        bool clamp = (i & 1) != 0;
        bool nearest = (i & 2) != 0;
        VkSamplerAddressMode mode =
            clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
            .minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = mode,
            .addressModeV = mode,
            .addressModeW = mode,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        vkCreateSampler(device, &info, nullptr, &samplers_[i]);
    }

    for (auto& [key, program] : programs_) {
        if (!program.shader.error.empty()) continue;
        program.vertex = module(device, program.shader.vertex);
        program.fragment = module(device, program.shader.fragment);
    }
    return true;
}

SceneAssets::~SceneAssets() {
    release_videos();
    VkDevice device = gpu_.device();
    // Pipelines and modules may still be in use by frames in flight.
    std::vector<VkPipeline> pipelines;
    std::vector<VkShaderModule> modules;
    for (auto& [key, program] : programs_) {
        for (auto& [blend, pipeline] : program.pipelines) pipelines.push_back(pipeline);
        if (program.vertex != VK_NULL_HANDLE) modules.push_back(program.vertex);
        if (program.fragment != VK_NULL_HANDLE) modules.push_back(program.fragment);
    }
    std::vector<VkSampler> samplers(std::begin(samplers_), std::end(samplers_));
    gpu::Context& gpu = gpu_;
    gpu.defer([&gpu, device, pipelines, modules, samplers, layout = layout_, set = set_layout_] {
        for (VkPipeline p : pipelines) vkDestroyPipeline(device, p, nullptr);
        for (VkShaderModule m : modules) vkDestroyShaderModule(device, m, nullptr);
        for (VkSampler s : samplers) {
            if (s != VK_NULL_HANDLE) vkDestroySampler(device, s, nullptr);
        }
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
        if (set != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, set, nullptr);
        (void)gpu;
    });
    for (PendingTexture& p : pending_) staging_.abandon(p.span);
}

const SceneTexture& SceneAssets::texture(const std::string& name) const {
    if (name.starts_with("$mediaThumbnail:")) return texture(name.substr(16));
    if (auto it = textures_.find(name); it != textures_.end()) return it->second;
    if (name.starts_with("_rt_")) return clear_;
    return white_;
}

std::optional<Vec2> SceneAssets::picture_size(const std::string& name) const {
    if (name.starts_with("$mediaThumbnail:")) return picture_size(name.substr(16));
    if (auto it = textures_.find(name); it != textures_.end()) {
        return Vec2{static_cast<float>(it->second.img_width) * it->second.units,
                    static_cast<float>(it->second.img_height) * it->second.units};
    }
    for (const auto& p : pending_) {
        if (p.name == name) {
            return Vec2{static_cast<float>(p.info.img_width) * p.info.units,
                        static_cast<float>(p.info.img_height) * p.info.units};
        }
    }
    return std::nullopt;
}

const Mesh* SceneAssets::mesh(const std::string& path) const {
    auto it = meshes_.find(path);
    return it != meshes_.end() ? &it->second : nullptr;
}

SceneProgram* SceneAssets::program(const ShaderRequest& request) {
    auto it = programs_.find(request_key(request));
    if (it == programs_.end() || !it->second.shader.error.empty()) return nullptr;
    return &it->second;
}

VkSampler SceneAssets::sampler(bool clamp, bool nearest) const {
    return samplers_[(clamp ? 1 : 0) | (nearest ? 2 : 0)];
}

std::uint64_t SceneAssets::memory() const {
    std::uint64_t total = 0;
    for (const auto& [name, t] : textures_) total += t.texture ? t.texture->memory() : 0;
    return total;
}

VkPipeline SceneAssets::pipeline(SceneProgram& program, const std::string& blending,
                                 VkFormat format, VertexLayout layout) {
    std::string key = blending + "|" + std::to_string(static_cast<int>(format)) + "|" +
                      std::to_string(static_cast<int>(layout));
    if (auto it = program.pipelines.find(key); it != program.pipelines.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = program.vertex,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = program.fragment,
         .pName = "main"},
    };
    // One interleaved vertex stream: position (3 floats) and texcoord (2 floats). Inputs the
    // shader declares beyond those read a stream of zeros.
    bool particle = layout == VertexLayout::particle;
    VkVertexInputBindingDescription bindings[] = {
        {0, static_cast<std::uint32_t>((particle ? 17 : 5) * sizeof(float)),
         VK_VERTEX_INPUT_RATE_VERTEX},
        {1, 0, VK_VERTEX_INPUT_RATE_VERTEX},
    };
    std::vector<VkVertexInputAttributeDescription> attributes;
    bool zeros = false;
    auto f = [](std::size_t floats) { return static_cast<std::uint32_t>(floats * sizeof(float)); };
    for (const auto& [name, location] : program.shader.attributes) {
        if (name == "a_Position") {
            attributes.push_back({location, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
        } else if (!particle && name == "a_TexCoord") {
            attributes.push_back({location, 0, VK_FORMAT_R32G32_SFLOAT, f(3)});
        } else if (particle && name == "a_TexCoordVec4") {
            attributes.push_back({location, 0, VK_FORMAT_R32G32B32A32_SFLOAT, f(3)});
        } else if (particle && name == "a_Color") {
            attributes.push_back({location, 0, VK_FORMAT_R32G32B32A32_SFLOAT, f(7)});
        } else if (particle && name == "a_TexCoordC2") {
            attributes.push_back({location, 0, VK_FORMAT_R32G32_SFLOAT, f(11)});
        } else if (particle && name == "a_TexCoordVec4C1") {
            attributes.push_back({location, 0, VK_FORMAT_R32G32B32A32_SFLOAT, f(13)});
        } else {
            attributes.push_back({location, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            zeros = true;
        }
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = zeros ? 2u : 1u,
        .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState attachment = blend_state(blending);
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout_,
    };
    VkPipeline result = VK_NULL_HANDLE;
    if (VkResult r = vkCreateGraphicsPipelines(gpu_.device(), gpu_.pipeline_cache(), 1, &info,
                                               nullptr, &result);
        r != VK_SUCCESS) {
        log::warn("scene pipeline {}: {}", program.shader.hash, gpu::result_name(r));
        result = VK_NULL_HANDLE;
    }
    program.pipelines[key] = result;
    return result;
}

namespace {

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkDependencyInfo dep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

void SceneAssets::start_video(PendingVideo& pending) {
    std::unique_ptr<video::Decoder> decoder = std::move(pending.decoder);
    if (!decoder) return;
    auto v = std::make_unique<VideoTexture>();
    v->name = pending.name;
    v->width = std::max(1u, decoder->info().width);
    v->height = std::max(1u, decoder->info().height);
    v->decoder = std::move(decoder);

    VkDevice device = gpu_.device();
    std::uint32_t sizes[3][2] = {{v->width, v->height},
                                 {(v->width + 1) / 2, (v->height + 1) / 2},
                                 {(v->width + 1) / 2, (v->height + 1) / 2}};
    VkDeviceSize bytes = 0;
    for (int i = 0; i < 3; ++i) {
        VideoTexture::Plane& p = v->planes[static_cast<std::size_t>(i)];
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
        VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VmaAllocationInfo result{};
        if (vmaCreateImage(gpu_.allocator(), &info, &alloc, &p.image, &p.allocation, &result) !=
            VK_SUCCESS) {
            warnings_.push_back("texture " + pending.name + ": video planes: out of memory");
            return;
        }
        v->memory += result.size;
        VkImageViewCreateInfo view{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = p.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8_UNORM,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCreateImageView(device, &view, nullptr, &p.view);
    }
    for (gpu::Buffer& b : v->staging) {
        b = gpu::Buffer(gpu_.allocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }
    VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {v->width, v->height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo result{};
    if (vmaCreateImage(gpu_.allocator(), &info, &alloc, &v->image, &v->allocation, &result) !=
        VK_SUCCESS) {
        warnings_.push_back("texture " + pending.name + ": video picture: out of memory");
        return;
    }
    v->memory += result.size;
    VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = v->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCreateImageView(device, &view, nullptr, &v->view);
    if (!video_pass_) video_pass_ = std::make_unique<gpu::VideoPass>(gpu_);

    SceneTexture t;
    t.external = v->view;
    t.tex_width = t.img_width = v->width;
    t.tex_height = t.img_height = v->height;
    t.clamp = true;
    textures_[pending.name] = t;
    log::info("texture {}: video {}x{} {} fps, {}", pending.name, v->width, v->height,
              v->decoder->info().fps, v->decoder->info().codec);
    videos_.push_back(std::move(v));
}

void SceneAssets::advance_videos(VkCommandBuffer cmd, double time) {
    for (auto& owned : videos_) {
        VideoTexture& v = *owned;
        std::optional<video::Picture> picture = v.decoder->take(time);
        bool first = v.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        if (!picture && !first) continue;
        if (picture &&
            (picture->width != v.planes[0].width || picture->height != v.planes[0].height ||
             picture->prime) &&
            !v.complained) {
            v.complained = true;
            log::warn("texture {}: video frame {}x{}{} does not fit its planes {}x{}", v.name,
                      picture->width, picture->height, picture->prime ? " (dma-buf)" : "",
                      v.planes[0].width, v.planes[0].height);
        }
        if (picture && picture->width == v.planes[0].width &&
            picture->height == v.planes[0].height && !picture->prime) {
            // The planes, through one of two staging buffers (the GPU may still read the
            // other).
            gpu::Buffer& buffer = v.staging[v.slot];
            v.slot ^= 1u;
            const std::vector<std::uint8_t>* data[] = {&picture->y, &picture->u, &picture->v};
            VkDeviceSize offset = 0;
            for (std::size_t i = 0; i < 3 && buffer; ++i) {
                VideoTexture::Plane& p = v.planes[i];
                if (offset + data[i]->size() > buffer.size()) break;
                std::memcpy(buffer.mapped() + offset, data[i]->data(), data[i]->size());
                image_barrier(cmd, p.image,
                              p.sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{
                    .bufferOffset = offset,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .imageExtent = {p.width, p.height, 1},
                };
                vkCmdCopyBufferToImage(cmd, buffer.get(), p.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                image_barrier(cmd, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                p.sampled = true;
                offset += data[i]->size();
            }
            if (buffer) buffer.flush(0, VK_WHOLE_SIZE);
            if (v.shown < 0) {
                log::debug("texture {}: first video frame at {:.3f} s (scene time {:.3f})", v.name,
                           picture->time, time);
            }
            v.shown = picture->time;
        }
        bool planes_ready = v.planes[0].sampled && v.planes[1].sampled && v.planes[2].sampled;
        // The picture: the planes converted, or black until the first frame is there.
        image_barrier(cmd, v.image, v.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = v.view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        color.clearValue.color = {{0, 0, 0, 1}};
        VkExtent2D extent{v.width, v.height};
        VkRenderingInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{0, 0}, extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
        };
        vkCmdBeginRendering(cmd, &info);
        if (planes_ready && video_pass_) {
            VkViewport viewport{0, 0, static_cast<float>(v.width), static_cast<float>(v.height),
                                0, 1};
            VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            gpu::Target target{cmd, extent, VK_FORMAT_R8G8B8A8_UNORM, {}, true};
            gpu::VideoPass::Frame frame{v.planes[0].view,
                                        v.planes[1].view,
                                        v.planes[2].view,
                                        v.width,
                                        v.height,
                                        v.decoder->info().bt709,
                                        v.decoder->info().full_range,
                                        false};
            float white[3] = {1, 1, 1};
            video_pass_->draw(target, frame, gpu::Scaling::stretch, white, 1.0f);
        }
        vkCmdEndRendering(cmd);
        image_barrier(cmd, v.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        v.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

void SceneAssets::release_videos() {
    if (videos_.empty()) return;
    VkDevice device = gpu_.device();
    std::vector<std::pair<VkImage, VmaAllocation>> images;
    std::vector<VkImageView> views;
    std::vector<gpu::Buffer> buffers;
    for (auto& owned : videos_) {
        VideoTexture& v = *owned;
        v.decoder.reset();  // its thread first
        for (VideoTexture::Plane& p : v.planes) {
            if (p.view != VK_NULL_HANDLE) views.push_back(p.view);
            if (p.image != VK_NULL_HANDLE) images.emplace_back(p.image, p.allocation);
        }
        if (v.view != VK_NULL_HANDLE) views.push_back(v.view);
        if (v.image != VK_NULL_HANDLE) images.emplace_back(v.image, v.allocation);
        for (gpu::Buffer& b : v.staging) buffers.push_back(std::move(b));
    }
    videos_.clear();
    VmaAllocator allocator = gpu_.allocator();
    auto held = std::make_shared<std::vector<gpu::Buffer>>(std::move(buffers));
    gpu_.defer([device, allocator, images, views, held]() mutable {
        for (VkImageView v : views) vkDestroyImageView(device, v, nullptr);
        for (auto& [image, allocation] : images) vmaDestroyImage(allocator, image, allocation);
        held.reset();
    });
}

}  // namespace glint::scene

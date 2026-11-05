#include "wallpaper/scene/plan.hpp"

#include <regex>
#include <set>

namespace glint::scene {

namespace {

PlanTexture texture_of(const std::optional<std::string>& name, Plan& plan) {
    PlanTexture t;
    if (!name) return t;
    if (name->starts_with("_rt_FullFrameBuffer") || name->starts_with("_rt_MipMappedFrameBuffer")) {
        t.kind = PlanTexture::Kind::framebuffer;
        plan.reads_framebuffer = true;
        return t;
    }
    if (auto it = plan.composites.find(*name); it != plan.composites.end()) {
        t.kind = PlanTexture::Kind::target;
        t.target = it->second;
        return t;
    }
    t.kind = PlanTexture::Kind::file;
    t.file = *name;
    return t;
}

void bind_mask(PlanDraw& draw) {
    draw.request.bound_textures = 0;
    for (std::size_t i = 0; i < draw.textures.size(); ++i) {
        if (draw.textures[i].kind != PlanTexture::Kind::none) {
            draw.request.bound_textures |= 1u << i;
        }
    }
}

}  // namespace

Vec2 object_size(const ImageObject& image, const SceneGeneral& general, const Values& values,
                 const TextureSize& texture_size) {
    if (image.fullscreen) return general.ortho;
    Vec2 size = parse_vec2(image.size.resolve(values), {0, 0});
    if (size[0] > 0 && size[1] > 0) return size;
    if (image.model_size[0] > 0 && image.model_size[1] > 0) return image.model_size;
    if (!image.material.passes.empty() && !image.material.passes[0].textures.empty() &&
        image.material.passes[0].textures[0]) {
        if (auto s = texture_size(*image.material.passes[0].textures[0])) return *s;
    }
    return general.ortho;
}

std::string request_key(const ShaderRequest& request) {
    std::string key = request.name + "|" + std::to_string(request.bound_textures);
    for (const auto& [name, value] : request.combos) {
        key += "|" + name + "=" + std::to_string(value);
    }
    return key;
}

Plan build_plan(const SceneDesc& scene, const Values& values, const TextureSize& texture_size,
                bool everything) {
    Plan plan;

    // Composites first: an object some texture list names as _rt_imageLayerComposite_<id>_a
    // is drawn with its material into a target of its own, hidden or not (scenes keep
    // hidden helper objects for exactly this), before anything reads it.
    {
        static const std::regex composite(R"(^_rt_imageLayerComposite_(\d+)_[ab]$)");
        std::set<std::string> names;
        auto note = [&](const std::optional<std::string>& t) {
            if (t && std::regex_match(*t, composite)) names.insert(*t);
        };
        for (const ImageObject& image : scene.images) {
            for (const auto& pass : image.material.passes) {
                for (const auto& t : pass.textures) note(t);
            }
            for (const Effect& effect : image.effects) {
                for (const EffectPass& ep : effect.passes) {
                    for (const auto& mp : ep.material.passes) {
                        for (const auto& t : mp.textures) note(t);
                    }
                    for (const auto& b : ep.binds) note(b.name);
                }
                for (const auto& o : effect.overrides) {
                    for (const auto& t : o.textures) note(t);
                }
            }
        }
        for (const std::string& name : names) {
            std::smatch m;
            std::regex_match(name, m, composite);
            int id = std::stoi(m[1].str());
            int object = -1;
            for (std::size_t i = 0; i < scene.images.size(); ++i) {
                if (scene.images[i].id == id) object = static_cast<int>(i);
            }
            if (object < 0 ||
                scene.images[static_cast<std::size_t>(object)].material.passes.empty()) {
                plan.warnings.push_back(name + ": no such image object");
                continue;
            }
            const ImageObject& image = scene.images[static_cast<std::size_t>(object)];
            int target = static_cast<int>(plan.targets.size());
            plan.targets.push_back({object, object_size(image, scene.general, values, texture_size),
                                    false, false, name});
            plan.composites[name] = target;
            bool first = true;
            for (const MaterialPass& pass : image.material.passes) {
                PlanDraw draw;
                draw.object = object;
                draw.request = {pass.shader, pass.combos, 0};
                draw.blending = first ? "normal" : pass.blending;
                draw.geometry = Geometry::clip;
                draw.target = target;
                draw.clear = first;
                for (std::size_t i = 0; i < pass.textures.size() && i < max_texture_slots; ++i) {
                    draw.textures[i] = texture_of(pass.textures[i], plan);
                }
                draw.crop = draw.textures[0].kind == PlanTexture::Kind::file;
                draw.constants = pass.constants;
                draw.user = pass.user_constants;
                bind_mask(draw);
                plan.draws.push_back(std::move(draw));
                first = false;
            }
        }
    }

    for (std::size_t index = 0; index < scene.images.size(); ++index) {
        const ImageObject& image = scene.images[index];
        if (!everything && !image.visible.resolve_bool(values, true)) continue;
        if (image.material.passes.empty()) {
            plan.warnings.push_back(image.name + ": no material");
            continue;
        }
        int object = static_cast<int>(index);
        Geometry geometry = image.fullscreen ? Geometry::clip : Geometry::object;

        std::vector<int> effects;
        for (std::size_t e = 0; e < image.effects.size(); ++e) {
            if (everything || image.effects[e].visible.resolve_bool(values, true)) {
                if (!image.effects[e].passes.empty()) effects.push_back(static_cast<int>(e));
            }
        }

        // The material passes, drawn into the scene; texture 0 replaced by a layer if any.
        auto material_draws = [&](std::optional<int> layer) {
            for (const MaterialPass& pass : image.material.passes) {
                PlanDraw draw;
                draw.object = object;
                draw.request = {pass.shader, pass.combos, 0};
                draw.blending = pass.blending;
                draw.geometry = geometry;
                draw.target = -1;
                for (std::size_t i = 0; i < pass.textures.size() && i < max_texture_slots; ++i) {
                    draw.textures[i] = texture_of(pass.textures[i], plan);
                }
                if (layer) {
                    draw.textures[0] = {PlanTexture::Kind::target, {}, *layer};
                } else {
                    draw.crop = draw.textures[0].kind == PlanTexture::Kind::file;
                }
                draw.constants = pass.constants;
                draw.user = pass.user_constants;
                bind_mask(draw);
                plan.draws.push_back(std::move(draw));
            }
        };

        if (effects.empty()) {
            material_draws(std::nullopt);
            continue;
        }

        // A layer: two ping-pong targets of the object's size.
        Vec2 size = object_size(image, scene.general, values, texture_size);
        int a = static_cast<int>(plan.targets.size());
        plan.targets.push_back({object, size, false, false, "layer a"});
        int b = a + 1;
        plan.targets.push_back({object, size, false, false, "layer b"});

        // The picture into layer a.
        PlanDraw copy;
        copy.object = object;
        copy.target = a;
        copy.clear = true;
        const MaterialPass& first = image.material.passes[0];
        copy.textures[0] =
            texture_of(first.textures.empty() ? std::nullopt : first.textures[0], plan);
        copy.crop = copy.textures[0].kind == PlanTexture::Kind::file;
        plan.draws.push_back(std::move(copy));
        int current = a;

        for (int e : effects) {
            const Effect& effect = image.effects[static_cast<std::size_t>(e)];
            std::map<std::string, int> fbos;
            for (const EffectFbo& fbo : effect.fbos) {
                fbos[fbo.name] = static_cast<int>(plan.targets.size());
                PlanTarget t{object,
                             {size[0] / fbo.scale, size[1] / fbo.scale},
                             false,
                             fbo.repeat,
                             fbo.name};
                if (fbo.width > 0 && fbo.height > 0) {
                    t.size = {static_cast<float>(fbo.width), static_cast<float>(fbo.height)};
                    t.fixed = true;
                }
                plan.targets.push_back(t);
            }
            for (std::size_t p = 0; p < effect.passes.size(); ++p) {
                const EffectPass& pass = effect.passes[p];
                if (!pass.command.empty()) {
                    plan.warnings.push_back(effect.file + ": command " + pass.command +
                                            " not done yet");
                    continue;
                }
                const Effect::Override* o =
                    p < effect.overrides.size() ? &effect.overrides[p] : nullptr;
                int target = -1;
                if (!pass.target.empty()) {
                    auto it = fbos.find(pass.target);
                    if (it == fbos.end()) {
                        plan.warnings.push_back(effect.file + ": unknown target " + pass.target);
                        continue;
                    }
                    target = it->second;
                } else {
                    target = current == a ? b : a;
                }
                for (const MaterialPass& mp : pass.material.passes) {
                    PlanDraw draw;
                    draw.object = object;
                    draw.effect = e;
                    draw.request = {mp.shader, mp.combos, 0};
                    draw.blending = mp.blending;
                    draw.geometry = Geometry::clip;
                    draw.target = target;
                    draw.clear = true;
                    for (std::size_t i = 0; i < mp.textures.size() && i < max_texture_slots; ++i) {
                        draw.textures[i] = texture_of(mp.textures[i], plan);
                    }
                    if (o != nullptr) {
                        for (std::size_t i = 0; i < o->textures.size() && i < max_texture_slots;
                             ++i) {
                            if (o->textures[i]) draw.textures[i] = texture_of(o->textures[i], plan);
                        }
                        for (const auto& [k, v] : o->combos) draw.request.combos[k] = v;
                        draw.bound = o->constants;
                    }
                    // Slot 0 is the input unless the pass binds something else there.
                    if (draw.textures[0].kind == PlanTexture::Kind::none) {
                        draw.textures[0] = {PlanTexture::Kind::target, {}, current};
                    }
                    for (const EffectBind& bind : pass.binds) {
                        if (bind.index < 0 || bind.index >= static_cast<int>(max_texture_slots)) {
                            continue;
                        }
                        auto& slot = draw.textures[static_cast<std::size_t>(bind.index)];
                        if (bind.name == "previous") {
                            slot = {PlanTexture::Kind::target, {}, current};
                        } else if (auto it = fbos.find(bind.name); it != fbos.end()) {
                            slot = {PlanTexture::Kind::target, {}, it->second};
                        } else {
                            slot = texture_of(bind.name, plan);
                        }
                    }
                    draw.constants = mp.constants;
                    draw.user = mp.user_constants;
                    bind_mask(draw);
                    plan.draws.push_back(std::move(draw));
                }
                if (pass.target.empty()) current = target;
            }
        }
        material_draws(current);
    }
    return plan;
}

}  // namespace glint::scene

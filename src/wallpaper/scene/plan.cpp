#include "wallpaper/scene/plan.hpp"

#include <cstdlib>
#include <regex>
#include <set>

#include "wallpaper/scene/particles.hpp"

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

void bind_mask(PlanDraw& draw, const TextureInfoFn& texture_info) {
    draw.request.bound_textures = 0;
    for (std::size_t i = 0; i < draw.textures.size(); ++i) {
        if (draw.textures[i].kind != PlanTexture::Kind::none) {
            draw.request.bound_textures |= 1u << i;
        }
        // The shaders switch on TEXnFORMAT for two-channel and one-channel textures
        // (luminance with alpha, alpha only); RGBA8 is 0, which an unset macro is too.
        if (texture_info && draw.textures[i].kind == PlanTexture::Kind::file) {
            int format = texture_info(draw.textures[i].file).format;
            if (format != 0) draw.request.combos["TEX" + std::to_string(i) + "FORMAT"] = format;
        }
    }
}

}  // namespace

Vec2 object_size(const ImageObject& image, const SceneGeneral& general, const Values& values,
                 const TextureSize& texture_size) {
    if (image.fullscreen) return general.ortho;
    if (image.text >= 0) {
        // The text block, as rasterised (the texture knows its canvas units).
        if (auto s = texture_size("_text_" + std::to_string(image.id))) return *s;
    }
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

bool visible_with_parents(const SceneDesc& scene, int id, const Values& values) {
    // Walks up by id over every kind; a cycle or an unknown parent ends the walk.
    int current = id;
    for (int guard = 0; current >= 0 && guard < 64; ++guard) {
        bool own = true, propagate = true;
        int parent = -1;
        bool found = false;
        for (const ImageObject& o : scene.images) {
            if (o.id != current) continue;
            own = !o.destroyed && o.visible.resolve_bool(values, true);
            propagate = !o.disable_propagation;
            parent = o.parent;
            found = true;
            break;
        }
        if (!found) {
            for (const ParticleObject& o : scene.particles) {
                if (o.id != current) continue;
                own = o.visible.resolve_bool(values, true);
                parent = o.parent;
                found = true;
                break;
            }
        }
        if (!found) {
            for (const GroupObject& o : scene.groups) {
                if (o.id != current) continue;
                own = o.visible.resolve_bool(values, true);
                propagate = !o.disable_propagation;
                parent = o.parent;
                found = true;
                break;
            }
        }
        if (!found) return true;
        if (!own && (current == id || propagate)) return false;
        if (!own && !propagate) return true;
        current = parent;
    }
    return true;
}

Plan build_plan(const SceneDesc& scene, const Values& values, const TextureSize& texture_size,
                bool everything, const TextureInfoFn& texture_info) {
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
        // Only what is drawn reads them: a hidden object, or an effect a property turned
        // off, must not cost a target.
        for (const ImageObject& image : scene.images) {
            if (!everything && !image.visible.resolve_bool(values, true)) continue;
            for (const auto& pass : image.material.passes) {
                for (const auto& t : pass.textures) note(t);
            }
            for (const Effect& effect : image.effects) {
                if (!everything && !effect.visible.resolve_bool(values, true)) continue;
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
            // _a and _b hold the same picture (the object without its effects), so _b is
            // _a's target under another name; the set lists _a first.
            if (name.ends_with("_b")) {
                auto a = plan.composites.find(name.substr(0, name.size() - 2) + "_a");
                if (a != plan.composites.end()) {
                    plan.composites[name] = a->second;
                    continue;
                }
            }
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
                bind_mask(draw, texture_info);
                plan.draws.push_back(std::move(draw));
                first = false;
            }
        }
    }

    // A particle system and each of its children: one draw with the genericparticle shader
    // (or whatever the material names), the quads written every frame by the simulation.
    auto particle_draws = [&](int index) {
        const ParticleObject& object = scene.particles[static_cast<std::size_t>(index)];
        if (!everything && !visible_with_parents(scene, object.id, values)) return;
        // GLINT_SKIP_PARTICLES=1: no particle draws, to see what the rest of a scene does;
        // GLINT_SKIP_PARTICLE=<substring>: leave out the systems whose file matches.
        if (const char* skip = std::getenv("GLINT_SKIP_PARTICLES");
            !everything && skip != nullptr && skip[0] == '1') {
            return;
        }
        if (const char* skip = std::getenv("GLINT_SKIP_PARTICLE");
            !everything && skip != nullptr && skip[0] != 0 &&
            object.file.find(skip) != std::string::npos) {
            return;
        }
        int child = -1;
        auto add = [&](const ParticleDesc& desc, auto& self) -> void {
            int which = child++;
            if (!desc.material.passes.empty()) {
                const MaterialPass& pass = desc.material.passes[0];
                PlanDraw draw;
                draw.particle = index;
                draw.child = which;
                draw.request = {pass.shader.empty() ? "genericparticle" : pass.shader, pass.combos,
                                0};
                draw.request.combos["THICKFORMAT"] = 1;
                if (desc.sprite_trail()) draw.request.combos["TRAILRENDERER"] = 1;
                draw.blending = pass.blending.empty() ? "additive" : pass.blending;
                draw.geometry = Geometry::object;
                draw.target = -1;
                for (std::size_t i = 0; i < pass.textures.size() && i < max_texture_slots; ++i) {
                    draw.textures[i] = texture_of(pass.textures[i], plan);
                }
                if (texture_info && draw.textures[0].kind == PlanTexture::Kind::file &&
                    texture_info(draw.textures[0].file).frames > 1) {
                    draw.request.combos["SPRITESHEET"] = 1;
                }
                draw.constants = pass.constants;
                draw.user = pass.user_constants;
                bind_mask(draw, texture_info);
                plan.draws.push_back(std::move(draw));
            } else {
                plan.warnings.push_back(desc.file + ": no material");
            }
            for (const ParticleDesc::Child& c : desc.children) self(*c.desc, self);
        };
        add(*object.desc, add);
    };

    auto image_draws = [&](std::size_t index) {
        const ImageObject& image = scene.images[index];
        if (!everything && !visible_with_parents(scene, image.id, values)) return;
        // GLINT_SKIP_OBJECT=<substring>: leave out image objects by name, for debugging.
        if (const char* skip = std::getenv("GLINT_SKIP_OBJECT");
            !everything && skip != nullptr && skip[0] != 0 &&
            image.name.find(skip) != std::string::npos) {
            return;
        }
        if (image.material.passes.empty()) {
            plan.warnings.push_back(image.name + ": no material");
            return;
        }
        int object = static_cast<int>(index);
        Geometry geometry = image.fullscreen ? Geometry::clip : Geometry::object;

        std::vector<int> effects;
        // GLINT_SKIP_EFFECT=<substring>: leave matching effects out, to find which one draws
        // something wrong.
        const char* skip = std::getenv("GLINT_SKIP_EFFECT");
        for (std::size_t e = 0; e < image.effects.size(); ++e) {
            if (!everything && skip != nullptr && skip[0] != 0 &&
                image.effects[e].file.find(skip) != std::string::npos) {
                continue;
            }
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
                bind_mask(draw, texture_info);
                plan.draws.push_back(std::move(draw));
            }
        };

        if (effects.empty()) {
            material_draws(std::nullopt);
            return;
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
                    bind_mask(draw, texture_info);
                    plan.draws.push_back(std::move(draw));
                }
                if (pass.target.empty()) current = target;
            }
        }
        material_draws(current);
    };

    if (scene.order.empty()) {
        for (std::size_t index = 0; index < scene.images.size(); ++index) image_draws(index);
    } else {
        for (const ObjectRef& ref : scene.order) {
            if (ref.kind == ObjectRef::Kind::image) {
                image_draws(static_cast<std::size_t>(ref.index));
            } else {
                particle_draws(ref.index);
            }
        }
    }
    return plan;
}

}  // namespace glint::scene

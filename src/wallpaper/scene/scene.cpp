#include "wallpaper/scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "wallpaper/scene/particles.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace glint::scene {

namespace {

using nlohmann::json;

std::optional<json> read_json(const Vfs& vfs, std::string_view path,
                              std::vector<std::string>& warnings) {
    auto file = vfs.read(path);
    if (!file) {
        warnings.push_back(std::string(path) + ": not found");
        return std::nullopt;
    }
    json parsed = json::parse(file->text(), nullptr, false);
    if (parsed.is_discarded()) {
        warnings.push_back(std::string(path) + ": not valid JSON");
        return std::nullopt;
    }
    return parsed;
}

std::string str(const json& j, const char* key, std::string fallback = {}) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return fallback;
}

bool boolean(const json& j, const char* key, bool fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_object() && v.contains("value")) return boolean(v, "value", fallback);
    return fallback;
}

// An integer that may be stored as a number or a string ("7"), never throwing.
int integer(const json& j, const char* key, int fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number()) return static_cast<int>(v.get<double>());
    if (v.is_string()) {
        char* end = nullptr;
        long n = std::strtol(v.get<std::string>().c_str(), &end, 10);
        return end != nullptr && end != v.get<std::string>().c_str() ? static_cast<int>(n)
                                                                     : fallback;
    }
    if (v.is_object() && v.contains("value")) return integer(v, "value", fallback);
    return fallback;
}

float number(const json& j, const char* key, float fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_number()) return v.get<float>();
    if (v.is_string()) return std::strtof(v.get<std::string>().c_str(), nullptr);
    if (v.is_object() && v.contains("value")) return number(v, "value", fallback);
    return fallback;
}

std::vector<float> floats(const json& j) {
    std::vector<float> out;
    if (j.is_number()) {
        out.push_back(j.get<float>());
    } else if (j.is_string()) {
        std::istringstream in(j.get<std::string>());
        float f;
        while (in >> f) out.push_back(f);
    } else if (j.is_object() && j.contains("value")) {
        return floats(j["value"]);
    }
    return out;
}

std::vector<std::optional<std::string>> texture_list(const json& j) {
    std::vector<std::optional<std::string>> out;
    if (!j.is_array()) return out;
    for (const auto& t : j) {
        if (t.is_string()) {
            out.emplace_back(t.get<std::string>());
        } else {
            out.emplace_back(std::nullopt);
        }
    }
    return out;
}

std::map<std::string, int> combo_map(const json& j) {
    std::map<std::string, int> out;
    if (!j.is_object()) return out;
    for (const auto& [name, value] : j.items()) {
        if (value.is_number()) out[name] = value.get<int>();
        if (value.is_boolean()) out[name] = value.get<bool>() ? 1 : 0;
    }
    return out;
}

void parse_effect(const Vfs& vfs, const json& entry, Effect& effect,
                  std::vector<std::string>& warnings) {
    effect.file = str(entry, "file");
    effect.id = integer(entry, "id", 0);
    effect.name = str(entry, "name");
    effect.visible = entry.contains("visible") ? parse_bound(entry["visible"]) : Bound{true};
    if (entry.contains("passes") && entry["passes"].is_array()) {
        for (const auto& pass : entry["passes"]) {
            if (!pass.is_object()) continue;
            Effect::Override o;
            o.combos = combo_map(pass.value("combos", json::object()));
            if (pass.contains("constantshadervalues") && pass["constantshadervalues"].is_object()) {
                for (const auto& [k, v] : pass["constantshadervalues"].items()) {
                    o.constants[k] = parse_bound(v);
                }
            }
            o.textures = texture_list(pass.value("textures", json::array()));
            effect.overrides.push_back(std::move(o));
        }
    }

    auto file = read_json(vfs, effect.file, warnings);
    if (!file) return;
    if (file->contains("passes") && (*file)["passes"].is_array()) {
        for (const auto& pass : (*file)["passes"]) {
            EffectPass p;
            p.material_path = str(pass, "material");
            p.target = str(pass, "target");
            p.command = str(pass, "command");
            p.source = str(pass, "source");
            if (pass.contains("bind") && pass["bind"].is_array()) {
                for (const auto& bind : pass["bind"]) {
                    if (!bind.is_object()) continue;
                    p.binds.push_back({str(bind, "name"), integer(bind, "index", 0)});
                }
            }
            if (!p.material_path.empty()) {
                parse_material(vfs, p.material_path, p.material, warnings);
            }
            effect.passes.push_back(std::move(p));
        }
    }
    if (file->contains("fbos") && (*file)["fbos"].is_array()) {
        for (const auto& fbo : (*file)["fbos"]) {
            EffectFbo f;
            f.name = str(fbo, "name");
            f.scale = number(fbo, "scale", 1);
            if (f.scale <= 0) f.scale = 1;
            f.format = str(fbo, "format", "rgba8888");
            f.width = static_cast<std::uint32_t>(std::max(0.0f, number(fbo, "width", 0)));
            f.height = static_cast<std::uint32_t>(std::max(0.0f, number(fbo, "height", 0)));
            f.repeat = str(fbo, "uvs") == "repeat";
            effect.fbos.push_back(std::move(f));
        }
    }
}

void parse_image(const Vfs& vfs, const json& object, ImageObject& image,
                 std::vector<std::string>& warnings) {
    image.id = integer(object, "id", 0);
    image.name = str(object, "name");
    if (object.contains("parent") && object["parent"].is_number()) {
        image.parent = object["parent"].get<int>();
    }
    image.attachment = str(object, "attachment");
    image.origin = parse_bound(object.value("origin", json("0 0 0")));
    image.scale = parse_bound(object.value("scale", json("1 1 1")));
    image.angles = parse_bound(object.value("angles", json("0 0 0")));
    image.size = parse_bound(object.value("size", json()));
    image.visible = object.contains("visible") ? parse_bound(object["visible"]) : Bound{true};
    image.alpha = object.contains("alpha") ? parse_bound(object["alpha"]) : Bound{1.0};
    image.color = object.contains("color") ? parse_bound(object["color"]) : Bound{"1 1 1"};
    image.brightness =
        object.contains("brightness") ? parse_bound(object["brightness"]) : Bound{1.0};
    image.color_blend_mode = static_cast<int>(number(object, "colorBlendMode", 0));
    image.parallax_depth = parse_vec2(object.value("parallaxDepth", json()), {1, 1});
    image.copy_background = boolean(object, "copybackground", false);
    image.alignment = str(object, "alignment", "center");
    image.disable_propagation = boolean(object, "disablepropagation", false);
    image.perspective = boolean(object, "perspective", false);

    if (object.contains("animationlayers") && object["animationlayers"].is_array()) {
        for (const auto& layer : object["animationlayers"]) {
            if (!layer.is_object()) continue;
            AnimationLayer a;
            a.animation = static_cast<int>(number(layer, "animation", 0));
            a.rate = number(layer, "rate", 1);
            a.blend = number(layer, "blend", 1);
            a.visible = boolean(layer, "visible", true);
            a.additive = boolean(layer, "additive", false);
            image.animation_layers.push_back(a);
        }
    }
    image.model_path = str(object, "image");
    std::optional<json> model;
    if (!image.model_path.empty()) model = read_json(vfs, image.model_path, warnings);
    if (model) {
        image.autosize = boolean(*model, "autosize", false);
        image.fullscreen = boolean(*model, "fullscreen", false);
        image.passthrough = boolean(*model, "passthrough", false);
        image.puppet_path = str(*model, "puppet");
        image.puppet = !image.puppet_path.empty();
        image.model_size = {number(*model, "width", 0), number(*model, "height", 0)};
        std::string material = str(*model, "material");
        if (!material.empty()) parse_material(vfs, material, image.material, warnings);
    }
    // A material instance: the object's own combos and textures over the material's first
    // pass, and system textures ("$mediaThumbnail", the playing media's cover) in a slot.
    if (object.contains("instance") && object["instance"].is_object() &&
        !image.material.passes.empty()) {
        const json& inst = object["instance"];
        MaterialPass& pass = image.material.passes[0];
        for (const auto& [k, v] : combo_map(inst.value("combos", json::object())))
            pass.combos[k] = v;
        if (inst.contains("textures") && inst["textures"].is_array()) {
            auto list = texture_list(inst["textures"]);
            if (pass.textures.size() < list.size()) pass.textures.resize(list.size());
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i]) pass.textures[i] = list[i];
            }
        }
        if (inst.contains("usertextures") && inst["usertextures"].is_array()) {
            std::size_t i = 0;
            for (const json& ut : inst["usertextures"]) {
                if (ut.is_object() && str(ut, "name") == "$mediaThumbnail") {
                    if (pass.textures.size() <= i) pass.textures.resize(i + 1);
                    std::string placeholder = pass.textures[i] ? *pass.textures[i] : "util/white";
                    pass.textures[i] = "$mediaThumbnail:" + placeholder;
                    image.media_thumbnail = true;
                }
                ++i;
            }
        }
    }
    if (object.contains("effects") && object["effects"].is_array()) {
        for (const auto& entry : object["effects"]) {
            if (!entry.is_object()) continue;
            Effect effect;
            parse_effect(vfs, entry, effect, warnings);
            image.effects.push_back(std::move(effect));
        }
    }
}

}  // namespace

Vec3 parse_vec3(const json& j, Vec3 fallback) {
    auto f = floats(j);
    if (f.size() == 1) return {f[0], f[0], f[0]};
    for (std::size_t i = 0; i < 3 && i < f.size(); ++i) fallback[i] = f[i];
    return fallback;
}

Vec2 parse_vec2(const json& j, Vec2 fallback) {
    auto f = floats(j);
    if (f.size() == 1) return {f[0], f[0]};
    for (std::size_t i = 0; i < 2 && i < f.size(); ++i) fallback[i] = f[i];
    return fallback;
}

// Keys for the bound fields of the scene being loaded (one load per thread at a time).
thread_local int bound_keys = 0;

Bound parse_bound(const json& j) {
    Bound b;
    b.key = bound_keys++;
    if (!j.is_object()) {
        b.value = j;
        return b;
    }
    b.value = j.value("value", json());
    if (j.contains("user")) {
        const json& user = j["user"];
        if (user.is_string()) {
            b.user = user.get<std::string>();
        } else if (user.is_object()) {
            b.user = str(user, "name");
            if (user.contains("condition")) {
                const json& c = user["condition"];
                b.condition = c.is_string() ? c.get<std::string>() : c.dump();
            }
        }
    }
    if (j.contains("script") && j["script"].is_string()) {
        b.script = true;
        b.script_source = j["script"].get<std::string>();
        b.script_properties = j.value("scriptproperties", json::object());
    }
    if (j.contains("animation") && j["animation"].is_object()) {
        const json& a = j["animation"];
        auto animation = std::make_shared<Animation>();
        const json options = a.value("options", json::object());
        animation->fps = number(options, "fps", 30);
        if (animation->fps <= 0) animation->fps = 30;
        animation->length = number(options, "length", 0);
        std::string mode = str(options, "mode", "single");
        animation->mode = mode == "loop"     ? Animation::Mode::loop
                          : mode == "mirror" ? Animation::Mode::mirror
                                             : Animation::Mode::single;
        for (int c = 0; c < 4; ++c) {
            std::string key = "c" + std::to_string(c);
            if (!a.contains(key) || !a[key].is_array()) break;
            std::vector<Keyframe> keys;
            for (const json& k : a[key]) {
                Keyframe f;
                f.frame = number(k, "frame", 0);
                f.value = number(k, "value", 0);
                if (k.contains("back") && k["back"].is_object()) {
                    f.back = boolean(k["back"], "enabled", false);
                    f.back_x = number(k["back"], "x", 0);
                    f.back_y = number(k["back"], "y", 0);
                }
                if (k.contains("front") && k["front"].is_object()) {
                    f.front = boolean(k["front"], "enabled", false);
                    f.front_x = number(k["front"], "x", 0);
                    f.front_y = number(k["front"], "y", 0);
                }
                keys.push_back(f);
            }
            std::ranges::sort(keys, {}, &Keyframe::frame);
            animation->channels.push_back(std::move(keys));
        }
        if (animation->length <= 0) {
            for (const auto& ch : animation->channels) {
                if (!ch.empty()) animation->length = std::max(animation->length, ch.back().frame);
            }
        }
        if (!animation->channels.empty()) b.animation = std::move(animation);
    }
    return b;
}

namespace {

float bezier(float p0, float p1, float p2, float p3, float t) {
    float u = 1 - t;
    return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
}

float channel_at(const std::vector<Keyframe>& keys, float frame) {
    if (keys.empty()) return 0;
    if (frame <= keys.front().frame) return keys.front().value;
    if (frame >= keys.back().frame) return keys.back().value;
    std::size_t i = 1;
    while (i < keys.size() && keys[i].frame < frame) ++i;
    const Keyframe& a = keys[i - 1];
    const Keyframe& b = keys[i];
    float span = b.frame - a.frame;
    if (span <= 0) return b.value;
    if (!a.front && !b.back) {
        return a.value + (b.value - a.value) * (frame - a.frame) / span;
    }
    float x0 = a.frame, y0 = a.value;
    float x1 = a.frame + (a.front ? a.front_x : 0) * span, y1 = a.value + (a.front ? a.front_y : 0);
    float x2 = b.frame + (b.back ? b.back_x : 0) * span, y2 = b.value + (b.back ? b.back_y : 0);
    float x3 = b.frame, y3 = b.value;
    // x(t) is monotonic for sane handles: bisect for t.
    float lo = 0, hi = 1, t = 0.5f;
    for (int k = 0; k < 24; ++k) {
        t = (lo + hi) * 0.5f;
        if (bezier(x0, x1, x2, x3, t) < frame) {
            lo = t;
        } else {
            hi = t;
        }
    }
    return bezier(y0, y1, y2, y3, t);
}

}  // namespace

std::vector<float> Animation::at(double seconds) const {
    double frame = seconds * fps;
    if (length > 0) {
        switch (mode) {
            case Mode::single: frame = std::min(frame, static_cast<double>(length)); break;
            case Mode::loop: frame = std::fmod(frame, static_cast<double>(length)); break;
            case Mode::mirror: {
                double cycle = std::fmod(frame, 2.0 * length);
                frame = cycle <= length ? cycle : 2.0 * length - cycle;
                break;
            }
        }
    }
    std::vector<float> out;
    for (const auto& ch : channels) out.push_back(channel_at(ch, static_cast<float>(frame)));
    return out;
}

json Bound::at(const Values& values, double seconds) const {
    const ScriptCell* cell = values.cell(key);
    if (cell != nullptr && cell->set) return cell->value;
    if (!animation) return resolve(values);
    if (cell != nullptr) {
        // A script controls the timeline: paused or stopped it holds a frame, playing it
        // runs from where it was started at its rate.
        if (!cell->playing && cell->held_set) {
            seconds = cell->held / animation->fps;
        } else {
            seconds = (seconds - cell->start) * cell->rate;
            if (cell->held_set) seconds += cell->held / animation->fps;
        }
    }
    std::vector<float> v = animation->at(seconds);
    if (v.size() == 1) return v[0];
    std::string text;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) text += ' ';
        text += std::to_string(v[i]);
    }
    return text;
}

json Bound::resolve(const Values& values) const {
    if (const ScriptCell* cell = values.cell(key); cell != nullptr && cell->set) return cell->value;
    if (user.empty()) return value;
    auto it = values.find(user);
    if (it == values.end()) return value;
    if (!condition.empty()) {
        // A combo-style binding: true when the property has that value.
        const json& v = it->second;
        std::string current = v.is_string() ? v.get<std::string>() : v.dump();
        return current == condition;
    }
    return it->second;
}

bool Bound::resolve_bool(const Values& values, bool fallback) const {
    json v = resolve(values);
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) return v.get<std::string>() == "true" || v.get<std::string>() == "1";
    return fallback;
}

float Bound::resolve_float(const Values& values, float fallback) const {
    json v = resolve(values);
    if (v.is_number()) return v.get<float>();
    if (v.is_boolean()) return v.get<bool>() ? 1.0f : 0.0f;
    if (v.is_string()) {
        auto f = floats(v);
        if (!f.empty()) return f[0];
    }
    return fallback;
}

bool parse_material(const Vfs& vfs, std::string_view path, Material& out,
                    std::vector<std::string>& warnings) {
    out.path = std::string(path);
    auto file = read_json(vfs, path, warnings);
    if (!file) return false;
    if (!file->contains("passes") || !(*file)["passes"].is_array()) {
        warnings.push_back(std::string(path) + ": no passes");
        return false;
    }
    for (const auto& pass : (*file)["passes"]) {
        MaterialPass p;
        p.shader = str(pass, "shader");
        p.blending = str(pass, "blending", "normal");
        p.cullmode = str(pass, "cullmode", "nocull");
        p.depth_test = str(pass, "depthtest") == "enabled";
        p.depth_write = str(pass, "depthwrite") == "enabled";
        p.textures = texture_list(pass.value("textures", json::array()));
        p.combos = combo_map(pass.value("combos", json::object()));
        if (pass.contains("constantshadervalues") && pass["constantshadervalues"].is_object()) {
            for (const auto& [k, v] : pass["constantshadervalues"].items()) p.constants[k] = v;
        }
        if (pass.contains("usershadervalues") && pass["usershadervalues"].is_object()) {
            for (const auto& [k, v] : pass["usershadervalues"].items()) {
                if (v.is_string()) p.user_constants[k] = v.get<std::string>();
            }
        }
        out.passes.push_back(std::move(p));
    }
    return true;
}

void bound_keys_begin(int start) {
    bound_keys = start;
}

int bound_keys_end() {
    return bound_keys;
}

void parse_image_object(const Vfs& vfs, const json& object, ImageObject& image,
                        std::vector<std::string>& warnings) {
    parse_image(vfs, object, image, warnings);
}

std::string load_scene(const Vfs& vfs, std::string_view file, SceneDesc& out) {
    bound_keys = 0;
    auto file_data = vfs.read(file);
    if (!file_data) return std::string(file) + " not found";
    json scene = json::parse(file_data->text(), nullptr, false);
    if (scene.is_discarded() || !scene.is_object()) return std::string(file) + " is not JSON";

    const json general = scene.value("general", json::object());
    if (general.contains("orthogonalprojection") && general["orthogonalprojection"].is_object()) {
        const json& ortho = general["orthogonalprojection"];
        out.general.ortho = {number(ortho, "width", 1920), number(ortho, "height", 1080)};
        if (boolean(ortho, "auto", false))
            out.warnings.push_back("auto projection: using 1920x1080");
    } else {
        out.general.perspective = true;
        out.warnings.push_back("no orthogonalprojection: a 3D scene, drawn flat");
    }
    out.general.clear_color = parse_bound(general.value("clearcolor", json("0 0 0")));
    out.general.clear_enabled = boolean(general, "clearenabled", true);
    out.general.camera_parallax = boolean(general, "cameraparallax", false);
    out.general.parallax_amount = number(general, "cameraparallaxamount", 0.5f);
    out.general.parallax_delay = number(general, "cameraparallaxdelay", 0.1f);
    out.general.parallax_mouse_influence = number(general, "cameraparallaxmouseinfluence", 0.5f);
    out.general.bloom = general.contains("bloom") ? parse_bound(general["bloom"]) : Bound{false};

    if (!scene.contains("objects") || !scene["objects"].is_array()) {
        return std::string(file) + " has no objects";
    }
    int next_id = 1;
    for (const auto& object : scene["objects"]) {
        if (!object.is_object()) continue;
        if (object.contains("image") && object["image"].is_string()) {
            ImageObject image;
            parse_image(vfs, object, image, out.warnings);
            if (image.id == 0) image.id = next_id;  // version 1 scenes have no ids
            out.order.push_back({ObjectRef::Kind::image, static_cast<int>(out.images.size())});
            out.images.push_back(std::move(image));
        } else if (object.contains("particle") && object["particle"].is_string()) {
            ParticleObject particle;
            particle.id = integer(object, "id", next_id);
            particle.name = str(object, "name");
            particle.file = object["particle"].get<std::string>();
            if (object.contains("parent") && object["parent"].is_number()) {
                particle.parent = object["parent"].get<int>();
            }
            particle.origin = parse_bound(object.value("origin", json("0 0 0")));
            particle.scale = parse_bound(object.value("scale", json("1 1 1")));
            particle.angles = parse_bound(object.value("angles", json("0 0 0")));
            particle.visible =
                object.contains("visible") ? parse_bound(object["visible"]) : Bound{true};
            particle.parallax_depth = parse_vec2(object.value("parallaxDepth", json()), {1, 1});
            if (object.contains("instanceoverride") && object["instanceoverride"].is_object()) {
                for (const auto& [k, v] : object["instanceoverride"].items()) {
                    particle.overrides[k] = parse_bound(v);
                }
            }
            particle.desc = parse_particle(vfs, particle.file, out.warnings);
            if (particle.desc) {
                out.order.push_back(
                    {ObjectRef::Kind::particle, static_cast<int>(out.particles.size())});
                out.particles.push_back(std::move(particle));
            }
        } else if (object.contains("text")) {
            TextObject text;
            text.id = integer(object, "id", next_id);
            text.name = str(object, "name");
            text.text = parse_bound(object["text"]);
            text.font = str(object, "font");
            text.pointsize = number(object, "pointsize", 32);
            text.horizontal_align = str(object, "horizontalalign", "center");
            text.vertical_align = str(object, "verticalalign", "center");
            text.anchor = str(object, "anchor");
            // One number pads both axes (SceneScript's padding), "x y" each its own.
            float pad = number(object, "padding", 32);
            text.padding = parse_vec2(object.value("padding", json()), {pad, pad});
            text.spacing = parse_vec2(object.value("spacing", json()), {0, 0});
            text.max_width = number(object, "maxwidth", 500);
            text.max_rows = static_cast<int>(number(object, "maxrows", 1));
            text.limit_rows = boolean(object, "limitrows", false);
            text.limit_width = boolean(object, "limitwidth", false);
            text.ellipsis = boolean(object, "limituseellipsis", false);
            text.block_align = boolean(object, "blockalign", false);
            text.background = parse_vec3(object.value("backgroundcolor", json("0 0 0")), {0, 0, 0});
            text.opaque_background = boolean(object, "opaquebackground", false);
            // Drawn as a picture whose texture is the rasterised text; the shared fields
            // (placement, color, effects) parse as for any image.
            ImageObject image;
            parse_image(vfs, object, image, out.warnings);
            image.id = text.id;
            image.size = Bound{};  // scene.json's size plays no part: the block is the text's
            image.text = static_cast<int>(out.texts.size());
            image.material = Material{};
            MaterialPass pass;
            pass.shader = "genericimage2";
            pass.blending = "translucent";
            pass.textures.push_back("_text_" + std::to_string(text.id));
            image.material.passes.push_back(std::move(pass));
            out.texts.push_back(std::move(text));
            out.order.push_back({ObjectRef::Kind::image, static_cast<int>(out.images.size())});
            out.images.push_back(std::move(image));
        } else if (object.contains("sound") && object["sound"].is_array()) {
            SoundObject sound;
            sound.name = str(object, "name");
            sound.id = integer(object, "id", next_id);
            for (const auto& f : object["sound"]) {
                if (f.is_string()) sound.files.push_back(f.get<std::string>());
            }
            sound.mode = str(object, "playbackmode", "loop");
            sound.volume = object.contains("volume") ? parse_bound(object["volume"]) : Bound{1.0};
            sound.visible =
                object.contains("visible") ? parse_bound(object["visible"]) : Bound{true};
            sound.min_time = number(object, "mintime", 1);
            sound.max_time = std::max(sound.min_time, number(object, "maxtime", 5));
            sound.start_silent = boolean(object, "startsilent", false);
            if (!sound.files.empty()) out.sounds.push_back(std::move(sound));
        } else if (object.contains("light")) {
            ++out.skipped["light"];
        } else if (object.contains("model") && object["model"].is_string()) {
            ++out.skipped["model"];
        } else if (object.contains("camera") || object.contains("fov")) {
            ++out.skipped["camera"];
        } else {
            // A transform node: nothing drawn, but children hang from it.
            GroupObject group;
            group.id = integer(object, "id", next_id);
            group.name = str(object, "name");
            if (object.contains("parent") && object["parent"].is_number()) {
                group.parent = object["parent"].get<int>();
            }
            group.origin = parse_bound(object.value("origin", json("0 0 0")));
            group.scale = parse_bound(object.value("scale", json("1 1 1")));
            group.angles = parse_bound(object.value("angles", json("0 0 0")));
            group.visible =
                object.contains("visible") ? parse_bound(object["visible"]) : Bound{true};
            group.disable_propagation = boolean(object, "disablepropagation", false);
            out.groups.push_back(std::move(group));
        }
        ++next_id;
    }
    out.bound_count = bound_keys;
    return {};
}

}  // namespace glint::scene

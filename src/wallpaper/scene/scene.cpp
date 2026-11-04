#include "wallpaper/scene/scene.hpp"

#include <cstdlib>
#include <sstream>

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
    effect.id = entry.value("id", 0);
    effect.name = str(entry, "name");
    effect.visible = entry.contains("visible") ? parse_bound(entry["visible"]) : Bound{true};
    if (entry.contains("passes") && entry["passes"].is_array()) {
        for (const auto& pass : entry["passes"]) {
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
                    p.binds.push_back({str(bind, "name"), bind.value("index", 0)});
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
            effect.fbos.push_back(std::move(f));
        }
    }
}

void parse_image(const Vfs& vfs, const json& object, ImageObject& image,
                 std::vector<std::string>& warnings) {
    image.id = object.value("id", 0);
    image.name = str(object, "name");
    if (object.contains("parent") && object["parent"].is_number()) {
        image.parent = object["parent"].get<int>();
    }
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
    image.perspective = boolean(object, "perspective", false);

    image.model_path = str(object, "image");
    auto model = read_json(vfs, image.model_path, warnings);
    if (model) {
        image.autosize = boolean(*model, "autosize", false);
        image.fullscreen = boolean(*model, "fullscreen", false);
        image.passthrough = boolean(*model, "passthrough", false);
        image.puppet = model->contains("puppet");
        image.model_size = {number(*model, "width", 0), number(*model, "height", 0)};
        std::string material = str(*model, "material");
        if (!material.empty()) parse_material(vfs, material, image.material, warnings);
    }
    if (object.contains("effects") && object["effects"].is_array()) {
        for (const auto& entry : object["effects"]) {
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

Bound parse_bound(const json& j) {
    Bound b;
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
    b.script = j.contains("script");
    return b;
}

json Bound::resolve(const Values& values) const {
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

std::string load_scene(const Vfs& vfs, std::string_view file, SceneDesc& out) {
    auto file_data = vfs.read(file);
    if (!file_data) return std::string(file) + " not found";
    json scene = json::parse(file_data->text(), nullptr, false);
    if (scene.is_discarded() || !scene.is_object()) return std::string(file) + " is not JSON";

    const json general = scene.value("general", json::object());
    if (general.contains("orthogonalprojection") && general["orthogonalprojection"].is_object()) {
        const json& ortho = general["orthogonalprojection"];
        out.general.ortho = {number(ortho, "width", 1920), number(ortho, "height", 1080)};
        if (ortho.value("auto", false)) out.warnings.push_back("auto projection: using 1920x1080");
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
            out.images.push_back(std::move(image));
        } else if (object.contains("particle")) {
            ++out.skipped["particle"];
        } else if (object.contains("text")) {
            ++out.skipped["text"];
        } else if (object.contains("sound")) {
            ++out.skipped["sound"];
        } else if (object.contains("light")) {
            ++out.skipped["light"];
        } else if (object.contains("model")) {
            ++out.skipped["model"];
        } else {
            ++out.skipped["other"];
        }
        ++next_id;
    }
    return {};
}

}  // namespace glint::scene

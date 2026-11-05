#include "wallpaper/scene/script.hpp"

#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <utility>

#include "core/log.hpp"
#include "core/paths.hpp"
#include "wallpaper/scene/script_prelude.hpp"

namespace glint::scene {

using nlohmann::json;

// A script and the field it is bound to: where it lives (the layer by kind and index, the
// effect on it when it belongs to one), the field's name, and its exported functions.
struct ScriptHost::Script {
    const Bound* bound = nullptr;
    int kind = 0;   // 0 image, 1 particle, 2 group, 3 the scene
    int index = 0;  // into scene.images, .particles, .groups
    int effect = -1;
    int text = -1;  // the text object the field belongs to
    std::string field;
    std::string label;
    bool failed = false;
    bool has_update = false;
    bool cursor_handlers = false;
    bool hovered = false;
    bool pressed = false;
};

namespace {

constexpr int kind_image = 0, kind_particle = 1, kind_group = 2, kind_scene = 3, kind_sound = 4;

// A script property's stored value: like a bound field, it may follow a project property,
// with a condition, and the value it falls back to may itself be bound.
json resolve_script_property(const json& v, const Values& values, int depth = 0) {
    if (!v.is_object() || !v.contains("value") || depth > 4) return v;
    Bound b = parse_bound(v);
    json out = b.resolve(values);
    return resolve_script_property(out, values, depth + 1);
}

std::string vec_text(const Vec2& v) {
    return std::format("{} {}", v[0], v[1]);
}

std::string vec_text(const Vec3& v) {
    return std::format("{} {} {}", v[0], v[1], v[2]);
}

// JSON scalars across the bridge; anything structured goes as JSON text.
JSValue to_js(JSContext* ctx, const json& v) {
    if (v.is_null()) return JS_UNDEFINED;
    if (v.is_boolean()) return JS_NewBool(ctx, v.get<bool>());
    if (v.is_number()) return JS_NewFloat64(ctx, v.get<double>());
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        return JS_NewStringLen(ctx, s.data(), s.size());
    }
    std::string text = v.dump();
    return JS_ParseJSON(ctx, text.data(), text.size(), "<value>");
}

json from_js(JSContext* ctx, JSValueConst v) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return json();
    if (JS_IsBool(v)) return JS_ToBool(ctx, v) != 0;
    if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(ctx, &d, v);
        return d;
    }
    if (JS_IsString(v)) {
        std::size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        json out = s != nullptr ? json(std::string(s, len)) : json("");
        if (s != nullptr) JS_FreeCString(ctx, s);
        return out;
    }
    JSValue text = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    json out;
    if (JS_IsString(text)) {
        std::size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, text);
        if (s != nullptr) {
            out = json::parse(std::string_view(s, len), nullptr, false);
            if (out.is_discarded()) out = json();
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, text);
    return out;
}

std::string string_of(JSContext* ctx, JSValueConst v) {
    std::size_t len = 0;
    const char* s = JS_ToCStringLen(ctx, &len, v);
    std::string out = s != nullptr ? std::string(s, len) : std::string();
    if (s != nullptr) JS_FreeCString(ctx, s);
    return out;
}

int int_of(JSContext* ctx, JSValueConst v) {
    std::int32_t i = 0;
    JS_ToInt32(ctx, &i, v);
    return i;
}

double double_of(JSContext* ctx, JSValueConst v) {
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

JSValue pair(JSContext* ctx, int kind, int index) {
    JSValue values[2] = {JS_NewInt32(ctx, kind), JS_NewInt32(ctx, index)};
    return JS_NewArrayFrom(ctx, 2, values);
}

// The modules scripts import: thin re-exports of the prelude's globals.
const char* module_source(std::string_view name) {
    if (name == "WEMath") {
        return "const m = globalThis.WEMath; export const smoothStep = m.smoothStep; export const "
               "mix = m.mix; export const deg2rad = m.deg2rad; export const rad2deg = m.rad2deg;";
    }
    if (name == "WEVector") {
        return "const m = globalThis.WEVector; export const angleVector2 = m.angleVector2; "
               "export const vectorAngle2 = m.vectorAngle2;";
    }
    if (name == "WEColor") {
        return "const m = globalThis.WEColor; export const rgb2hsv = m.rgb2hsv; export const "
               "hsv2rgb = m.hsv2rgb; export const normalizeColor = m.normalizeColor; export "
               "const expandColor = m.expandColor;";
    }
    return nullptr;
}

}  // namespace

struct ScriptHost::Impl {
    ScriptHost& host;
    const SceneDesc& scene;
    const Project& project;
    std::string label;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSValue glint = JS_UNDEFINED;  // the prelude's __glint
    JSValue run = JS_UNDEFINED, event = JS_UNDEFINED, tick = JS_UNDEFINED;
    JSValue audio_tick = JS_UNDEFINED, user_props = JS_UNDEFINED, load_storage = JS_UNDEFINED;
    JSValue cursor_event = JS_UNDEFINED;
    JSValue engine = JS_UNDEFINED, input = JS_UNDEFINED;
    JSValue audio_left = JS_UNDEFINED, audio_right = JS_UNDEFINED;
    float* audio_left_data = nullptr;
    float* audio_right_data = nullptr;
    double time = 0;
    std::chrono::steady_clock::time_point deadline;
    std::vector<std::string> color_names;
    std::map<std::string, int> warned;  // messages already given, with counts
    std::filesystem::path storage_path;
    json storage;
    bool storage_dirty = false;
    double storage_written = -10;
    // Puppet animation layers under script control: by image index.
    std::vector<std::optional<std::vector<AnimationLayer>>> layers;
    std::vector<std::vector<AnimationLayerState>> layer_states;
    std::vector<int> script_of_cell;  // the script bound to a cell's field, -1 if none

    Impl(ScriptHost& h, const SceneDesc& s, const Project& p, std::string l)
        : host(h), scene(s), project(p), label(std::move(l)) {}

    void warn_once(const std::string& text) {
        int& count = warned[text];
        if (count++ == 0) {
            log::warn("script {}: {}", label, text);
            host.warnings_.push_back(text);
        }
    }

    // The pending exception as text, cleared.
    std::string exception() {
        JSValue e = JS_GetException(ctx);
        std::string text = string_of(ctx, e);
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        if (JS_IsString(stack)) {
            std::string s = string_of(ctx, stack);
            if (!s.empty()) text += "\n" + s;
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, e);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        return text;
    }

    void arm() { deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250); }

    JSValue call(JSValueConst fn, int argc, JSValueConst* argv) {
        arm();
        return JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    }

    // --- the scene for scripts ---

    const Bound* field_bound(int kind, int index, std::string_view name) const {
        auto of = [&](const Bound& origin, const Bound& scale, const Bound& angles,
                      const Bound& visible) -> const Bound* {
            if (name == "origin") return &origin;
            if (name == "scale") return &scale;
            if (name == "angles") return &angles;
            if (name == "visible") return &visible;
            return nullptr;
        };
        if (kind == kind_image) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.images.size()) return nullptr;
            const ImageObject& im = scene.images[static_cast<std::size_t>(index)];
            if (const Bound* b = of(im.origin, im.scale, im.angles, im.visible)) return b;
            if (name == "alpha") return &im.alpha;
            if (name == "color") return &im.color;
            if (name == "brightness") return &im.brightness;
            if (name == "size" && im.text < 0) return &im.size;
            if (name == "text" && im.text >= 0 &&
                static_cast<std::size_t>(im.text) < scene.texts.size()) {
                return &scene.texts[static_cast<std::size_t>(im.text)].text;
            }
            return nullptr;
        }
        if (kind == kind_particle) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.particles.size())
                return nullptr;
            const ParticleObject& p = scene.particles[static_cast<std::size_t>(index)];
            if (const Bound* b = of(p.origin, p.scale, p.angles, p.visible)) return b;
            if (auto it = p.overrides.find(std::string(name)); it != p.overrides.end()) {
                return &it->second;
            }
            return nullptr;
        }
        if (kind == kind_group) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.groups.size()) return nullptr;
            const GroupObject& g = scene.groups[static_cast<std::size_t>(index)];
            return of(g.origin, g.scale, g.angles, g.visible);
        }
        if (kind == kind_sound) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.sounds.size()) return nullptr;
            const SoundObject& so = scene.sounds[static_cast<std::size_t>(index)];
            if (name == "visible") return &so.visible;
            if (name == "volume") return &so.volume;
            return nullptr;
        }
        return nullptr;
    }

    json field_value(int kind, int index, std::string_view name, const Values& values) const {
        if (const Bound* b = field_bound(kind, index, name)) return b->at(values, time);
        if (kind == kind_image) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.images.size()) return json();
            const ImageObject& im = scene.images[static_cast<std::size_t>(index)];
            if (name == "id") return im.id;
            if (name == "name") return im.name;
            if (name == "parallaxDepth") return vec_text(im.parallax_depth);
            if (name == "perspective") return im.perspective;
            if (name == "solid") return false;
            if (name == "effectCount") return im.effects.size();
            if (name == "animationLayerCount") return im.animation_layers.size();
            if (name == "boneCount") return 0;
            if (name == "size") {
                Vec2 size = host.size_of_ ? host.size_of_(index) : im.model_size;
                return vec_text(size);
            }
            if (im.text >= 0 && static_cast<std::size_t>(im.text) < scene.texts.size()) {
                const TextObject& t = scene.texts[static_cast<std::size_t>(im.text)];
                if (name == "pointsize") return t.pointsize;
                if (name == "font") return t.font;
                if (name == "padding") return t.padding[0];
                if (name == "horizontalalign") return t.horizontal_align;
                if (name == "verticalalign") return t.vertical_align;
                if (name == "anchor") return t.anchor;
                if (name == "limitrows") return t.limit_rows;
                if (name == "maxrows") return t.max_rows;
                if (name == "limitwidth") return t.limit_width;
                if (name == "maxwidth") return t.max_width;
                if (name == "opaquebackground") return t.opaque_background;
                if (name == "backgroundcolor") return vec_text(t.background);
            }
            return json();
        }
        if (kind == kind_particle) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.particles.size())
                return json();
            const ParticleObject& p = scene.particles[static_cast<std::size_t>(index)];
            if (name == "id") return p.id;
            if (name == "name") return p.name;
            if (name == "parallaxDepth") return vec_text(p.parallax_depth);
            return json();
        }
        if (kind == kind_group) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.groups.size()) return json();
            const GroupObject& g = scene.groups[static_cast<std::size_t>(index)];
            if (name == "id") return g.id;
            if (name == "name") return g.name;
            return json();
        }
        if (kind == kind_sound) {
            if (index < 0 || static_cast<std::size_t>(index) >= scene.sounds.size()) return json();
            const SoundObject& so = scene.sounds[static_cast<std::size_t>(index)];
            if (name == "id") return so.id;
            if (name == "name") return so.name;
            return json();
        }
        return json();
    }

    // Writes a field's cell; false when the field is not one a script can write.
    bool write(const Bound* bound, json value) {
        if (bound == nullptr || bound->key < 0 ||
            static_cast<std::size_t>(bound->key) >= host.cells_.size()) {
            return false;
        }
        ScriptCell& cell = host.cells_[static_cast<std::size_t>(bound->key)];
        if (cell.set && cell.value == value) return true;
        cell.value = std::move(value);
        cell.set = true;
        return true;
    }

    bool field_set(int kind, int index, std::string_view name, json value) {
        const Bound* b = field_bound(kind, index, name);
        if (b == nullptr) return false;
        bool visible = name == "visible";
        json before = b->key >= 0 && host.cells_[static_cast<std::size_t>(b->key)].set
                          ? host.cells_[static_cast<std::size_t>(b->key)].value
                          : json();
        if (!write(b, value)) return false;
        if (visible && before != value) ++host.structure_;
        if (name == "text" && kind == kind_image) {
            host.changed_texts_.insert(scene.images[static_cast<std::size_t>(index)].id);
        }
        return true;
    }

    // What a script's init() or update() returned, into its field.
    void field_set_result(Script& script, json value) {
        const Bound* b = script.bound;
        if (b == nullptr || b->key < 0) return;
        const ScriptCell& cell = host.cells_[static_cast<std::size_t>(b->key)];
        json before = cell.set ? cell.value : b->value;
        if (!write(b, value)) return;
        if (script.field == "visible" && before != value) ++host.structure_;
        if (script.text >= 0 && before != value) {
            host.changed_texts_.insert(scene.images[static_cast<std::size_t>(script.index)].id);
        }
    }

    const Bound* script_bound(int s) const {
        if (s < 0 || static_cast<std::size_t>(s) >= host.scripts_.size()) return nullptr;
        return host.scripts_[static_cast<std::size_t>(s)].bound;
    }

    int layer_id(int kind, int index) const {
        json id = field_value(kind, index, "id", Values{});
        return id.is_number() ? id.get<int>() : -1;
    }

    int parent_id(int kind, int index) const {
        if (kind == kind_image) return scene.images[static_cast<std::size_t>(index)].parent;
        if (kind == kind_particle) return scene.particles[static_cast<std::size_t>(index)].parent;
        if (kind == kind_group) return scene.groups[static_cast<std::size_t>(index)].parent;
        return -1;
    }

    std::optional<std::pair<int, int>> find_by_id(int id) const {
        for (std::size_t i = 0; i < scene.images.size(); ++i) {
            if (scene.images[i].id == id) return std::pair{kind_image, static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < scene.particles.size(); ++i) {
            if (scene.particles[i].id == id) return std::pair{kind_particle, static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < scene.groups.size(); ++i) {
            if (scene.groups[i].id == id) return std::pair{kind_group, static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < scene.sounds.size(); ++i) {
            if (scene.sounds[i].id == id) return std::pair{kind_sound, static_cast<int>(i)};
        }
        return std::nullopt;
    }

    std::optional<std::pair<int, int>> find_by_name(std::string_view name) const {
        for (std::size_t i = 0; i < scene.images.size(); ++i) {
            if (scene.images[i].name == name) return std::pair{kind_image, static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < scene.particles.size(); ++i) {
            if (scene.particles[i].name == name) {
                return std::pair{kind_particle, static_cast<int>(i)};
            }
        }
        for (std::size_t i = 0; i < scene.groups.size(); ++i) {
            if (scene.groups[i].name == name) return std::pair{kind_group, static_cast<int>(i)};
        }
        for (std::size_t i = 0; i < scene.sounds.size(); ++i) {
            if (scene.sounds[i].name == name) return std::pair{kind_sound, static_cast<int>(i)};
        }
        return std::nullopt;
    }

    // The layers in scene order: the drawn objects, then the groups and sounds.
    std::size_t layer_count() const {
        return scene.order.size() + scene.groups.size() + scene.sounds.size();
    }
    std::optional<std::pair<int, int>> layer_at(std::size_t i) const {
        if (i < scene.order.size()) {
            const ObjectRef& ref = scene.order[i];
            return std::pair{ref.kind == ObjectRef::Kind::image ? kind_image : kind_particle,
                             ref.index};
        }
        i -= scene.order.size();
        if (i < scene.groups.size()) return std::pair{kind_group, static_cast<int>(i)};
        i -= scene.groups.size();
        if (i < scene.sounds.size()) return std::pair{kind_sound, static_cast<int>(i)};
        return std::nullopt;
    }
    int layer_index(int kind, int index) const {
        for (std::size_t i = 0; i < layer_count(); ++i) {
            auto at = layer_at(i);
            if (at && at->first == kind && at->second == index) return static_cast<int>(i);
        }
        return -1;
    }

    // --- timelines (IAnimation) ---

    const Bound* bound_of_key(int key) const {
        if (key < 0) return nullptr;
        for (const Script& s : host.scripts_) {
            if (s.bound != nullptr && s.bound->key == key) return s.bound;
        }
        // Not a scripted field: one of the layer fields.
        for (const ImageObject& im : scene.images) {
            for (const Bound* b : {&im.origin, &im.scale, &im.angles, &im.visible, &im.alpha,
                                   &im.color, &im.brightness, &im.size}) {
                if (b->key == key) return b;
            }
            for (const Effect& e : im.effects) {
                if (e.visible.key == key) return &e.visible;
                for (const auto& o : e.overrides) {
                    for (const auto& [n, b] : o.constants) {
                        if (b.key == key) return &b;
                    }
                }
            }
        }
        for (const ParticleObject& p : scene.particles) {
            for (const Bound* b : {&p.origin, &p.scale, &p.angles, &p.visible}) {
                if (b->key == key) return b;
            }
            for (const auto& [n, b] : p.overrides) {
                if (b.key == key) return &b;
            }
        }
        for (const GroupObject& g : scene.groups) {
            for (const Bound* b : {&g.origin, &g.scale, &g.angles, &g.visible}) {
                if (b->key == key) return b;
            }
        }
        for (const TextObject& t : scene.texts) {
            if (t.text.key == key) return &t.text;
        }
        return nullptr;
    }

    double frame_of(const Bound& bound, const ScriptCell& cell) const {
        const Animation& a = *bound.animation;
        double seconds;
        if (!cell.playing && cell.held_set) {
            seconds = cell.held / a.fps;
        } else {
            seconds = (time - cell.start) * cell.rate;
            if (cell.held_set) seconds += cell.held / a.fps;
        }
        double frame = seconds * a.fps;
        if (a.length <= 0) return 0;
        switch (a.mode) {
            case Animation::Mode::loop: return std::fmod(frame, a.length);
            case Animation::Mode::mirror: {
                double m = std::fmod(frame, 2.0 * a.length);
                return m <= a.length ? m : 2.0 * a.length - m;
            }
            default: return std::min(frame, static_cast<double>(a.length));
        }
    }

    JSValue animation_op(int key, std::string_view op, double arg) {
        const Bound* bound = bound_of_key(key);
        if (bound == nullptr || !bound->animation ||
            static_cast<std::size_t>(key) >= host.cells_.size()) {
            return JS_UNDEFINED;
        }
        const Animation& a = *bound->animation;
        ScriptCell& cell = host.cells_[static_cast<std::size_t>(key)];
        if (op == "fps") return JS_NewFloat64(ctx, a.fps);
        if (op == "frameCount") return JS_NewFloat64(ctx, a.length);
        if (op == "duration") return JS_NewFloat64(ctx, a.fps > 0 ? a.length / a.fps : 0);
        if (op == "name") return JS_NewString(ctx, "");
        if (op == "rate") return JS_NewFloat64(ctx, cell.rate);
        if (op == "isPlaying") return JS_NewBool(ctx, cell.playing);
        if (op == "getFrame") return JS_NewFloat64(ctx, frame_of(*bound, cell));
        if (op == "setRate") {
            cell.held = frame_of(*bound, cell);
            cell.held_set = true;
            cell.start = time;
            cell.rate = arg;
        } else if (op == "play") {
            double frame = frame_of(*bound, cell);
            bool finished = a.mode == Animation::Mode::single && frame >= a.length;
            if (!cell.playing || finished) {
                cell.held = finished ? 0 : frame;
                cell.held_set = true;
                cell.start = time;
                cell.playing = true;
            }
        } else if (op == "pause") {
            cell.held = frame_of(*bound, cell);
            cell.held_set = true;
            cell.playing = false;
        } else if (op == "stop") {
            cell.held = 0;
            cell.held_set = true;
            cell.playing = false;
        } else if (op == "setFrame") {
            cell.held = arg;
            cell.held_set = true;
            cell.start = time;
        }
        return JS_UNDEFINED;
    }

    // --- effects ---

    const Effect* effect_of(int kind, int index, JSValueConst which) const {
        if (kind != kind_image || index < 0 ||
            static_cast<std::size_t>(index) >= scene.images.size()) {
            return nullptr;
        }
        const ImageObject& im = scene.images[static_cast<std::size_t>(index)];
        if (JS_IsNumber(which)) {
            int i = int_of(ctx, which);
            if (i < 0 || static_cast<std::size_t>(i) >= im.effects.size()) return nullptr;
            return &im.effects[static_cast<std::size_t>(i)];
        }
        std::string name = string_of(ctx, which);
        for (const Effect& e : im.effects) {
            if (e.name == name) return &e;
        }
        return nullptr;
    }

    JSValue effect_op(int kind, int index, JSValueConst which, std::string_view op,
                      JSValueConst arg, const Values& values) {
        const Effect* e = effect_of(kind, index, which);
        if (op == "exists") return JS_NewBool(ctx, e != nullptr);
        if (e == nullptr) return JS_UNDEFINED;
        if (op == "visible") return JS_NewBool(ctx, e->visible.resolve_bool(values, true));
        if (op == "name") return JS_NewString(ctx, e->name.c_str());
        if (op == "materialCount") return JS_NewInt32(ctx, static_cast<int>(e->passes.size()));
        if (op == "setVisible") {
            bool v = JS_ToBool(ctx, arg) != 0;
            bool before = e->visible.resolve_bool(values, true);
            if (write(&e->visible, v) && before != v) ++host.structure_;
            return JS_UNDEFINED;
        }
        if (op.starts_with("setProperty:")) {
            std::string name(op.substr(12));
            bool found = false;
            for (const auto& o : e->overrides) {
                if (auto it = o.constants.find(name); it != o.constants.end()) {
                    write(&it->second, from_js(ctx, arg));
                    found = true;
                }
            }
            if (!found) warn_once("setMaterialProperty(" + name + "): no such override");
            return JS_UNDEFINED;
        }
        if (op.starts_with("animationKey:")) return JS_NewInt32(ctx, -1);
        return JS_UNDEFINED;
    }

    // --- puppet animation layers ---

    std::vector<AnimationLayer>* layers_of(int kind, int index) {
        if (kind != kind_image || index < 0 ||
            static_cast<std::size_t>(index) >= scene.images.size()) {
            return nullptr;
        }
        auto i = static_cast<std::size_t>(index);
        if (layers.size() < scene.images.size()) {
            layers.resize(scene.images.size());
            layer_states.resize(scene.images.size());
        }
        if (!layers[i]) {
            layers[i] = scene.images[i].animation_layers;
            layer_states[i].assign(layers[i]->size(), {});
        }
        return &*layers[i];
    }

    JSValue animation_layer_op(int kind, int index, JSValueConst which, std::string_view op,
                               JSValueConst arg) {
        std::vector<AnimationLayer>* list = layers_of(kind, index);
        int i = -1;
        if (list != nullptr) {
            if (JS_IsNumber(which)) {
                i = int_of(ctx, which);
            } else {
                // By the animation's name: the layer whose animation id matches a name is
                // not known here; the first layer stands in.
                i = list->empty() ? -1 : 0;
            }
            if (i < 0 || static_cast<std::size_t>(i) >= list->size()) i = -1;
        }
        if (op == "exists") return JS_NewBool(ctx, i >= 0);
        if (i < 0) return JS_UNDEFINED;
        AnimationLayer& layer = (*list)[static_cast<std::size_t>(i)];
        AnimationLayerState& state =
            layer_states[static_cast<std::size_t>(index)][static_cast<std::size_t>(i)];
        if (op == "fps") return JS_NewFloat64(ctx, 30);
        if (op == "frameCount") return JS_NewFloat64(ctx, 0);
        if (op == "duration") return JS_NewFloat64(ctx, 0);
        if (op == "name") return JS_NewString(ctx, "");
        if (op == "rate") return JS_NewFloat64(ctx, layer.rate);
        if (op == "blend") return JS_NewFloat64(ctx, layer.blend);
        if (op == "visible") return JS_NewBool(ctx, layer.visible);
        if (op == "playing") return JS_NewBool(ctx, state.playing);
        if (op == "frame") {
            double seconds = state.playing ? (time - state.start) + state.held : state.held;
            return JS_NewFloat64(ctx, seconds * 30);
        }
        if (op == "setRate") {
            layer.rate = static_cast<float>(double_of(ctx, arg));
        } else if (op == "setBlend") {
            layer.blend = static_cast<float>(double_of(ctx, arg));
        } else if (op == "setVisible") {
            layer.visible = JS_ToBool(ctx, arg) != 0;
        } else if (op == "setPlaying") {
            bool play = JS_ToBool(ctx, arg) != 0;
            if (play && !state.playing) {
                state.start = time;
                state.playing = true;
            } else if (!play && state.playing) {
                state.held += time - state.start;
                state.held_set = true;
                state.playing = false;
            }
        } else if (op == "setStop") {
            state.playing = false;
            state.held = 0;
            state.held_set = true;
        } else if (op == "setFrame") {
            state.held = double_of(ctx, arg) / 30.0;
            state.held_set = true;
            state.start = time;
        }
        return JS_UNDEFINED;
    }

    // --- storage ---

    void load_storage_file() {
        if (storage_path.empty()) {
            storage = json::object();
            return;
        }
        std::ifstream in(storage_path);
        if (!in) {
            storage = json::object();
            return;
        }
        storage = json::parse(in, nullptr, false);
        if (!storage.is_object()) storage = json::object();
    }

    void flush_storage() {
        if (!storage_dirty || storage_path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(storage_path.parent_path(), ec);
        std::ofstream out(storage_path);
        if (out) out << storage.dump();
        storage_dirty = false;
        storage_written = time;
    }
};

namespace {

ScriptHost::Impl& impl_of(JSContext* ctx) {
    return *static_cast<ScriptHost::Impl*>(JS_GetContextOpaque(ctx));
}

// The Values the frame is running with: the host's cells behind the renderer's values.
thread_local const Values* current_values = nullptr;

const Values& values_now() {
    static const Values none;
    return current_values != nullptr ? *current_values : none;
}

JSValue native_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    ScriptHost::Impl& impl = impl_of(ctx);
    if (argc < 2) return JS_UNDEFINED;
    int level = int_of(ctx, argv[0]);
    std::string text = string_of(ctx, argv[1]);
    if (level == 0) {
        log::info("script {}: {}", impl.label, text);
    } else {
        log::warn("script {}: {}", impl.label, text);
    }
    return JS_UNDEFINED;
}

JSValue native_warn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 1) impl_of(ctx).warn_once(string_of(ctx, argv[0]));
    return JS_UNDEFINED;
}

JSValue native_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    ScriptHost::Impl& impl = impl_of(ctx);
    std::string name = string_of(ctx, argv[2]);
    return to_js(ctx,
                 impl.field_value(int_of(ctx, argv[0]), int_of(ctx, argv[1]), name, values_now()));
}

JSValue native_set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_FALSE;
    ScriptHost::Impl& impl = impl_of(ctx);
    std::string name = string_of(ctx, argv[2]);
    return JS_NewBool(ctx, impl.field_set(int_of(ctx, argv[0]), int_of(ctx, argv[1]), name,
                                          from_js(ctx, argv[3])));
}

JSValue native_parent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    ScriptHost::Impl& impl = impl_of(ctx);
    int kind = int_of(ctx, argv[0]), index = int_of(ctx, argv[1]);
    if (impl.field_value(kind, index, "id", values_now()).is_null()) return JS_UNDEFINED;
    auto found = impl.find_by_id(impl.parent_id(kind, index));
    if (!found) return JS_UNDEFINED;
    return pair(ctx, found->first, found->second);
}

JSValue native_children(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValue out = JS_NewArray(ctx);
    if (argc < 2) return out;
    ScriptHost::Impl& impl = impl_of(ctx);
    int id = impl.layer_id(int_of(ctx, argv[0]), int_of(ctx, argv[1]));
    if (id < 0) return out;
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < impl.layer_count(); ++i) {
        auto at = impl.layer_at(i);
        if (!at || impl.parent_id(at->first, at->second) != id) continue;
        JS_SetPropertyUint32(ctx, out, n++, pair(ctx, at->first, at->second));
    }
    return out;
}

JSValue native_find_layer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    ScriptHost::Impl& impl = impl_of(ctx);
    std::optional<std::pair<int, int>> found;
    if (JS_IsNumber(argv[0])) {
        int n = int_of(ctx, argv[0]);
        found = impl.find_by_id(n);
        if (!found && n >= 0) found = impl.layer_at(static_cast<std::size_t>(n));
    } else if (JS_IsObject(argv[0])) {
        JSValue k = JS_GetPropertyStr(ctx, argv[0], "__kind");
        JSValue i = JS_GetPropertyStr(ctx, argv[0], "__index");
        if (JS_IsNumber(k) && JS_IsNumber(i)) found = std::pair{int_of(ctx, k), int_of(ctx, i)};
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, i);
    } else {
        std::string name = string_of(ctx, argv[0]);
        found = impl.find_by_name(name);
        if (!found) {
            char* end = nullptr;
            long id = std::strtol(name.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && !name.empty()) {
                found = impl.find_by_id(static_cast<int>(id));
            }
        }
    }
    if (!found) return JS_UNDEFINED;
    return pair(ctx, found->first, found->second);
}

JSValue native_layer_count(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, static_cast<int>(impl_of(ctx).layer_count()));
}

JSValue native_layer_at(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    int i = int_of(ctx, argv[0]);
    if (i < 0) return JS_UNDEFINED;
    auto at = impl_of(ctx).layer_at(static_cast<std::size_t>(i));
    if (!at) return JS_UNDEFINED;
    return pair(ctx, at->first, at->second);
}

JSValue native_layer_index(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, impl_of(ctx).layer_index(int_of(ctx, argv[0]), int_of(ctx, argv[1])));
}

// The key of a layer field's timeline: by field name, or the first field that has one.
JSValue native_animation_key(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_NewInt32(ctx, -1);
    ScriptHost::Impl& impl = impl_of(ctx);
    int kind = int_of(ctx, argv[0]), index = int_of(ctx, argv[1]);
    std::string name = string_of(ctx, argv[2]);
    const char* fields[] = {"origin",  "scale",      "angles", "alpha", "color",
                            "visible", "brightness", "size",   "text"};
    for (const char* f : fields) {
        if (!name.empty() && name != f) continue;
        const Bound* b = impl.field_bound(kind, index, f);
        if (b != nullptr && b->animation) return JS_NewInt32(ctx, b->key);
    }
    return JS_NewInt32(ctx, -1);
}

JSValue native_script_animation_key(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, -1);
    ScriptHost::Impl& impl = impl_of(ctx);
    const Bound* b = impl.script_bound(int_of(ctx, argv[0]));
    return JS_NewInt32(ctx, b != nullptr && b->animation ? b->key : -1);
}

JSValue native_animation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_UNDEFINED;
    std::string op = string_of(ctx, argv[1]);
    double arg = argc >= 3 ? double_of(ctx, argv[2]) : 0;
    return impl_of(ctx).animation_op(int_of(ctx, argv[0]), op, arg);
}

JSValue native_animation_layer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_UNDEFINED;
    std::string op = string_of(ctx, argv[3]);
    return impl_of(ctx).animation_layer_op(int_of(ctx, argv[0]), int_of(ctx, argv[1]), argv[2], op,
                                           argc >= 5 ? argv[4] : JS_UNDEFINED);
}

JSValue native_effect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_UNDEFINED;
    std::string op = string_of(ctx, argv[3]);
    return impl_of(ctx).effect_op(int_of(ctx, argv[0]), int_of(ctx, argv[1]), argv[2], op,
                                  argc >= 5 ? argv[4] : JS_UNDEFINED, values_now());
}

JSValue native_transform(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    // The layer's own transform: its origin, scale and angles (no parents yet).
    JSValue out = JS_NewArray(ctx);
    if (argc < 2) return out;
    ScriptHost::Impl& impl = impl_of(ctx);
    int kind = int_of(ctx, argv[0]), index = int_of(ctx, argv[1]);
    const Values& values = values_now();
    Vec3 origin = parse_vec3(impl.field_value(kind, index, "origin", values), {0, 0, 0});
    Vec3 scale = parse_vec3(impl.field_value(kind, index, "scale", values), {1, 1, 1});
    Vec3 angles = parse_vec3(impl.field_value(kind, index, "angles", values), {0, 0, 0});
    math::Mat4 m = math::Mat4::translate(origin[0], origin[1], origin[2]) *
                   math::Mat4::rotate_z(-angles[2]) *
                   math::Mat4::scale(scale[0], scale[1], scale[2]);
    for (std::uint32_t i = 0; i < 16; ++i)
        JS_SetPropertyUint32(ctx, out, i, JS_NewFloat64(ctx, m.m[i]));
    return out;
}

JSValue native_script_properties(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    ScriptHost::Impl& impl = impl_of(ctx);
    const Bound* b = impl.script_bound(int_of(ctx, argv[0]));
    if (b == nullptr || !b->script_properties.is_object()) return JS_UNDEFINED;
    json resolved = json::object();
    for (const auto& [name, value] : b->script_properties.items()) {
        resolved[name] = resolve_script_property(value, values_now());
    }
    return to_js(ctx, resolved);
}

JSValue native_store(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_UNDEFINED;
    ScriptHost::Impl& impl = impl_of(ctx);
    std::string location = string_of(ctx, argv[0]);
    std::string key = string_of(ctx, argv[1]);
    json& bucket = impl.storage[location];
    if (!bucket.is_object()) bucket = json::object();
    if (JS_IsUndefined(argv[2])) {
        bucket.erase(key);
    } else {
        json v = json::parse(string_of(ctx, argv[2]), nullptr, false);
        if (!v.is_discarded()) bucket[key] = v;
    }
    impl.storage_dirty = true;
    return JS_UNDEFINED;
}

int interrupt(JSRuntime*, void* opaque) {
    auto* impl = static_cast<ScriptHost::Impl*>(opaque);
    return std::chrono::steady_clock::now() > impl->deadline ? 1 : 0;
}

JSModuleDef* load_module(JSContext* ctx, const char* name, void*) {
    const char* source = module_source(name);
    if (source == nullptr) {
        JS_ThrowReferenceError(ctx, "no module '%s'", name);
        return nullptr;
    }
    JSValue v = JS_Eval(ctx, source, std::strlen(source), name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(v)) return nullptr;
    auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(v));
    JS_FreeValue(ctx, v);
    return def;
}

}  // namespace

ScriptHost::ScriptHost(const SceneDesc& scene, const Project& project, std::string label,
                       std::string storage_id)
    : impl_(std::make_unique<Impl>(*this, scene, project, std::move(label))) {
    cells_.assign(static_cast<std::size_t>(std::max(scene.bound_count, 0)), {});

    // The scripts: every bound field with one, on layers, effects and texts.
    auto add = [&](const Bound& b, int kind, int index, int effect, int text, std::string field,
                   std::string owner) {
        if (!b.script || b.script_source.empty()) return;
        Script s;
        s.bound = &b;
        s.kind = kind;
        s.index = index;
        s.effect = effect;
        s.text = text;
        s.field = std::move(field);
        s.label = owner + "." + s.field;
        scripts_.push_back(std::move(s));
    };
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        const ImageObject& im = scene.images[i];
        std::string owner = im.name.empty() ? std::to_string(im.id) : im.name;
        auto idx = static_cast<int>(i);
        add(im.origin, kind_image, idx, -1, -1, "origin", owner);
        add(im.scale, kind_image, idx, -1, -1, "scale", owner);
        add(im.angles, kind_image, idx, -1, -1, "angles", owner);
        add(im.size, kind_image, idx, -1, -1, "size", owner);
        add(im.visible, kind_image, idx, -1, -1, "visible", owner);
        add(im.alpha, kind_image, idx, -1, -1, "alpha", owner);
        add(im.color, kind_image, idx, -1, -1, "color", owner);
        add(im.brightness, kind_image, idx, -1, -1, "brightness", owner);
        if (im.text >= 0 && static_cast<std::size_t>(im.text) < scene.texts.size()) {
            add(scene.texts[static_cast<std::size_t>(im.text)].text, kind_image, idx, -1, im.text,
                "text", owner);
        }
        for (std::size_t e = 0; e < im.effects.size(); ++e) {
            const Effect& effect = im.effects[e];
            std::string eo = owner + "/" + (effect.name.empty() ? effect.file : effect.name);
            add(effect.visible, kind_image, idx, static_cast<int>(e), -1, "visible", eo);
            for (const auto& o : effect.overrides) {
                for (const auto& [name, b] : o.constants) {
                    add(b, kind_image, idx, static_cast<int>(e), -1, name, eo);
                }
            }
        }
    }
    for (std::size_t i = 0; i < scene.particles.size(); ++i) {
        const ParticleObject& p = scene.particles[i];
        std::string owner = p.name.empty() ? std::to_string(p.id) : p.name;
        auto idx = static_cast<int>(i);
        add(p.origin, kind_particle, idx, -1, -1, "origin", owner);
        add(p.scale, kind_particle, idx, -1, -1, "scale", owner);
        add(p.angles, kind_particle, idx, -1, -1, "angles", owner);
        add(p.visible, kind_particle, idx, -1, -1, "visible", owner);
        for (const auto& [name, b] : p.overrides) add(b, kind_particle, idx, -1, -1, name, owner);
    }
    for (std::size_t i = 0; i < scene.groups.size(); ++i) {
        const GroupObject& g = scene.groups[i];
        std::string owner = g.name.empty() ? std::to_string(g.id) : g.name;
        auto idx = static_cast<int>(i);
        add(g.origin, kind_group, idx, -1, -1, "origin", owner);
        add(g.scale, kind_group, idx, -1, -1, "scale", owner);
        add(g.angles, kind_group, idx, -1, -1, "angles", owner);
        add(g.visible, kind_group, idx, -1, -1, "visible", owner);
    }
    add(scene.general.clear_color, kind_scene, 0, -1, -1, "clearcolor", "scene");
    add(scene.general.bloom, kind_scene, 0, -1, -1, "bloom", "scene");
    if (scripts_.empty()) return;

    for (const ProjectProperty& p : project.properties) {
        if (p.type == "color") impl_->color_names.push_back(p.name);
    }
    if (!storage_id.empty()) {
        impl_->storage_path = paths::state_dir() / "scripts" / (storage_id + ".json");
    }
    impl_->load_storage_file();

    Impl& impl = *impl_;
    impl.rt = JS_NewRuntime();
    JS_SetMemoryLimit(impl.rt, 64u << 20);
    JS_SetMaxStackSize(impl.rt, 1u << 20);
    JS_SetInterruptHandler(impl.rt, interrupt, &impl);
    JS_SetModuleLoaderFunc(impl.rt, nullptr, load_module, &impl);
    impl.ctx = JS_NewContext(impl.rt);
    JS_SetContextOpaque(impl.ctx, &impl);
    JSContext* ctx = impl.ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue native = JS_NewObject(ctx);
    struct Fn {
        const char* name;
        JSCFunction* fn;
        int length;
    };
    const Fn fns[] = {
        {"log", native_log, 2},
        {"warn", native_warn, 1},
        {"get", native_get, 3},
        {"set", native_set, 4},
        {"parent", native_parent, 2},
        {"children", native_children, 2},
        {"findLayer", native_find_layer, 1},
        {"layerCount", native_layer_count, 0},
        {"layerAt", native_layer_at, 1},
        {"layerIndex", native_layer_index, 2},
        {"animationKey", native_animation_key, 3},
        {"scriptAnimationKey", native_script_animation_key, 1},
        {"animation", native_animation, 3},
        {"animationLayer", native_animation_layer, 5},
        {"effect", native_effect, 5},
        {"transform", native_transform, 2},
        {"scriptProperties", native_script_properties, 1},
        {"store", native_store, 3},
    };
    for (const Fn& f : fns) {
        JS_SetPropertyStr(ctx, native, f.name, JS_NewCFunction(ctx, f.fn, f.name, f.length));
    }
    JS_SetPropertyStr(ctx, global, "__native", native);

    impl.arm();
    JSValue r =
        JS_Eval(ctx, script_prelude, std::strlen(script_prelude), "<glint>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        log::warn("script {}: prelude: {}", impl.label, impl.exception());
        JS_FreeValue(ctx, global);
        scripts_.clear();
        return;
    }
    JS_FreeValue(ctx, r);
    impl.glint = JS_GetPropertyStr(ctx, global, "__glint");
    impl.run = JS_GetPropertyStr(ctx, impl.glint, "run");
    impl.event = JS_GetPropertyStr(ctx, impl.glint, "event");
    impl.tick = JS_GetPropertyStr(ctx, impl.glint, "tick");
    impl.audio_tick = JS_GetPropertyStr(ctx, impl.glint, "audioTick");
    impl.user_props = JS_GetPropertyStr(ctx, impl.glint, "userProperties");
    impl.load_storage = JS_GetPropertyStr(ctx, impl.glint, "loadStorage");
    impl.cursor_event = JS_GetPropertyStr(ctx, impl.glint, "cursorEvent");
    impl.engine = JS_GetPropertyStr(ctx, global, "engine");
    impl.input = JS_GetPropertyStr(ctx, global, "input");
    JSValue register_fn = JS_GetPropertyStr(ctx, impl.glint, "register");
    JSValue namespace_fn = JS_GetPropertyStr(ctx, impl.glint, "setNamespace");
    JS_FreeValue(ctx, global);

    // The spectrum the audio buffers read from: two arrays of 64 the host writes into.
    for (int side = 0; side < 2; ++side) {
        JSValue n = JS_NewInt32(ctx, 64);
        JSValue arr = JS_NewTypedArray(ctx, 1, &n, JS_TYPED_ARRAY_FLOAT32);
        JSValue buffer = JS_GetTypedArrayBuffer(ctx, arr, nullptr, nullptr, nullptr);
        std::size_t size = 0;
        auto* data = reinterpret_cast<float*>(JS_GetArrayBuffer(ctx, &size, buffer));
        JS_FreeValue(ctx, buffer);
        (side == 0 ? impl.audio_left : impl.audio_right) = arr;
        (side == 0 ? impl.audio_left_data : impl.audio_right_data) = data;
    }
    {
        JSValue stored = to_js(ctx, impl.storage);
        JSValue arg = JS_IsUndefined(stored) ? JS_NewObject(ctx) : stored;
        JS_FreeValue(ctx, impl.call(impl.load_storage, 1, &arg));
        JS_FreeValue(ctx, arg);
    }

    // Each script is a module with its own thisLayer, thisObject and script properties.
    for (std::size_t s = 0; s < scripts_.size(); ++s) {
        Script& script = scripts_[s];
        std::string source = std::format(
                                 "const thisLayer = __glint.layer({}, {}); const thisObject = "
                                 "__glint.object({}); const createScriptProperties = "
                                 "__glint.properties({});\n",
                                 script.kind == kind_scene ? kind_image : script.kind,
                                 script.kind == kind_scene ? 0 : script.index, s, s) +
                             script.bound->script_source;
        {
            // Registered before it runs: its top level reads thisLayer and thisObject.
            JSValue args[5] = {
                JS_NewInt32(ctx, static_cast<int>(s)),
                JS_NewInt32(ctx, script.kind == kind_scene ? kind_image : script.kind),
                JS_NewInt32(ctx, script.kind == kind_scene ? 0 : script.index),
                JS_NewInt32(ctx, script.effect), JS_NewString(ctx, script.field.c_str())};
            JS_FreeValue(ctx, impl.call(register_fn, 5, args));
            for (JSValue& a : args) JS_FreeValue(ctx, a);
        }
        impl.arm();
        JSValue mod = JS_Eval(ctx, source.data(), source.size(), script.label.c_str(),
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(mod)) {
            impl.warn_once(script.label + ": " + impl.exception());
            script.failed = true;
            continue;
        }
        auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(mod));
        JSValue ret = JS_EvalFunction(ctx, mod);
        bool failed = JS_IsException(ret);
        if (!failed && JS_IsPromise(ret) && JS_PromiseState(ctx, ret) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, ret);
            JS_Throw(ctx, reason);
            failed = true;
        }
        if (failed) {
            impl.warn_once(script.label + ": " + impl.exception());
            script.failed = true;
            JS_FreeValue(ctx, ret);
            continue;
        }
        JS_FreeValue(ctx, ret);
        JSValue ns = JS_GetModuleNamespace(ctx, def);
        JSValue args[2] = {JS_NewInt32(ctx, static_cast<int>(s)), ns};
        JS_FreeValue(ctx, impl.call(namespace_fn, 2, args));
        for (JSValue& a : args) JS_FreeValue(ctx, a);
        JSValue ns2 = JS_GetModuleNamespace(ctx, def);
        auto has = [&](const char* name) {
            JSValue f = JS_GetPropertyStr(ctx, ns2, name);
            bool yes = JS_IsFunction(ctx, f);
            JS_FreeValue(ctx, f);
            return yes;
        };
        script.has_update = has("update");
        script.cursor_handlers = has("cursorEnter") || has("cursorLeave") || has("cursorMove") ||
                                 has("cursorDown") || has("cursorUp") || has("cursorClick");
        if (script.cursor_handlers) uses_pointer_ = true;
        JS_FreeValue(ctx, ns2);
    }
    JS_FreeValue(ctx, register_fn);
    JS_FreeValue(ctx, namespace_fn);
    // input.cursorWorldPosition is read by scripts that follow the pointer.
    for (const Script& s : scripts_) {
        if (s.bound->script_source.find("cursorWorldPosition") != std::string::npos ||
            s.bound->script_source.find("cursorScreenPosition") != std::string::npos) {
            uses_pointer_ = true;
        }
        if (s.bound->script_source.find("registerAudioBuffers") != std::string::npos) {
            uses_audio_ = true;
        }
        if (s.bound->script_source.find("media") != std::string::npos) uses_media_ = true;
    }
}

ScriptHost::~ScriptHost() {
    Impl& impl = *impl_;
    impl.flush_storage();
    if (impl.ctx != nullptr) {
        for (JSValue* v : {&impl.glint, &impl.run, &impl.event, &impl.tick, &impl.audio_tick,
                           &impl.user_props, &impl.load_storage, &impl.cursor_event, &impl.engine,
                           &impl.input, &impl.audio_left, &impl.audio_right}) {
            JS_FreeValue(impl.ctx, *v);
        }
        JS_FreeContext(impl.ctx);
    }
    if (impl.rt != nullptr) JS_FreeRuntime(impl.rt);
}

namespace {

// engine.* and input.* for the frame.
void set_frame(ScriptHost::Impl& impl, const ScriptFrame& f) {
    JSContext* ctx = impl.ctx;
    JS_SetPropertyStr(ctx, impl.engine, "frametime", JS_NewFloat64(ctx, f.delta));
    JS_SetPropertyStr(ctx, impl.engine, "runtime", JS_NewFloat64(ctx, f.time));
    JS_SetPropertyStr(ctx, impl.engine, "timeOfDay", JS_NewFloat64(ctx, f.daytime));
    auto set_vec = [&](JSValueConst object, const char* name, float x, float y) {
        JSValue v = JS_GetPropertyStr(ctx, object, name);
        JS_SetPropertyStr(ctx, v, "x", JS_NewFloat64(ctx, x));
        JS_SetPropertyStr(ctx, v, "y", JS_NewFloat64(ctx, y));
        JS_FreeValue(ctx, v);
    };
    set_vec(impl.engine, "canvasSize", f.canvas[0], f.canvas[1]);
    set_vec(impl.engine, "screenResolution", f.screen[0], f.screen[1]);
    set_vec(impl.input, "cursorWorldPosition", f.pointer_world[0], f.pointer_world[1]);
    set_vec(impl.input, "cursorScreenPosition", f.pointer_screen[0], f.pointer_screen[1]);
    JS_SetPropertyStr(ctx, impl.input, "cursorLeftDown", JS_NewBool(ctx, f.left_down));
}

}  // namespace

void ScriptHost::start(const Values& values, const ScriptFrame& frame) {
    if (scripts_.empty()) return;
    Impl& impl = *impl_;
    JSContext* ctx = impl.ctx;
    impl.time = frame.time;
    current_values = &values;
    set_frame(impl, frame);
    for (std::size_t s = 0; s < scripts_.size(); ++s) {
        Script& script = scripts_[s];
        if (script.failed) continue;
        JSValue args[3] = {JS_NewInt32(ctx, static_cast<int>(s)), JS_NewString(ctx, "init"),
                           to_js(ctx, script.bound->at(values, frame.time))};
        JSValue r = impl.call(impl.run, 3, args);
        for (JSValue& a : args) JS_FreeValue(ctx, a);
        if (JS_IsException(r)) {
            impl.warn_once(script.label + ": init: " + impl.exception());
            script.failed = true;
            continue;
        }
        if (!JS_IsUndefined(r)) impl.field_set_result(script, from_js(ctx, r));
        JS_FreeValue(ctx, r);
    }
    values_changed(Values{}, values);
    current_values = nullptr;
}

void ScriptHost::values_changed(const Values& before, const Values& after) {
    if (scripts_.empty()) return;
    Impl& impl = *impl_;
    JSContext* ctx = impl.ctx;
    json changed = json::object();
    for (const auto& [name, value] : after) {
        auto it = before.find(name);
        if (it == before.end() || it->second != value) changed[name] = value;
    }
    if (changed.empty()) return;
    const Values* saved = current_values;
    current_values = &after;
    JSValue colors = JS_NewArray(ctx);
    for (std::size_t i = 0; i < impl.color_names.size(); ++i) {
        JS_SetPropertyUint32(ctx, colors, static_cast<std::uint32_t>(i),
                             JS_NewString(ctx, impl.color_names[i].c_str()));
    }
    // engine.userProperties holds every value; the event gets what changed.
    JSValue all_args[2] = {to_js(ctx, json(after)), colors};
    JSValue all = impl.call(impl.user_props, 2, all_args);
    JS_SetPropertyStr(ctx, impl.engine, "userProperties", all);
    JS_FreeValue(ctx, all_args[0]);
    JSValue changed_args[2] = {to_js(ctx, changed), colors};
    JSValue arg = impl.call(impl.user_props, 2, changed_args);
    JS_FreeValue(ctx, changed_args[0]);
    JS_FreeValue(ctx, colors);
    for (std::size_t s = 0; s < scripts_.size(); ++s) {
        Script& script = scripts_[s];
        if (script.failed) continue;
        JSValue args[3] = {JS_NewInt32(ctx, static_cast<int>(s)),
                           JS_NewString(ctx, "applyUserProperties"), JS_DupValue(ctx, arg)};
        JSValue r = impl.call(impl.event, 3, args);
        for (JSValue& a : args) JS_FreeValue(ctx, a);
        if (JS_IsException(r)) {
            impl.warn_once(script.label + ": applyUserProperties: " + impl.exception());
            script.failed = true;
        }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, arg);
    current_values = saved;
}

void ScriptHost::frame(const Values& values, const ScriptFrame& frame) {
    if (scripts_.empty()) return;
    Impl& impl = *impl_;
    JSContext* ctx = impl.ctx;
    impl.time = frame.time;
    current_values = &values;
    set_frame(impl, frame);
    {
        JSValue ms = JS_NewFloat64(ctx, frame.time * 1000.0);
        JSValue r = impl.call(impl.tick, 1, &ms);
        if (JS_IsException(r)) impl.warn_once("timers: " + impl.exception());
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, ms);
    }
    if (uses_audio_ && impl.audio_left_data != nullptr && impl.audio_right_data != nullptr) {
        for (int i = 0; i < 64; ++i) {
            impl.audio_left_data[i] = frame.audio_left != nullptr ? frame.audio_left[i] : 0.0f;
            impl.audio_right_data[i] = frame.audio_right != nullptr ? frame.audio_right[i] : 0.0f;
        }
        JSValue args[2] = {impl.audio_left, impl.audio_right};
        JSValue r = impl.call(impl.audio_tick, 2, args);
        if (JS_IsException(r)) impl.warn_once("audio: " + impl.exception());
        JS_FreeValue(ctx, r);
    }
    for (std::size_t s = 0; s < scripts_.size(); ++s) {
        Script& script = scripts_[s];
        if (script.failed || !script.has_update) continue;
        JSValue args[3] = {JS_NewInt32(ctx, static_cast<int>(s)), JS_NewString(ctx, "update"),
                           to_js(ctx, script.bound->at(values, frame.time))};
        JSValue r = impl.call(impl.run, 3, args);
        for (JSValue& a : args) JS_FreeValue(ctx, a);
        if (JS_IsException(r)) {
            impl.warn_once(script.label + ": update: " + impl.exception());
            script.failed = true;
            continue;
        }
        if (!JS_IsUndefined(r)) impl.field_set_result(script, from_js(ctx, r));
        JS_FreeValue(ctx, r);
    }
    if (impl.storage_dirty && frame.time - impl.storage_written > 2) impl.flush_storage();
    current_values = nullptr;
}

void ScriptHost::cursor(const Values& values, const ScriptFrame& frame, bool down_edge,
                        bool up_edge) {
    if (scripts_.empty() || !uses_pointer_) return;
    Impl& impl = *impl_;
    JSContext* ctx = impl.ctx;
    current_values = &values;
    for (std::size_t s = 0; s < scripts_.size(); ++s) {
        Script& script = scripts_[s];
        if (script.failed || !script.cursor_handlers) continue;
        // Inside the layer's box: its size scaled about its origin, rotation ignored.
        Vec3 origin =
            parse_vec3(impl.field_value(script.kind, script.index, "origin", values), {0, 0, 0});
        Vec3 scale =
            parse_vec3(impl.field_value(script.kind, script.index, "scale", values), {1, 1, 1});
        Vec2 size = parse_vec2(impl.field_value(script.kind, script.index, "size", values), {0, 0});
        float w = size[0] * scale[0], h = size[1] * scale[1];
        float lx = frame.pointer_world[0] - (origin[0] - w / 2);
        float ly = (origin[1] + h / 2) - frame.pointer_world[1];
        bool inside = w > 0 && h > 0 && lx >= 0 && lx <= w && ly >= 0 && ly <= h;
        float local[2] = {scale[0] != 0 ? lx / scale[0] : 0, scale[1] != 0 ? ly / scale[1] : 0};
        auto fire = [&](const char* name) {
            JSValue world = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, world, 0, JS_NewFloat64(ctx, frame.pointer_world[0]));
            JS_SetPropertyUint32(ctx, world, 1, JS_NewFloat64(ctx, frame.pointer_world[1]));
            JSValue loc = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, loc, 0, JS_NewFloat64(ctx, local[0]));
            JS_SetPropertyUint32(ctx, loc, 1, JS_NewFloat64(ctx, local[1]));
            JSValue ev_args[2] = {world, loc};
            JSValue ev = impl.call(impl.cursor_event, 2, ev_args);
            JS_FreeValue(ctx, world);
            JS_FreeValue(ctx, loc);
            JSValue args[3] = {JS_NewInt32(ctx, static_cast<int>(s)), JS_NewString(ctx, name), ev};
            JSValue r = impl.call(impl.event, 3, args);
            for (JSValue& a : args) JS_FreeValue(ctx, a);
            if (JS_IsException(r)) {
                impl.warn_once(script.label + ": " + name + ": " + impl.exception());
                script.failed = true;
            }
            JS_FreeValue(ctx, r);
        };
        if (inside && !script.hovered) {
            script.hovered = true;
            fire("cursorEnter");
        } else if (!inside && script.hovered) {
            script.hovered = false;
            fire("cursorLeave");
        }
        if (inside) fire("cursorMove");
        if (down_edge && inside) {
            script.pressed = true;
            fire("cursorDown");
        }
        if (up_edge) {
            if (inside) fire("cursorUp");
            if (script.pressed && inside) fire("cursorClick");
            script.pressed = false;
        }
    }
    current_values = nullptr;
}

bool ScriptHost::empty() const {
    return scripts_.empty();
}

std::size_t ScriptHost::count() const {
    return scripts_.size();
}

void ScriptHost::media(const Values& values, const MediaState& state, const MediaEvents& events) {
    if (scripts_.empty() || !uses_media_) return;
    Impl& impl = *impl_;
    JSContext* ctx = impl.ctx;
    current_values = &values;
    auto vec3 = [&](JSValueConst object, const char* name, const float* c) {
        JSValue v = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, v, "x", JS_NewFloat64(ctx, c[0]));
        JS_SetPropertyStr(ctx, v, "y", JS_NewFloat64(ctx, c[1]));
        JS_SetPropertyStr(ctx, v, "z", JS_NewFloat64(ctx, c[2]));
        // A real Vec3, so scripts may call its methods.
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, global, "Vec3");
        JSValue made = JS_CallConstructor(ctx, ctor, 1, &v);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, v);
        JS_SetPropertyStr(ctx, object, name, made);
    };
    auto send = [&](const char* name, JSValue ev) {
        for (std::size_t s = 0; s < scripts_.size(); ++s) {
            Script& script = scripts_[s];
            if (script.failed) continue;
            JSValue args[3] = {JS_NewInt32(ctx, static_cast<int>(s)), JS_NewString(ctx, name),
                               JS_DupValue(ctx, ev)};
            JSValue r = impl.call(impl.event, 3, args);
            for (JSValue& a : args) JS_FreeValue(ctx, a);
            if (JS_IsException(r)) {
                impl.warn_once(script.label + ": " + name + ": " + impl.exception());
                script.failed = true;
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, ev);
    };
    if (events.status) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "enabled", JS_NewBool(ctx, true));
        send("mediaStatusChanged", ev);
    }
    if (events.properties) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "title", JS_NewString(ctx, state.title.c_str()));
        JS_SetPropertyStr(ctx, ev, "artist", JS_NewString(ctx, state.artist.c_str()));
        JS_SetPropertyStr(ctx, ev, "subTitle", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, ev, "albumTitle", JS_NewString(ctx, state.album.c_str()));
        JS_SetPropertyStr(ctx, ev, "albumArtist", JS_NewString(ctx, state.album_artist.c_str()));
        JS_SetPropertyStr(ctx, ev, "genres", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, ev, "contentType", JS_NewString(ctx, state.content_type.c_str()));
        send("mediaPropertiesChanged", ev);
    }
    if (events.playback) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "state", JS_NewInt32(ctx, state.playback));
        send("mediaPlaybackChanged", ev);
    }
    if (events.thumbnail) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "hasThumbnail", JS_NewBool(ctx, state.has_thumbnail));
        vec3(ev, "primaryColor", state.primary);
        vec3(ev, "secondaryColor", state.secondary);
        vec3(ev, "tertiaryColor", state.tertiary);
        vec3(ev, "textColor", state.text_color);
        vec3(ev, "highContrastColor", state.high_contrast);
        send("mediaThumbnailChanged", ev);
    }
    if (events.timeline) {
        JSValue ev = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ev, "position", JS_NewFloat64(ctx, state.position));
        JS_SetPropertyStr(ctx, ev, "duration", JS_NewFloat64(ctx, state.duration));
        send("mediaTimelineChanged", ev);
    }
    current_values = nullptr;
}

std::vector<int> ScriptHost::take_changed_texts() {
    std::vector<int> out(changed_texts_.begin(), changed_texts_.end());
    changed_texts_.clear();
    return out;
}

const std::vector<AnimationLayer>* ScriptHost::animation_layers(int image) const {
    const Impl& impl = *impl_;
    if (image < 0 || static_cast<std::size_t>(image) >= impl.layers.size()) return nullptr;
    const auto& l = impl.layers[static_cast<std::size_t>(image)];
    return l ? &*l : nullptr;
}

double ScriptHost::layer_time(int image, int layer, double time) const {
    const Impl& impl = *impl_;
    if (image < 0 || static_cast<std::size_t>(image) >= impl.layer_states.size()) return time;
    const auto& states = impl.layer_states[static_cast<std::size_t>(image)];
    if (layer < 0 || static_cast<std::size_t>(layer) >= states.size()) return time;
    const AnimationLayerState& s = states[static_cast<std::size_t>(layer)];
    if (!s.playing) return s.held;
    return s.held_set ? (time - s.start) + s.held : time;
}

}  // namespace glint::scene

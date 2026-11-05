#include "wallpaper/scene/scene_wallpaper.hpp"

#include <time.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/worker.hpp"
#include "gpu/context.hpp"
#include "gpu/surface.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace glint::scene {

namespace {

// Seconds since local midnight, read once per scene start; frames add their own time.
double local_day_seconds() {
    time_t now = ::time(nullptr);
    tm local{};
    ::localtime_r(&now, &local);
    return local.tm_hour * 3600.0 + local.tm_min * 60.0 + local.tm_sec;
}

// A property value as JSON text for IPC: colours as #rrggbb, the rest as stored.
std::string to_ipc(const ProjectProperty& p, const nlohmann::json& value) {
    if (p.type == "color" && value.is_string()) {
        return nlohmann::json(color_to_hex(value.get<std::string>())).dump();
    }
    if (p.type == "bool" && value.is_number()) return value.get<double>() != 0 ? "true" : "false";
    return value.dump();
}

}  // namespace

namespace {

// A value as a client or the state file gives it, in the form the project stores (colours
// as "r g b", combos as their option string, sliders within their range); an error when it
// is not one the property takes.
std::string normalize_property(const ProjectProperty& p, nlohmann::json& value) {
    if (p.type == "color") {
        if (value.is_string() && value.get<std::string>().starts_with("#")) {
            auto rgb = hex_to_color(value.get<std::string>());
            if (!rgb) return "a colour is #rrggbb";
            value = *rgb;
        } else {
            // "r g b" as the project stores it.
            int numbers = 0;
            if (value.is_string()) {
                std::istringstream in(value.get<std::string>());
                float f = 0;
                while (in >> f) ++numbers;
            }
            if (numbers != 3) return "a colour is #rrggbb";
        }
        return {};
    }
    if (p.type == "combo") {
        if (!value.is_string()) value = value.dump();
        if (!p.options.empty()) {
            std::string v = value.get<std::string>();
            bool known = false;
            for (const auto& [label, option] : p.options) known = known || option == v;
            if (!known) return "not one of the options";
        }
        return {};
    }
    if (p.type == "bool") {
        if (value.is_number()) value = value.get<double>() != 0;
        if (!value.is_boolean()) return "a bool is true or false";
        return {};
    }
    if (p.type == "slider") {
        if (!value.is_number()) return "a slider takes a number";
        double v = value.get<double>();
        if (p.min.is_number()) v = std::max(v, p.min.get<double>());
        if (p.max.is_number()) v = std::min(v, p.max.get<double>());
        value = v;
        return {};
    }
    if (p.type == "textinput" || p.type == "text" || p.type == "file" || p.type == "directory" ||
        p.type == "scenetexture") {
        if (!value.is_string()) return "a string is wanted";
        return {};
    }
    return {};
}

}  // namespace

SceneCache::SceneCache(gpu::Context& gpu, gpu::StagingRing& staging, Workers& workers)
    : gpu_(gpu), staging_(staging), workers_(workers) {}

void SceneCache::request(const std::filesystem::path& item, Callback callback) {
    std::string key = item.string();
    if (auto it = ready_.find(key); it != ready_.end()) {
        if (auto assets = it->second.lock()) {
            callback({std::move(assets), {}});
            return;
        }
        ready_.erase(it);
    }
    auto& waiters = loading_[key];
    waiters.push_back(std::move(callback));
    if (waiters.size() > 1) return;

    auto result = std::make_shared<Result>();
    gpu::Context& gpu = gpu_;
    gpu::StagingRing& staging = staging_;
    // The worker gets a plain pointer: the callback owns the result, so the assets are always
    // released on the main thread, never on the worker.
    Result* raw = result.get();
    workers_.post(
        [item, raw, &gpu, &staging] {
            raw->assets = SceneAssets::load(item, default_assets(), paths::cache_dir() / "spirv",
                                            gpu, staging, raw->error);
        },
        [this, key, result] {
            if (result->assets) {
                result->assets->finish();
                ready_[key] = result->assets;
                const SceneAssets& a = *result->assets;
                log::info("scene {}: {} on a worker, {} shaders compiled, {} from cache",
                          a.project().title, std::format("{:.0f} ms", a.load_ms()), a.compiled(),
                          a.cached());
                for (const auto& w : a.warnings()) log::warn("scene {}: {}", a.project().title, w);
            }
            auto node = loading_.extract(key);
            if (node.empty()) return;
            for (auto& waiter : node.mapped()) waiter(*result);
        });
}

Scene::Scene(std::filesystem::path path, const Options& options, Services& services)
    : path_(std::move(path)), options_(options), services_(services) {}

Scene::~Scene() {
    player_.reset();
    if (assets_ && assets_->sound_owner == this) assets_->sound_owner = nullptr;
}

void Scene::start_sound() {
    float volume = options_.mute ? 0.0f : options_.volume;
    if (volume <= 0 || assets_->sound_owner != nullptr) return;
    const Values& values = renderer_->values();
    std::vector<audio::TrackSpec> tracks;
    sound_tracks_.assign(assets_->scene().sounds.size(), -1);
    for (std::size_t si = 0; si < assets_->scene().sounds.size(); ++si) {
        const SoundObject& sound = assets_->scene().sounds[si];
        if (!sound.visible.resolve_bool(values, true)) continue;
        audio::TrackSpec spec;
        spec.mode = sound.mode;
        spec.volume = sound.volume.resolve_float(values, 1);
        // Silent at start: a script starts it.
        spec.playing = !sound.start_silent;
        spec.min_pause = sound.min_time;
        spec.max_pause = sound.max_time;
        std::size_t first = sound_files_.size();
        for (const std::string& path : sound.files) {
            auto file = assets_->vfs().read(path);
            if (!file) {
                log::warn("sound {}: not found", path);
                continue;
            }
            sound_files_.push_back(std::move(*file));
        }
        // Moving a File keeps its bytes where they are, so the views stay valid.
        for (std::size_t i = first; i < sound_files_.size(); ++i) {
            spec.files.push_back(sound_files_[i].bytes());
        }
        if (!spec.files.empty()) {
            sound_tracks_[si] = static_cast<int>(tracks.size());
            tracks.push_back(std::move(spec));
        }
    }
    if (tracks.empty()) return;
    player_ = std::make_unique<audio::Player>();
    if (std::string error = player_->start(std::move(tracks), volume); !error.empty()) {
        log::warn("sound: {}", error);
        player_.reset();
        return;
    }
    assets_->sound_owner = this;
}

void Scene::pause() {
    if (player_) player_->set_paused(true);
}

void Scene::resume() {
    if (player_) player_->set_paused(false);
}

void Scene::load(std::function<void(std::string error)> done) {
    std::weak_ptr<bool> alive = alive_;
    services_.scenes.request(
        path_, [this, alive, done = std::move(done)](const SceneCache::Result& result) {
            if (alive.expired()) return;
            if (!result.assets) {
                done(result.error);
                return;
            }
            assets_ = result.assets;
            renderer_ = std::make_unique<SceneRenderer>(services_.gpu, services_.blit, assets_);
            // Values set over IPC earlier for this project.
            if (options_.properties.is_object()) {
                // Values from the state file or a set: through the same conversion as a
                // props request, so a colour given as #rrggbb comes back.
                Values values = renderer_->values();
                for (const auto& [name, value] : options_.properties.items()) {
                    const ProjectProperty* p = assets_->project().property(name);
                    if (p == nullptr) continue;
                    nlohmann::json v = value;
                    if (normalize_property(*p, v).empty()) values[name] = v;
                }
                renderer_->set_values(values);
            }
            last_time_ = local_day_seconds();
            start_sound();
            done({});
        });
}

void Scene::prepare(const Frame& frame, VkCommandBuffer cmd, VkExtent2D extent) {
    if (!renderer_) return;
    extent_ = extent;
    FrameInputs inputs;
    inputs.time = frame.time;
    inputs.delta = frame.delta;
    inputs.daytime = std::fmod((last_time_ + frame.time) / 86400.0, 1.0);
    inputs.pointer[0] = pointer_[0];
    inputs.pointer[1] = pointer_[1];
    inputs.pointer_last[0] = pointer_last_[0];
    inputs.pointer_last[1] = pointer_last_[1];
    pointer_last_[0] = pointer_[0];
    pointer_last_[1] = pointer_[1];
    // The camera parallax follows the pointer with the scene's delay.
    const SceneGeneral& general = assets_->scene().general;
    float follow = general.parallax_delay > 0
                       ? std::min(1.0f, static_cast<float>(frame.delta) / general.parallax_delay)
                       : 1.0f;
    for (int i = 0; i < 2; ++i) {
        parallax_[i] += (pointer_[i] - parallax_[i]) * follow;
        if (std::fabs(pointer_[i] - parallax_[i]) < 1e-4f) parallax_[i] = pointer_[i];
    }
    inputs.parallax[0] = parallax_[0];
    inputs.parallax[1] = parallax_[1];
    if (audio_) {
        inputs.audio_left = audio_left_.data();
        inputs.audio_right = audio_right_.data();
    }
    inputs.buttons = buttons_;
    renderer_->record(cmd, extent, inputs);
    apply_script_sounds();
}

// What scripts did to the sound objects since the last frame, onto the player.
void Scene::apply_script_sounds() {
    if (!player_ || !renderer_) return;
    const std::vector<SoundState>* states = renderer_->script_sounds();
    if (states == nullptr) return;
    sound_applied_.resize(states->size(), 0);
    sound_restarts_.resize(states->size(), 0);
    for (std::size_t i = 0; i < states->size() && i < sound_tracks_.size(); ++i) {
        const SoundState& st = (*states)[i];
        if (st.version == sound_applied_[i] || sound_tracks_[i] < 0) continue;
        auto track = static_cast<std::size_t>(sound_tracks_[i]);
        if (st.restarts != sound_restarts_[i]) {
            player_->restart_track(track);
            sound_restarts_[i] = st.restarts;
        }
        player_->set_track(track, st.playing, st.volume);
        sound_applied_[i] = st.version;
    }
}

bool Scene::render(const Frame&, const gpu::Target& target, float opacity) {
    if (!renderer_) return false;
    renderer_->composite(target, opacity);
    return true;
}

bool Scene::wants_frames() const {
    if (!assets_) return false;
    if (assets_->animated()) return true;
    if (renderer_ && renderer_->scripted()) return true;
    // A scene that follows the cursor keeps drawing, as Wallpaper Engine does: the pointer is
    // read with every frame.
    return wants_cursor();
}

bool Scene::wants_audio() const {
    // The project says so, or a shader reads the spectrum.
    return assets_ && renderer_ && (assets_->project().audio || renderer_->uses_audio());
}

void Scene::audio_spectrum(const float* left, const float* right) {
    if (left == nullptr || right == nullptr) {
        audio_ = false;
        return;
    }
    std::copy_n(left, 64, audio_left_.begin());
    std::copy_n(right, 64, audio_right_.begin());
    audio_ = true;
}

bool Scene::wants_cursor() const {
    if (!assets_ || !renderer_) return false;
    if (assets_->scene().general.camera_parallax) {
        // The project may switch the parallax off through a bound property.
        return true;
    }
    return renderer_->uses_pointer();
}

void Scene::cursor(const Cursor& cursor) {
    pointer_[0] = std::clamp(static_cast<float>(cursor.x / extent_.width), 0.0f, 1.0f);
    pointer_[1] = std::clamp(static_cast<float>(cursor.y / extent_.height), 0.0f, 1.0f);
    buttons_ = cursor.buttons;
}

std::uint64_t Scene::memory() const {
    std::uint64_t total = 0;
    if (assets_) total += assets_->memory();
    if (renderer_) total += renderer_->memory();
    return total;
}

Values Scene::property_values() const {
    return renderer_ ? renderer_->values() : Values{};
}

std::vector<Property> Scene::properties() const {
    std::vector<Property> out;
    if (!assets_ || !renderer_) return out;
    const Values& values = renderer_->values();
    for (const ProjectProperty& p : assets_->project().properties) {
        Property entry;
        entry.name = p.name;
        entry.type = p.type;
        entry.text = p.text;
        auto it = values.find(p.name);
        entry.value = to_ipc(p, it != values.end() ? it->second : p.value);
        entry.def = to_ipc(p, p.value);
        nlohmann::json extra = nlohmann::json::object();
        if (!p.min.is_null()) extra["min"] = p.min;
        if (!p.max.is_null()) extra["max"] = p.max;
        if (!p.step.is_null()) extra["step"] = p.step;
        if (!p.options.empty()) {
            extra["options"] = nlohmann::json::array();
            for (const auto& [label, value] : p.options) {
                extra["options"].push_back({{"label", label}, {"value", value}});
            }
        }
        if (!p.condition.empty()) extra["condition"] = p.condition;
        entry.extra = extra.dump();
        entry.visible = evaluate_condition(p.condition, values);
        entry.order = p.order;
        out.push_back(std::move(entry));
    }
    return out;
}

std::string Scene::check_property(std::string_view name, std::string_view json) const {
    if (!assets_ || !renderer_) return "still loading";
    const ProjectProperty* p = assets_->project().property(name);
    if (p == nullptr) return "no such setting";
    nlohmann::json value = nlohmann::json::parse(json, nullptr, false);
    if (value.is_discarded()) return "not JSON";
    return normalize_property(*p, value);
}

std::vector<std::string> Scene::set_property(std::string_view name, std::string_view json) {
    if (!assets_ || !renderer_) return {};
    const ProjectProperty* p = assets_->project().property(name);
    if (p == nullptr) return {};
    nlohmann::json value = nlohmann::json::parse(json, nullptr, false);
    if (value.is_discarded()) return {};
    if (!normalize_property(*p, value).empty()) return {};
    Values values = renderer_->values();
    values[std::string(name)] = value;
    renderer_->set_values(values);
    return {std::string(name)};
}

std::vector<std::string> Scene::reset_properties() {
    if (!assets_ || !renderer_) return {};
    renderer_->set_values(default_values(assets_->project()));
    std::vector<std::string> names;
    for (const auto& p : assets_->project().properties) names.push_back(p.name);
    return names;
}

}  // namespace glint::scene

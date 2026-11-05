#include "wallpaper/scene/particles.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

#include "wallpaper/scene/vfs.hpp"

namespace glint::scene {

namespace {

using nlohmann::json;

float num(const json& j, const char* key, float fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    const json& v = j[key];
    if (v.is_number()) return v.get<float>();
    if (v.is_string()) return std::strtof(v.get<std::string>().c_str(), nullptr);
    return fallback;
}

std::string text(const json& j, const char* key, const std::string& fallback = {}) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return fallback;
}

Vec3 vec(const json& j, const char* key, Vec3 fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    return parse_vec3(j[key], fallback);
}

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 operator-(Vec3 a, Vec3 b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 operator*(Vec3 a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
Vec3 operator*(Vec3 a, Vec3 b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
float dot(Vec3 a, Vec3 b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float length(Vec3 a) {
    return std::sqrt(dot(a, a));
}
Vec3 normalized(Vec3 a) {
    float l = length(a);
    return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 0};
}
float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// A small hash for per-particle randoms that do not change from frame to frame.
std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float hashed(std::uint32_t seed, std::uint32_t k) {
    return static_cast<float>(hash(seed + k * 0x9e3779b9U) >> 8) / 16777216.0f;
}

// Gradient noise in three dimensions, -1 to 1, for turbulence.
float grad(std::uint32_t h, float x, float y, float z) {
    switch (h & 15u) {
        case 0: return x + y;
        case 1: return -x + y;
        case 2: return x - y;
        case 3: return -x - y;
        case 4: return x + z;
        case 5: return -x + z;
        case 6: return x - z;
        case 7: return -x - z;
        case 8: return y + z;
        case 9: return -y + z;
        case 10: return y - z;
        case 11: return -y - z;
        case 12: return y + x;
        case 13: return -y + z;
        case 14: return y - x;
        default: return -y - z;
    }
}
float noise(float x, float y, float z) {
    auto fl = [](float v) { return std::floor(v); };
    float xf = fl(x), yf = fl(y), zf = fl(z);
    auto xi = static_cast<std::int32_t>(xf), yi = static_cast<std::int32_t>(yf),
         zi = static_cast<std::int32_t>(zf);
    x -= xf;
    y -= yf;
    z -= zf;
    auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    float u = fade(x), v = fade(y), w = fade(z);
    auto h = [&](int dx, int dy, int dz) {
        return hash(static_cast<std::uint32_t>(xi + dx) * 73856093u ^
                    static_cast<std::uint32_t>(yi + dy) * 19349663u ^
                    static_cast<std::uint32_t>(zi + dz) * 83492791u);
    };
    float n000 = grad(h(0, 0, 0), x, y, z), n100 = grad(h(1, 0, 0), x - 1, y, z);
    float n010 = grad(h(0, 1, 0), x, y - 1, z), n110 = grad(h(1, 1, 0), x - 1, y - 1, z);
    float n001 = grad(h(0, 0, 1), x, y, z - 1), n101 = grad(h(1, 0, 1), x - 1, y, z - 1);
    float n011 = grad(h(0, 1, 1), x, y - 1, z - 1), n111 = grad(h(1, 1, 1), x - 1, y - 1, z - 1);
    float a = lerp(lerp(n000, n100, u), lerp(n010, n110, u), v);
    float b = lerp(lerp(n001, n101, u), lerp(n011, n111, u), v);
    return lerp(a, b, w);
}
Vec3 noise3(Vec3 p) {
    return {noise(p[0], p[1], p[2]), noise(p[1] + 31.7f, p[2] + 17.3f, p[0] + 5.1f),
            noise(p[2] + 101.3f, p[0] + 47.9f, p[1] + 9.7f)};
}

void read_modules(const json& j, const char* key, std::vector<ParticleModule>& out) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_array()) return;
    for (const json& m : j[key]) {
        if (!m.is_object()) continue;
        ParticleModule module;
        module.name = text(m, "name");
        module.json = m;
        out.push_back(std::move(module));
    }
}

std::shared_ptr<ParticleDesc> parse_desc(const Vfs& vfs, const json& j, const std::string& file,
                                         std::vector<std::string>& warnings, int depth);

void parse_children(const Vfs& vfs, const json& j, ParticleDesc& desc,
                    std::vector<std::string>& warnings, int depth) {
    if (!j.contains("children") || !j["children"].is_array() || depth > 3) return;
    for (const json& c : j["children"]) {
        if (!c.is_object()) continue;
        ParticleDesc::Child child;
        child.type = text(c, "type");
        child.scale = vec(c, "scale", {1, 1, 1});
        child.probability = num(c, "probability", 1);
        child.maxcount = static_cast<int>(num(c, "maxcount", 0));
        std::string name = text(c, "name");
        if (c.contains("emitter") || c.contains("renderer")) {
            child.desc = parse_desc(vfs, c, name.empty() ? desc.file : name, warnings, depth + 1);
        } else if (!name.empty()) {
            auto f = vfs.read(name);
            json parsed = f ? json::parse(f->text(), nullptr, false) : json();
            if (parsed.is_object()) {
                child.desc = parse_desc(vfs, parsed, name, warnings, depth + 1);
            } else {
                warnings.push_back(desc.file + ": child " + name + " not found");
            }
        }
        if (child.desc) desc.children.push_back(std::move(child));
    }
}

std::shared_ptr<ParticleDesc> parse_desc(const Vfs& vfs, const json& j, const std::string& file,
                                         std::vector<std::string>& warnings, int depth) {
    auto desc = std::make_shared<ParticleDesc>();
    desc->file = file;
    desc->maxcount = std::clamp(static_cast<int>(num(j, "maxcount", 100)), 1, 200000);
    desc->starttime = std::clamp(num(j, "starttime", 0), 0.0f, 30.0f);
    if (j.contains("flags") && j["flags"].is_number()) desc->flags = j["flags"].get<unsigned>();
    desc->animationmode = text(j, "animationmode");
    desc->sequencemultiplier = num(j, "sequencemultiplier", 1);
    read_modules(j, "emitter", desc->emitters);
    read_modules(j, "initializer", desc->initializers);
    read_modules(j, "operator", desc->operators);
    read_modules(j, "renderer", desc->renderers);
    if (j.contains("controlpoint") && j["controlpoint"].is_array()) {
        for (const json& c : j["controlpoint"]) {
            ParticleControlPoint point;
            point.offset = vec(c, "offset", {0, 0, 0});
            unsigned flags = c.is_object() && c.contains("flags") && c["flags"].is_number()
                                 ? c["flags"].get<unsigned>()
                                 : 0u;
            point.pointer = (flags & 1u) != 0 ||
                            (c.is_object() && c.contains("locktopointer") &&
                             c["locktopointer"].is_boolean() && c["locktopointer"].get<bool>());
            desc->control_points.push_back(point);
        }
    }
    std::string material = text(j, "material");
    if (!material.empty()) parse_material(vfs, material, desc->material, warnings);
    parse_children(vfs, j, *desc, warnings, depth);
    return desc;
}

}  // namespace

bool ParticleDesc::sprite_trail() const {
    for (const ParticleModule& r : renderers) {
        if (r.name == "spritetrail") return true;
    }
    return false;
}

bool ParticleDesc::rope() const {
    for (const ParticleModule& r : renderers) {
        if (r.name == "rope" || r.name == "ropetrail") return true;
    }
    return false;
}

std::shared_ptr<ParticleDesc> parse_particle(const Vfs& vfs, const std::string& path,
                                             std::vector<std::string>& warnings) {
    auto file = vfs.read(path);
    if (!file) {
        warnings.push_back(path + ": not found");
        return nullptr;
    }
    json parsed = json::parse(file->text(), nullptr, false);
    if (!parsed.is_object()) {
        warnings.push_back(path + ": not valid JSON");
        return nullptr;
    }
    return parse_desc(vfs, parsed, path, warnings, 0);
}

// The numbers of a module, read from its json once: the simulation never touches json.
struct ParticleSystem::Module {
    enum class Kind {
        unknown,
        // emitters
        sphere,
        box,
        // initializers
        lifetime,
        size,
        alpha,
        color,
        velocity,
        rotation,
        angular_velocity,
        turbulent_velocity,
        // operators
        movement,
        angular_movement,
        turbulence,
        vortex,
        attract,
        alpha_fade,
        size_change,
        alpha_change,
        color_change,
        oscillate_alpha,
        oscillate_size,
        oscillate_position,
    } kind = Kind::unknown;
    // Named after the json keys they come from; what a kind does not use stays unset.
    Vec3 origin{0, 0, 0}, directions{1, 1, 1}, min3{0, 0, 0}, max3{0, 0, 0};
    Vec3 mask{1, 1, 1}, gravity{0, 0, 0}, force{0, 0, 0}, offset3{0, 0, 0};
    Vec3 start3{1, 1, 1}, end3{1, 1, 1};
    float min = 0, max = 0, exponent = 1;
    float rate = 0, instantaneous = 0, duration = 0, speedmin = 0, speedmax = 0;
    float drag = 0, scale = 1, timescale = 1, offset = 0, threshold = 100;
    float inner = 0, outer = 100, speedinner = 0, speedouter = 0;
    float fadein = 0, fadeout = 0, starttime = 0, endtime = 1, startvalue = 0, endvalue = 1;
    float frequencymin = 1, frequencymax = 1, phasemin = 0, phasemax = 0;
    float scalemin = 0, scalemax = 1;
    int controlpoint = -1;
    bool has_min3 = false, has_max3 = false;
    // Emitters: distancemin/max as a number (sphere) or per axis (box).
    float sphere_min = 0, sphere_max = 100;
    Vec3 box_min{0, 0, 0}, box_max{100, 100, 0};
};

std::unique_ptr<ParticleSystem::Module> ParticleSystem::compile(const ParticleModule& m) {
    using Kind = ParticleSystem::Module::Kind;
    auto out = std::make_unique<ParticleSystem::Module>();
    const json& j = m.json;
    static const std::map<std::string, Kind> kinds = {
        {"sphererandom", Kind::sphere},
        {"boxrandom", Kind::box},
        {"lifetimerandom", Kind::lifetime},
        {"sizerandom", Kind::size},
        {"alpharandom", Kind::alpha},
        {"colorrandom", Kind::color},
        {"velocityrandom", Kind::velocity},
        {"rotationrandom", Kind::rotation},
        {"angularvelocityrandom", Kind::angular_velocity},
        {"turbulentvelocityrandom", Kind::turbulent_velocity},
        {"movement", Kind::movement},
        {"angularmovement", Kind::angular_movement},
        {"turbulence", Kind::turbulence},
        {"vortex", Kind::vortex},
        {"controlpointattract", Kind::attract},
        {"alphafade", Kind::alpha_fade},
        {"sizechange", Kind::size_change},
        {"alphachange", Kind::alpha_change},
        {"colorchange", Kind::color_change},
        {"oscillatealpha", Kind::oscillate_alpha},
        {"oscillatesize", Kind::oscillate_size},
        {"oscillateposition", Kind::oscillate_position},
    };
    if (auto it = kinds.find(m.name); it != kinds.end()) out->kind = it->second;
    out->origin = vec(j, "origin", {0, 0, 0});
    out->directions = vec(j, "directions", {1, 1, 1});
    out->has_min3 = j.is_object() && j.contains("min");
    out->has_max3 = j.is_object() && j.contains("max");
    out->min3 = vec(j, "min", {0, 0, 0});
    out->max3 = vec(j, "max", out->min3);
    out->min = num(j, "min", 0);
    out->max = num(j, "max", out->min);
    out->exponent = num(j, "exponent", 1);
    out->mask =
        vec(j, "mask", out->kind == Kind::oscillate_position ? Vec3{1, 1, 0} : Vec3{1, 1, 1});
    out->gravity = vec(j, "gravity", {0, 0, 0});
    out->force = vec(j, "force", {0, 0, 0});
    out->offset3 = vec(j, "offset", {0, 0, 0});
    out->start3 = vec(j, "startvalue", {1, 1, 1});
    out->end3 = vec(j, "endvalue", {1, 1, 1});
    out->rate = num(j, "rate", 0);
    out->instantaneous = num(j, "instantaneous", 0);
    out->duration = num(j, "duration", 0);
    out->speedmin = num(j, "speedmin", 0);
    out->speedmax = num(j, "speedmax", out->speedmin);
    out->drag = num(j, "drag", 0);
    out->scale = num(j, "scale", out->kind == Kind::attract ? 1000.0f : 1.0f);
    out->timescale = num(j, "timescale", 1);
    out->offset = num(j, "offset", 0);
    out->threshold = num(j, "threshold", 100);
    out->inner = num(j, "distanceinner", 0);
    out->outer = num(j, "distanceouter", 100);
    out->speedinner = num(j, "speedinner", 0);
    out->speedouter = num(j, "speedouter", 0);
    out->fadein = num(j, "fadeintime", 0);
    out->fadeout = num(j, "fadeouttime", 0);
    out->starttime = num(j, "starttime", 0);
    out->endtime = num(j, "endtime", 1);
    out->startvalue = num(j, "startvalue", out->kind == Kind::alpha_change ? 1.0f : 0.0f);
    out->endvalue = num(j, "endvalue", out->kind == Kind::alpha_change ? 0.0f : 1.0f);
    out->frequencymin = num(j, "frequencymin", 1);
    out->frequencymax = num(j, "frequencymax", out->frequencymin);
    out->phasemin = num(j, "phasemin", 0);
    out->phasemax = num(j, "phasemax", out->phasemin);
    out->scalemin = num(j, "scalemin", out->kind == Kind::oscillate_position ? 0.0f : 1.0f);
    out->scalemax =
        num(j, "scalemax", out->kind == Kind::oscillate_position ? 10.0f : out->scalemin);
    out->sphere_min = num(j, "distancemin", 0);
    out->sphere_max = num(j, "distancemax", 100);
    out->box_min = vec(j, "distancemin", {0, 0, 0});
    out->box_max = vec(j, "distancemax", {100, 100, 0});
    out->controlpoint = static_cast<int>(num(j, "controlpoint", -1));
    if (out->kind == Kind::attract && out->controlpoint < 0) out->controlpoint = 0;
    return out;
}

ParticleSystem::ParticleSystem(std::shared_ptr<const ParticleDesc> desc,
                               const ParticleOverride& over, std::uint32_t seed, Vec3 scale,
                               bool event_child)
    : desc_(std::move(desc)),
      over_(over),
      scale_(scale),
      rng_(seed * 0x9e3779b97f4a7c15ULL + 1),
      event_child_(event_child) {
    max_ = static_cast<std::size_t>(
        std::clamp(std::lround(static_cast<float>(desc_->maxcount) * over_.count), 1L, 200000L));
    particles_.reserve(max_);
    for (const ParticleModule& m : desc_->emitters) emitter_modules_.push_back(compile(m));
    for (const auto& m : emitter_modules_) emitters_.push_back({m.get()});
    for (const ParticleModule& m : desc_->initializers) initializers_.push_back(compile(m));
    for (const ParticleModule& m : desc_->operators) operators_.push_back(compile(m));
    for (const ParticleModule& r : desc_->renderers) {
        if (r.name == "rope" || r.name == "ropetrail") rope_length_ = num(r.json, "length", 1);
    }
    control_points_.resize(std::max<std::size_t>(desc_->control_points.size(), 8), {0, 0, 0});
    for (std::size_t i = 0; i < desc_->control_points.size(); ++i) {
        control_points_[i] = desc_->control_points[i].offset;
    }
    for (const ParticleDesc::Child& c : desc_->children) {
        ParticleOverride child_over = over;
        if (c.maxcount > 0) {
            child_over.count =
                static_cast<float>(c.maxcount) / static_cast<float>(c.desc->maxcount);
        }
        children_.push_back(std::make_unique<ParticleSystem>(
            c.desc, child_over, seed * 31u + 7u + static_cast<std::uint32_t>(children_.size()),
            scale * c.scale, !c.type.empty()));
    }
}

ParticleSystem::~ParticleSystem() = default;

float ParticleSystem::random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return static_cast<float>(rng_ >> 40) / 16777216.0f;
}

void ParticleSystem::step(const ParticleEnv& env) {
    for (std::size_t i = 0; i < desc_->control_points.size(); ++i) {
        const ParticleControlPoint& p = desc_->control_points[i];
        control_points_[i] = p.pointer ? env.pointer + p.offset : p.offset;
    }
    if (!warmed_) {
        warmed_ = true;
        // starttime: the system has already run that long when first seen.
        int steps = std::min(300, static_cast<int>(desc_->starttime * 30));
        for (int i = 0; i < steps; ++i) simulate(1.0 / 30, env);
    }
    simulate(std::clamp(env.delta, 0.0, 0.1), env);
    for (auto& child : children_) child->step(env);
}

void ParticleSystem::simulate(double dt_d, const ParticleEnv&) {
    auto dt = static_cast<float>(dt_d);
    time_ += dt_d;
    auto t = static_cast<float>(time_);
    using Kind = Module::Kind;

    // Emission, except for event children, which the parent feeds.
    if (!event_child_) {
        for (Emitter& e : emitters_) emit(e, dt_d, nullptr);
    }
    follow(dt_d);

    // The operators that move, then ageing.
    for (std::size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.age += dt;
        if (p.age >= p.life) {
            event("eventdeath", p);
            p = particles_.back();
            particles_.pop_back();
            continue;
        }
        for (const auto& op : operators_) {
            const Module& m = *op;
            switch (m.kind) {
                case Kind::movement: {
                    p.velocity = p.velocity + m.gravity * scale_ * dt;
                    if (m.drag > 0) p.velocity = p.velocity * std::max(0.0f, 1.0f - m.drag * dt);
                    p.position = p.position + p.velocity * dt;
                    break;
                }
                case Kind::angular_movement: {
                    p.angular = p.angular + m.force * dt;
                    p.rotation = p.rotation + p.angular * dt;
                    break;
                }
                case Kind::turbulence: {
                    float speed = lerp(m.speedmin, m.speedmax, hashed(p.seed, 40));
                    float phase = lerp(m.phasemin, m.phasemax, hashed(p.seed, 41));
                    Vec3 at = p.position * m.scale + Vec3{t * m.timescale * 0.1f + phase, 0, 0};
                    p.position = p.position + noise3(at) * m.mask * (speed * scale_[0] * dt);
                    break;
                }
                case Kind::vortex: {
                    Vec3 centre = m.offset3;
                    if (m.controlpoint >= 0 &&
                        static_cast<std::size_t>(m.controlpoint) < control_points_.size()) {
                        centre = centre + control_points_[static_cast<std::size_t>(m.controlpoint)];
                    }
                    Vec3 d = p.position - centre;
                    d[2] = 0;
                    float dist = length(d);
                    float k = m.outer > m.inner
                                  ? std::clamp((dist - m.inner) / (m.outer - m.inner), 0.0f, 1.0f)
                                  : 1.0f;
                    float a = lerp(m.speedinner, m.speedouter, k) * dt * std::numbers::pi_v<float> /
                              180.0f;  // degrees per second
                    float c = std::cos(a), s = std::sin(a);
                    Vec3 r{d[0] * c - d[1] * s, d[0] * s + d[1] * c, 0};
                    p.position = p.position + (r - d);
                    break;
                }
                case Kind::attract: {
                    Vec3 target = m.origin;
                    if (static_cast<std::size_t>(m.controlpoint) < control_points_.size()) {
                        target = target + control_points_[static_cast<std::size_t>(m.controlpoint)];
                    }
                    float scale = m.scale * scale_[0];
                    float threshold = std::max(1.0f, m.threshold * scale_[0]);
                    Vec3 d = target - p.position;
                    float dist = length(d);
                    float strength = 0;
                    if (scale >= 0) {
                        strength = dist > threshold ? 1.0f : dist / threshold;
                    } else if (dist < threshold) {
                        strength = 1.0f - dist / threshold;
                    }
                    p.velocity = p.velocity + normalized(d) * (scale * strength * dt);
                    break;
                }
                default: break;
            }
        }
        ++i;
    }
}

void ParticleSystem::emit(Emitter& e, double dt, const Vec3* at) {
    const Module& m = *e.module;
    if (m.duration > 0 && time_ > m.duration) return;
    if (!e.burst_done) {
        e.burst_done = true;
        int burst = static_cast<int>(m.instantaneous * over_.count);
        for (int i = 0; i < burst && particles_.size() < max_; ++i) spawn(m, at);
    }
    float rate = m.rate * over_.rate;
    if (rate <= 0) return;
    e.accumulated += rate * static_cast<float>(dt);
    int n = static_cast<int>(e.accumulated);
    e.accumulated -= static_cast<float>(n);
    for (int i = 0; i < n && particles_.size() < max_; ++i) spawn(m, at);
}

void ParticleSystem::spawn(const Module& emitter, const Vec3* at) {
    using Kind = Module::Kind;
    Particle p{};
    p.seed = hash(static_cast<std::uint32_t>(rng_ >> 32) ^ static_cast<std::uint32_t>(rng_));
    random();
    Vec3 origin = emitter.origin * scale_;
    if (at != nullptr) origin = origin + *at;
    Vec3 dir;
    if (emitter.kind == Kind::box) {
        // boxrandom: distancemax is the half extent per axis, distancemin an inner box left
        // empty.
        Vec3 dm = emitter.box_max * scale_;
        Vec3 dn = emitter.box_min * scale_;
        Vec3 offset{};
        for (int attempt = 0; attempt < 8; ++attempt) {
            offset = {(random() * 2 - 1) * dm[0], (random() * 2 - 1) * dm[1],
                      (random() * 2 - 1) * dm[2]};
            bool inner = (dn[0] > 0 || dn[1] > 0 || dn[2] > 0) && std::abs(offset[0]) < dn[0] &&
                         std::abs(offset[1]) < dn[1] && std::abs(offset[2]) < dn[2];
            if (!inner) break;
        }
        p.position = origin + offset;
        dir = normalized(Vec3{random() * 2 - 1, random() * 2 - 1, random() * 2 - 1} *
                         emitter.directions);
    } else {
        // sphererandom and anything unknown: a direction, a distance along it.
        float dmin = emitter.sphere_min * scale_[0];
        float dmax = emitter.sphere_max * scale_[0];
        dir = normalized(Vec3{random() * 2 - 1, random() * 2 - 1, random() * 2 - 1} *
                         emitter.directions);
        if (dir == Vec3{0, 0, 0}) dir = {1, 0, 0};
        p.position = origin + dir * lerp(dmin, dmax, random());
    }
    float speed = lerp(emitter.speedmin, emitter.speedmax, random());
    p.velocity = dir * (speed * over_.speed * scale_[0]);
    p.color = {1, 1, 1};
    p.alpha = 1;
    p.size = 20;
    p.life = 1;
    p.frame = random();

    auto spread = [&](const Module& m) {
        float r = random();
        if (m.exponent != 1 && m.exponent > 0) r = std::pow(r, m.exponent);
        return lerp(m.min, m.max, r);
    };
    auto spread3 = [&](const Module& m) {
        return Vec3{lerp(m.min3[0], m.max3[0], random()), lerp(m.min3[1], m.max3[1], random()),
                    lerp(m.min3[2], m.max3[2], random())};
    };
    for (const auto& init : initializers_) {
        const Module& m = *init;
        switch (m.kind) {
            case Kind::lifetime: p.life = std::max(0.01f, spread(m) * over_.lifetime); break;
            case Kind::size: p.size = spread(m) * over_.size * scale_[0]; break;
            case Kind::alpha: p.alpha = spread(m); break;
            case Kind::color: {
                float r = random();
                Vec3 mn = m.has_min3 ? m.min3 : Vec3{255, 255, 255};
                Vec3 mx = m.has_max3 ? m.max3 : mn;
                p.color = {lerp(mn[0], mx[0], r) / 255.0f, lerp(mn[1], mx[1], r) / 255.0f,
                           lerp(mn[2], mx[2], r) / 255.0f};
                break;
            }
            case Kind::velocity:
                p.velocity = p.velocity + spread3(m) * (over_.speed * scale_[0]);
                break;
            case Kind::rotation: p.rotation = spread3(m); break;
            case Kind::angular_velocity: p.angular = spread3(m); break;
            case Kind::turbulent_velocity: {
                float sp = lerp(m.speedmin, m.speedmax, random());
                Vec3 field = normalized(
                    noise3(p.position * (m.scale * 0.01f) + Vec3{m.offset, m.offset, m.offset}));
                p.velocity = p.velocity + field * (sp * over_.speed * scale_[0]);
                break;
            }
            default: break;
        }
    }
    p.color = p.color * over_.color * over_.brightness;
    p.alpha *= over_.alpha;
    particles_.push_back(p);
    event("eventspawn", particles_.back());
}

void ParticleSystem::event(const std::string& type, const Particle& particle) {
    for (std::size_t i = 0; i < children_.size(); ++i) {
        const ParticleDesc::Child& c = desc_->children[i];
        if (c.type != type) continue;
        if (c.probability < 1 && random() > c.probability) continue;
        ParticleSystem& child = *children_[i];
        for (Emitter& e : child.emitters_) {
            int n = std::max(1, static_cast<int>(e.module->instantaneous));
            for (int k = 0; k < n && child.particles_.size() < child.max_; ++k) {
                child.spawn(*e.module, &particle.position);
            }
        }
    }
}

void ParticleSystem::follow(double dt) {
    // eventfollow children emit at every live particle of the parent, at their rate.
    for (std::size_t i = 0; i < children_.size(); ++i) {
        if (desc_->children[i].type != "eventfollow") continue;
        ParticleSystem& child = *children_[i];
        for (Emitter& e : child.emitters_) {
            float rate = e.module->rate * child.over_.rate;
            if (rate <= 0) continue;
            for (const Particle& p : particles_) {
                e.accumulated += rate * static_cast<float>(dt);
                int n = static_cast<int>(e.accumulated);
                e.accumulated -= static_cast<float>(n);
                for (int k = 0; k < n && child.particles_.size() < child.max_; ++k) {
                    child.spawn(*e.module, &p.position);
                }
            }
        }
    }
}

std::uint32_t ParticleSystem::write(ParticleVertex* out, std::uint32_t capacity,
                                    float texture_ratio) const {
    using Kind = Module::Kind;
    std::uint32_t count = 0;
    bool trail = desc_->sprite_trail() || rope_length_ > 0;
    constexpr float two_pi = 2 * std::numbers::pi_v<float>;
    for (const Particle& p : particles_) {
        if (count >= capacity) break;
        float life = p.age / p.life;
        float alpha = p.alpha;
        float size = p.size;
        Vec3 color = p.color;
        Vec3 position = p.position;
        for (const auto& op : operators_) {
            const Module& m = *op;
            switch (m.kind) {
                case Kind::alpha_fade:
                    if (m.fadein > 0) alpha *= std::min(1.0f, life / m.fadein);
                    if (m.fadeout > 0) alpha *= std::min(1.0f, (1 - life) / m.fadeout);
                    break;
                case Kind::size_change:
                case Kind::alpha_change:
                case Kind::color_change: {
                    float k = m.endtime > m.starttime
                                  ? std::clamp((life - m.starttime) / (m.endtime - m.starttime),
                                               0.0f, 1.0f)
                                  : 1.0f;
                    if (m.kind == Kind::size_change) {
                        size *= lerp(m.startvalue, m.endvalue, k);
                    } else if (m.kind == Kind::alpha_change) {
                        alpha *= lerp(m.startvalue, m.endvalue, k);
                    } else {
                        color = color * Vec3{lerp(m.start3[0], m.end3[0], k),
                                             lerp(m.start3[1], m.end3[1], k),
                                             lerp(m.start3[2], m.end3[2], k)};
                    }
                    break;
                }
                case Kind::oscillate_alpha:
                case Kind::oscillate_size: {
                    float f = lerp(m.frequencymin, m.frequencymax, hashed(p.seed, 1));
                    float phase = lerp(m.phasemin, m.phasemax, hashed(p.seed, 2));
                    float w = std::sin((p.age * f + phase) * two_pi) * 0.5f + 0.5f;
                    float v = lerp(m.scalemin, m.scalemax, w);
                    if (m.kind == Kind::oscillate_alpha) {
                        alpha *= v;
                    } else {
                        size *= v;
                    }
                    break;
                }
                case Kind::oscillate_position: {
                    float f = lerp(m.frequencymin, m.frequencymax, hashed(p.seed, 3));
                    float phase = lerp(m.phasemin, m.phasemax, hashed(p.seed, 4));
                    float amplitude = lerp(m.scalemin, m.scalemax, hashed(p.seed, 5)) * scale_[0];
                    float a = (p.age * f + phase) * two_pi;
                    Vec3 offset{std::sin(a), std::cos(a + hashed(p.seed, 6) * two_pi),
                                std::sin(a * 0.7f)};
                    position = position + offset * m.mask * amplitude;
                    break;
                }
                default: break;
            }
        }
        float frame = desc_->animationmode == "randomframe"
                          ? p.frame
                          : std::fmod(life * desc_->sequencemultiplier, 1.0f);
        ParticleVertex v{};
        v.position[0] = position[0];
        v.position[1] = position[1];
        v.position[2] = position[2];
        v.texcoord[2] = p.rotation[2];
        v.texcoord[3] = size;
        v.color[0] = color[0];
        v.color[1] = color[1];
        v.color[2] = color[2];
        v.color[3] = std::clamp(alpha, 0.0f, 1.0f);
        v.rotation[0] = p.rotation[0];
        v.rotation[1] = p.rotation[1];
        Vec3 velocity = p.velocity;
        if (rope_length_ > 0) {
            // A rope trails its velocity times the length; the sprite-trail shader makes
            // the quad size times the velocity it is given, times the texture ratio.
            float k = rope_length_ / std::max(size * std::max(texture_ratio, 1e-3f), 1e-3f);
            velocity = velocity * k;
        }
        if (trail && length(velocity) < 1e-3f) velocity[1] = 1e-3f;
        v.velocity[0] = velocity[0];
        v.velocity[1] = velocity[1];
        v.velocity[2] = velocity[2];
        v.velocity[3] = frame;
        // Top-left, top-right, bottom-left, bottom-right in uv.
        const float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        for (int k = 0; k < 4; ++k) {
            v.texcoord[0] = uv[k][0];
            v.texcoord[1] = uv[k][1];
            out[count * 4 + static_cast<std::uint32_t>(k)] = v;
        }
        ++count;
    }
    return count;
}

}  // namespace glint::scene

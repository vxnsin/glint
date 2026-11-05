#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "wallpaper/scene/scene.hpp"

// Wallpaper Engine particle systems (docs/FORMATS.md, 9): the description read from a
// particles/x.json, and a simulation of it on the CPU that writes one quad per particle for
// the genericparticle shader. Positions are in the object's space, canvas units, y up.
namespace glint::scene {

class Vfs;

// One emitter, initializer, operator or renderer: its name and its numbers, as the json
// gives them. The simulation reads what it understands and ignores the rest.
struct ParticleModule {
    std::string name;
    nlohmann::json json;
};

struct ParticleControlPoint {
    Vec3 offset{0, 0, 0};
    bool pointer = false;  // follows the cursor (flags bit 0 or locktopointer)
};

struct ParticleDesc {
    std::string file;
    Material material;
    int maxcount = 100;
    float starttime = 0;  // seconds simulated before the first frame
    unsigned flags = 0;
    std::string animationmode;  // "", "sequence" or "randomframe"
    float sequencemultiplier = 1;
    std::vector<ParticleModule> emitters, initializers, operators, renderers;
    std::vector<ParticleControlPoint> control_points;

    struct Child {
        std::string type;  // "" (a system of its own at the same origin), eventfollow,
                           // eventspawn, eventdeath
        Vec3 scale{1, 1, 1};
        float probability = 1;
        int maxcount = 0;  // 0: the child's own
        std::shared_ptr<ParticleDesc> desc;
    };
    std::vector<Child> children;

    bool sprite_trail() const;
    bool rope() const;  // rope or ropetrail: drawn as sprite trails for now
};

// Reads the json (and its children, which may be files or inline). Null on failure.
std::shared_ptr<ParticleDesc> parse_particle(const Vfs& vfs, const std::string& path,
                                             std::vector<std::string>& warnings);

// A particle object's instanceoverride.
struct ParticleOverride {
    float rate = 1, count = 1, size = 1, speed = 1, lifetime = 1, alpha = 1, brightness = 1;
    Vec3 color{1, 1, 1};
    bool operator==(const ParticleOverride&) const = default;
};

// An object's instanceoverride with the property values applied (a value may be bound to a
// property, {"user": "bokeh", "value": 0.2}, or written by a script).
ParticleOverride resolve_override(const std::map<std::string, Bound>& overrides,
                                  const Values& values);
// Whether any override is a timeline or a script, so it is read every frame.
bool live_override(const std::map<std::string, Bound>& overrides);

// Whether the system or any child has a control point that follows the cursor.
bool follows_pointer(const ParticleDesc& desc);

// The per-frame inputs a system needs from the scene.
struct ParticleEnv {
    double delta = 0;
    double time = 0;
    Vec3 pointer{0, 0, 0};  // in the object's space
};

// A vertex of the genericparticle layout, THICKFORMAT: position, (uv, rotation z, size),
// colour, (rotation x, y), (velocity, lifetime fraction).
struct ParticleVertex {
    float position[3];
    float texcoord[4];
    float color[4];
    float rotation[2];
    float velocity[4];
};
static_assert(sizeof(ParticleVertex) == 17 * sizeof(float));

class ParticleSystem {
public:
    // event_child: fed by a parent's events, no emission of its own.
    ParticleSystem(std::shared_ptr<const ParticleDesc> desc, const ParticleOverride& over,
                   std::uint32_t seed, Vec3 scale = {1, 1, 1}, bool event_child = false);
    ~ParticleSystem();

    // Advances the system and its children; starttime is simulated the first time.
    void step(const ParticleEnv& env);
    // Writes the quads of this system (not the children) and returns how many.
    // texture_ratio is the sprite's height over its width, which the shader multiplies the
    // quad height by; a rope trail's length is corrected for it.
    std::uint32_t write(ParticleVertex* out, std::uint32_t capacity, float texture_ratio) const;
    // Quads at most: one per particle, or the ribbon's segments for a rope trail.
    std::uint32_t capacity() const {
        return static_cast<std::uint32_t>(max_) * (rope_samples_ > 1 ? rope_samples_ - 1 : 1);
    }
    const ParticleDesc& desc() const { return *desc_; }
    const ParticleOverride& override_values() const { return over_; }
    // New override values for the running system (rate, size, speed, lifetime, alpha,
    // colour); a changed count needs a new system, it is left as it was.
    void set_override(const ParticleOverride& over);

    // The children are drawn as systems of their own with their materials.
    std::vector<std::unique_ptr<ParticleSystem>>& children() { return children_; }

private:
    struct Particle {
        Vec3 position, velocity, rotation, angular;
        Vec3 color;
        float alpha, size, life, age;
        float frame;         // 0 to 1, the sheet frame when animationmode is randomframe
        std::uint32_t seed;  // per-particle randoms for the oscillators
    };
    struct Module;  // an emitter, initializer or operator with its numbers read once
    static std::unique_ptr<Module> compile(const ParticleModule& m);
    struct Emitter {
        const Module* module;
        float accumulated = 0;
        bool burst_done = false;
    };
    void simulate(double dt, const ParticleEnv& env);
    void emit(Emitter& e, double dt, const Vec3* at);
    void spawn(const Module& emitter, const Vec3* at);
    void event(const std::string& type, const Particle& particle);
    void follow(double dt);
    float random();  // 0 to 1

    std::shared_ptr<const ParticleDesc> desc_;
    ParticleOverride over_;
    Vec3 scale_;
    std::size_t max_;
    std::uint64_t rng_;
    std::vector<Particle> particles_;
    std::vector<std::unique_ptr<Module>> emitter_modules_, initializers_, operators_;
    std::vector<Emitter> emitters_;
    std::vector<std::unique_ptr<ParticleSystem>> children_;
    std::vector<Vec3> control_points_;
    // A rope trail: every particle keeps the last rope_samples_ of its positions, one every
    // rope_interval_ seconds, and is drawn as a ribbon through them as wide as its size.
    // Parallel to particles_, so a removal swaps both.
    float rope_length_ = 0;  // seconds of path the ribbon covers; 0 for sprites
    std::uint32_t rope_samples_ = 0;
    float rope_interval_ = 0;
    std::vector<Vec3> history_;
    std::vector<std::uint32_t> history_count_;
    std::vector<float> history_since_;
    void remember(std::size_t index, float dt);
    bool rope_ = false;  // a "rope" renderer: one ribbon through all the particles
    std::uint32_t write_rope_chain(ParticleVertex* out, std::uint32_t capacity) const;
    void write_rope(const Particle& p, std::size_t index, float alpha, float size, Vec3 color,
                    float frame, ParticleVertex* out, std::uint32_t& count,
                    std::uint32_t capacity) const;
    double time_ = 0;
    bool warmed_ = false;
    bool event_child_ = false;
};

}  // namespace glint::scene

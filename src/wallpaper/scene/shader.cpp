#include "wallpaper/scene/shader.hpp"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

#include "wallpaper/scene/vfs.hpp"

namespace glint::scene {

namespace {

// Bump when the rewriting changes, so cached SPIR-V from older rules is not reused.
constexpr const char* rewrite_version = "glint-shader-2";

// HLSL habits of the dialect, as macros the preprocessor applies.
constexpr const char* compat = R"(#define GLSL 1
#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define float2x2 mat2
#define float3x3 mat3
#define float4x4 mat4
#define mul(x, y) ((y) * (x))
#define lerp mix
#define frac fract
#define saturate(x) clamp(x, 0.0, 1.0)
#define texSample2D texture
#define texSample2DLod textureLod
#define atan2 atan
#define fmod(x, y) ((x) - (y) * trunc((x) / (y)))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define log10(x) (log2(x) * 0.30103)
#define rsqrt inversesqrt
#define CAST2(x) (vec2(x))
#define CAST3(x) (vec3(x))
#define CAST4(x) (vec4(x))
#define CAST3X3(x) (mat3(x))
#define max(x, y) max(y, x)
#define sample glint_sample
#define input glint_input
#define output glint_output
#define filter glint_filter
#define common glint_common
#define partition glint_partition
#define active glint_active
#define buffer glint_buffer
#define shared glint_shared
#define patch glint_patch
#define resource glint_resource
)";

std::once_flag glslang_once;

std::string upper(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::toupper(c); });
    return text;
}

std::uint64_t fnv1a(std::string_view text, std::uint64_t hash = 1469598103934665603ull) {
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Inlines #include "x" from shaders/ (the item's, then the assets'), each file once.
bool resolve_includes(const Vfs& vfs, std::string_view text, std::set<std::string>& seen,
                      std::string& out, std::string& error, int depth = 0) {
    if (depth > 16) {
        error = "includes nest too deep";
        return false;
    }
    std::istringstream in{std::string(text)};
    std::string line;
    static const std::regex include(R"(^\s*#include\s+["<]([^">]+)[">])");
    static const std::regex require(R"(^\s*#require\s+(\w+))");
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        // Lighting modules (LightingV1) come with lights in M5. Their functions are only
        // called under #if LIGHTING, which is off for every image glint draws until then.
        if (std::regex_search(line, m, require)) {
            out += "// #require " + m[1].str() + " (lighting, M5)\n";
            continue;
        }
        if (std::regex_search(line, m, include)) {
            std::string name = m[1].str();
            if (!seen.insert(name).second) continue;
            auto file = vfs.read("shaders/" + name);
            if (!file) {
                error = "include not found: " + name;
                return false;
            }
            if (!resolve_includes(vfs, file->text(), seen, out, error, depth + 1)) return false;
            continue;
        }
        out += line;
        out += '\n';
    }
    return true;
}

struct Annotations {
    std::map<std::string, int> combo_defaults;         // upper-case name -> default
    std::map<std::string, nlohmann::json> by_uniform;  // uniform name -> annotation
};

void read_annotations(std::string_view text, Annotations& out) {
    static const std::regex combo(R"(//\s*\[COMBO\]\s*(\{.*\}))");
    static const std::regex uniform(
        R"(^\s*uniform\s+(?:(?:lowp|mediump|highp)\s+)?\w+\s+(\w+)\s*(?:\[[^\]]*\])?\s*;\s*//\s*(\{.*\}))");
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, combo)) {
            auto j = nlohmann::json::parse(m[1].str(), nullptr, false);
            if (j.is_object() && j.contains("combo") && j["combo"].is_string()) {
                int value =
                    j.contains("default") && j["default"].is_number() ? j["default"].get<int>() : 0;
                out.combo_defaults[upper(j["combo"].get<std::string>())] = value;
            }
        } else if (std::regex_search(line, m, uniform)) {
            auto j = nlohmann::json::parse(m[2].str(), nullptr, false);
            if (j.is_object()) out.by_uniform[m[1].str()] = j;
        }
    }
}

const TBuiltInResource* resources() {
    return GetDefaultResources();
}

void setup(glslang::TShader& shader, EShLanguage stage) {
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
}

bool preprocess(EShLanguage stage, const std::string& text, std::string& out, std::string& error) {
    glslang::TShader shader(stage);
    const char* strings[] = {text.c_str()};
    shader.setStrings(strings, 1);
    setup(shader, stage);
    glslang::TShader::ForbidIncluder includer;
    auto messages =
        static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgRelaxedErrors);
    if (!shader.preprocess(resources(), 450, ENoProfile, false, false, messages, &out, includer)) {
        error = shader.getInfoLog();
        return false;
    }
    return true;
}

// std140 base alignment and size of one element.
bool std140(const std::string& type, std::uint32_t& align, std::uint32_t& size) {
    static const std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> table = {
        {"float", {4, 4}},    {"int", {4, 4}},      {"uint", {4, 4}},     {"bool", {4, 4}},
        {"vec2", {8, 8}},     {"ivec2", {8, 8}},    {"uvec2", {8, 8}},    {"bvec2", {8, 8}},
        {"vec3", {16, 12}},   {"ivec3", {16, 12}},  {"uvec3", {16, 12}},  {"bvec3", {16, 12}},
        {"vec4", {16, 16}},   {"ivec4", {16, 16}},  {"uvec4", {16, 16}},  {"bvec4", {16, 16}},
        {"mat2", {16, 32}},   {"mat3", {16, 48}},   {"mat4", {16, 64}},   {"mat2x2", {16, 32}},
        {"mat3x3", {16, 48}}, {"mat4x4", {16, 64}}, {"mat4x3", {16, 64}}, {"mat3x4", {16, 48}},
        {"mat2x3", {16, 32}}, {"mat3x2", {16, 48}}, {"mat2x4", {16, 32}}, {"mat4x2", {16, 64}},
    };
    auto it = table.find(type);
    if (it == table.end()) return false;
    align = it->second.first;
    size = it->second.second;
    return true;
}

std::uint32_t round_up(std::uint32_t v, std::uint32_t a) {
    return (v + a - 1) / a * a;
}

// A global declaration after preprocessing.
struct Declaration {
    std::string storage;  // uniform, attribute, varying, in, out
    std::string type;
    std::string name;
    std::string array;  // "[4]" or empty
};

struct Rewritten {
    std::string body;  // the stage without its uniform/attribute/varying declarations
    std::vector<Declaration> uniforms, samplers, inputs, outputs;
};

// Takes the global declarations out of a preprocessed stage.
void split(const std::string& text, bool vertex, Rewritten& out) {
    static const std::regex decl(
        R"(^\s*(uniform|attribute|varying|in|out)\s+(?:(?:lowp|mediump|highp|flat|smooth|noperspective)\s+)*(\w+)\s+(\w+)\s*(\[[^\]]*\])?\s*;\s*$)");
    std::istringstream in(text);
    std::string line;
    int depth = 0;
    while (std::getline(in, line)) {
        if (line.starts_with("#version") || line.starts_with("#extension") ||
            line.starts_with("#line") || line.starts_with("#pragma")) {
            continue;
        }
        std::smatch m;
        if (depth == 0 && std::regex_match(line, m, decl)) {
            Declaration d{m[1].str(), m[2].str(), m[3].str(), m[4].str()};
            if (d.storage == "uniform") {
                if (d.type.starts_with("sampler")) {
                    out.samplers.push_back(d);
                } else {
                    out.uniforms.push_back(d);
                }
            } else if (d.storage == "attribute" || (vertex && d.storage == "in")) {
                out.inputs.push_back(d);
            } else if (d.storage == "varying") {
                (vertex ? out.outputs : out.inputs).push_back(d);
            } else if (d.storage == "out") {
                out.outputs.push_back(d);
            } else {
                out.inputs.push_back(d);
            }
            continue;
        }
        for (char c : line) {
            if (c == '{') ++depth;
            if (c == '}') --depth;
        }
        out.body += line;
        out.body += '\n';
    }
}

std::uint32_t locations_of(const std::string& type) {
    if (type == "mat3" || type == "mat4x3" || type == "mat3x3") return type == "mat4x3" ? 4 : 3;
    if (type == "mat4" || type == "mat4x4") return 4;
    if (type == "mat2") return 2;
    return 1;
}

int texture_slot(const std::string& name) {
    static const std::regex slot(R"(^g_Texture(\d+)$)");
    std::smatch m;
    if (std::regex_match(name, m, slot)) return std::stoi(m[1].str());
    return -1;
}

bool compile_stage(EShLanguage stage, const std::string& text, glslang::TProgram& program,
                   std::vector<std::unique_ptr<glslang::TShader>>& keep, std::string& error) {
    auto shader = std::make_unique<glslang::TShader>(stage);
    const char* strings[] = {text.c_str()};
    shader->setStrings(strings, 1);
    setup(*shader, stage);
    auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader->parse(resources(), 450, false, messages)) {
        error =
            std::string(stage == EShLangVertex ? "vertex: " : "fragment: ") + shader->getInfoLog();
        return false;
    }
    program.addShader(shader.get());
    keep.push_back(std::move(shader));
    return true;
}

}  // namespace

const ShaderUniform* CompiledShader::uniform(std::string_view name) const {
    for (const auto& u : uniforms) {
        if (u.name == name) return &u;
    }
    return nullptr;
}

ShaderCompiler::ShaderCompiler(const Vfs& vfs, std::filesystem::path cache)
    : vfs_(vfs), cache_(std::move(cache)) {
    std::call_once(glslang_once, [] { glslang::InitializeProcess(); });
}

ShaderCompiler::~ShaderCompiler() = default;

CompiledShader ShaderCompiler::compile(const ShaderRequest& request) {
    CompiledShader result;
    std::string sources[2];
    const char* extensions[2] = {".vert", ".frag"};
    Annotations annotations;
    for (int i = 0; i < 2; ++i) {
        std::string path = "shaders/" + request.name + extensions[i];
        auto file = vfs_.read(path);
        if (!file) {
            result.error = path + " not found";
            return result;
        }
        std::set<std::string> seen;
        if (!resolve_includes(vfs_, file->text(), seen, sources[i], result.error)) {
            return result;
        }
        read_annotations(sources[i], annotations);
    }

    // Combos: the shader's defaults, then the material's, then one per bound texture slot
    // whose sampler names a combo.
    result.combos = annotations.combo_defaults;
    for (const auto& [name, value] : request.combos) result.combos[upper(name)] = value;
    for (const auto& [name, annotation] : annotations.by_uniform) {
        int slot = texture_slot(name);
        if (slot < 0 || !annotation.contains("combo") || !annotation["combo"].is_string()) {
            continue;
        }
        if ((request.bound_textures & (1u << slot)) != 0) {
            result.combos[upper(annotation["combo"].get<std::string>())] = 1;
        }
    }

    std::string defines;
    for (const auto& [name, value] : result.combos) {
        defines += "#define " + name + " " + std::to_string(value) + "\n";
    }

    Rewritten stages[2];
    for (int i = 0; i < 2; ++i) {
        bool vertex = i == 0;
        std::string text = "#version 450\n" + std::string(compat) + defines +
                           (vertex ? "" : "#define gl_FragColor glint_FragColor\n") + "#line 1\n" +
                           sources[i];
        std::string expanded;
        if (!preprocess(vertex ? EShLangVertex : EShLangFragment, text, expanded, result.error)) {
            result.error = std::string(vertex ? "vertex preprocess: " : "fragment preprocess: ") +
                           result.error;
            return result;
        }
        split(expanded, vertex, stages[i]);
    }

    // One uniform block shared by both stages, std140, in first-seen order.
    std::string block;
    std::uint32_t offset = 0;
    std::set<std::string> declared;
    for (const auto& stage : stages) {
        for (const auto& d : stage.uniforms) {
            if (!declared.insert(d.name).second) continue;
            std::uint32_t align = 0, size = 0;
            if (!std140(d.type, align, size)) {
                result.error = "uniform " + d.name + " has a type glint cannot place: " + d.type;
                return result;
            }
            std::uint32_t count = 1;
            if (!d.array.empty()) {
                std::string n = d.array.substr(1, d.array.size() - 2);
                count = static_cast<std::uint32_t>(std::max(1, std::atoi(n.c_str())));
                align = 16;
                size = round_up(size, 16) * count;
            }
            offset = round_up(offset, align);
            ShaderUniform u{d.name, d.type, count, offset, size, {}, {}};
            if (auto it = annotations.by_uniform.find(d.name); it != annotations.by_uniform.end()) {
                const auto& a = it->second;
                if (a.contains("material") && a["material"].is_string()) {
                    u.material = a["material"].get<std::string>();
                }
                if (a.contains("default")) u.fallback = a["default"];
            }
            result.uniforms.push_back(u);
            offset += size;
            block += "    " + d.type + " " + d.name + d.array + ";\n";
        }
    }
    result.block_size = round_up(offset, 16);
    std::string header = "#version 450\n";
    if (!block.empty()) {
        header += "layout(std140, set = 0, binding = 0) uniform GlintUniforms {\n" + block + "};\n";
    }

    // Samplers: g_TextureN at binding N + 1, others after the slots.
    std::uint32_t extra = texture_binding_base + max_texture_slots;
    std::map<std::string, std::uint32_t> sampler_bindings;
    for (const auto& stage : stages) {
        for (const auto& d : stage.samplers) {
            if (sampler_bindings.contains(d.name)) continue;
            int slot = texture_slot(d.name);
            std::uint32_t binding =
                slot >= 0 && static_cast<std::uint32_t>(slot) < max_texture_slots
                    ? texture_binding_base + static_cast<std::uint32_t>(slot)
                    : extra++;
            sampler_bindings[d.name] = binding;
            ShaderSampler s{d.name, slot, binding, {}};
            if (auto it = annotations.by_uniform.find(d.name); it != annotations.by_uniform.end()) {
                if (it->second.contains("default") && it->second["default"].is_string()) {
                    s.fallback = it->second["default"].get<std::string>();
                }
            }
            result.samplers.push_back(s);
        }
    }
    auto sampler_text = [&](const Rewritten& stage) {
        std::string text;
        for (const auto& d : stage.samplers) {
            text += "layout(set = 0, binding = " + std::to_string(sampler_bindings[d.name]) +
                    ") uniform " + d.type + " " + d.name + d.array + ";\n";
        }
        return text;
    };

    // Varyings: one location map by name for both stages.
    std::map<std::string, std::uint32_t> varying_locations;
    std::uint32_t next = 0;
    for (const auto* list : {&stages[0].outputs, &stages[1].inputs}) {
        for (const auto& d : *list) {
            if (varying_locations.contains(d.name)) continue;
            varying_locations[d.name] = next;
            next += locations_of(d.type);
        }
    }
    // Direct3D lets the stages disagree on a varying's width; Vulkan does not. The fragment
    // stage then reads the vertex stage's type under another name and converts at the top of
    // main: truncated, or padded with zeros.
    std::map<std::string, std::string> vertex_types;
    for (const auto& d : stages[0].outputs) vertex_types[d.name] = d.type;
    std::string vertex_io, fragment_io, fragment_globals, fragment_prologue;
    std::uint32_t attribute = 0;
    for (const auto& d : stages[0].inputs) {
        result.attributes[d.name] = attribute;
        vertex_io += "layout(location = " + std::to_string(attribute) + ") in " + d.type + " " +
                     d.name + d.array + ";\n";
        attribute += locations_of(d.type);
    }
    for (const auto& d : stages[0].outputs) {
        vertex_io += "layout(location = " + std::to_string(varying_locations[d.name]) + ") out " +
                     d.type + " " + d.name + d.array + ";\n";
    }
    for (const auto& d : stages[1].inputs) {
        std::string location = std::to_string(varying_locations[d.name]);
        auto vt = vertex_types.find(d.name);
        if (vt == vertex_types.end() || vt->second == d.type || !d.array.empty()) {
            fragment_io +=
                "layout(location = " + location + ") in " + d.type + " " + d.name + d.array + ";\n";
            continue;
        }
        auto width = [](const std::string& t) {
            if (t == "float") return 1;
            if (t.size() == 4 && t.starts_with("vec")) return t[3] - '0';
            return 0;
        };
        int from = width(vt->second), to = width(d.type);
        if (from == 0 || to == 0) {
            result.error = "varying " + d.name + " is " + vt->second + " in the vertex stage and " +
                           d.type + " in the fragment stage";
            return result;
        }
        std::string in_name = "glint_in_" + d.name;
        fragment_io +=
            "layout(location = " + location + ") in " + vt->second + " " + in_name + ";\n";
        fragment_globals += d.type + " " + d.name + ";\n";
        std::string value;
        if (to < from) {
            value = in_name + "." + std::string("xyzw").substr(0, static_cast<std::size_t>(to));
        } else {
            value = d.type + "(" + in_name;
            for (int k = from; k < to; ++k) value += ", 0.0";
            value += ")";
        }
        fragment_prologue += "    " + d.name + " = " + value + ";\n";
    }
    if (!fragment_prologue.empty()) {
        static const std::regex main_open(R"(void\s+main\s*\(\s*(void)?\s*\)\s*\{)");
        std::smatch m;
        if (!std::regex_search(stages[1].body, m, main_open)) {
            result.error = "fragment stage has no main()";
            return result;
        }
        auto at = static_cast<std::size_t>(m.position(0) + m.length(0));
        stages[1].body.insert(at, "\n" + fragment_prologue);
    }
    fragment_io += fragment_globals;
    fragment_io += "layout(location = 0) out vec4 glint_FragColor;\n";
    for (const auto& d : stages[1].outputs) {
        if (d.name == "glint_FragColor") continue;
        fragment_io += "layout(location = 0) out " + d.type + " " + d.name + ";\n";
    }

    last_vertex_ = header + sampler_text(stages[0]) + vertex_io + stages[0].body;
    last_fragment_ = header + sampler_text(stages[1]) + fragment_io + stages[1].body;

    std::uint64_t hash = fnv1a(rewrite_version);
    hash = fnv1a(last_vertex_, hash);
    hash = fnv1a("\n--fragment--\n", hash);
    hash = fnv1a(last_fragment_, hash);
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    result.hash = hex;

    // The cache: [u32 vertex words][vertex][u32 fragment words][fragment].
    std::filesystem::path cached = cache_.empty() ? cache_ : cache_ / (result.hash + ".spv");
    if (!cached.empty()) {
        std::ifstream in(cached, std::ios::binary);
        std::uint32_t n = 0;
        if (in && in.read(reinterpret_cast<char*>(&n), 4) && n < (1u << 24)) {
            result.vertex.resize(n);
            in.read(reinterpret_cast<char*>(result.vertex.data()), n * 4);
            if (in.read(reinterpret_cast<char*>(&n), 4) && n < (1u << 24)) {
                result.fragment.resize(n);
                in.read(reinterpret_cast<char*>(result.fragment.data()), n * 4);
                if (in && !result.vertex.empty() && !result.fragment.empty()) {
                    result.cached = true;
                    return result;
                }
            }
        }
        result.vertex.clear();
        result.fragment.clear();
    }

    glslang::TProgram program;
    std::vector<std::unique_ptr<glslang::TShader>> keep;
    if (!compile_stage(EShLangVertex, last_vertex_, program, keep, result.error) ||
        !compile_stage(EShLangFragment, last_fragment_, program, keep, result.error)) {
        return result;
    }
    if (!program.link(static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules))) {
        result.error = std::string("link: ") + program.getInfoLog();
        return result;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = false;
    options.optimizeSize = false;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), result.vertex, &options);
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), result.fragment, &options);

    if (!cached.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_, ec);
        auto tmp = cached;
        tmp += ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        auto n = static_cast<std::uint32_t>(result.vertex.size());
        out.write(reinterpret_cast<const char*>(&n), 4);
        out.write(reinterpret_cast<const char*>(result.vertex.data()), n * 4);
        n = static_cast<std::uint32_t>(result.fragment.size());
        out.write(reinterpret_cast<const char*>(&n), 4);
        out.write(reinterpret_cast<const char*>(result.fragment.data()), n * 4);
        out.close();
        if (out) std::filesystem::rename(tmp, cached, ec);
    }
    return result;
}

}  // namespace glint::scene

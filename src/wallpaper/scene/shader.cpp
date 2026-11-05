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
constexpr const char* rewrite_version = "glint-shader-6";

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
#define texSample2D glint_tex
#define texSample2DLod glint_texlod
#define atan2 atan
#define fmod(x, y) ((x) - (y) * trunc((x) / (y)))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define log10 glint_log10
#define rsqrt inversesqrt
#define CAST2(x) (vec2(x))
#define CAST3(x) (vec3(x))
#define CAST4(x) (vec4(x))
#define CAST3X3(x) (mat3(x))
#define max glint_max
#define mix glint_mix
#define min glint_min
#define pow glint_pow
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

// HLSL's max, min and pow take a scalar on either side of a vector; GLSL's only take it on the
// right (max, min) or not at all (pow). The dialect's calls go to these, by macro.
std::string overload_helpers() {
    std::string out;
    const char* types[] = {"vec2", "vec3", "vec4"};
    for (const char* f : {"max", "min"}) {
        out += std::string("float glint_") + f + "(float a, float b) { return " + f + "(a, b); }\n";
        out += std::string("int glint_") + f + "(int a, int b) { return " + f + "(a, b); }\n";
        for (const char* t : types) {
            std::string T = t;
            out += T + " glint_" + f + "(" + T + " a, " + T + " b) { return " + f + "(a, b); }\n";
            out += T + " glint_" + f + "(" + T + " a, float b) { return " + f + "(a, b); }\n";
            out += T + " glint_" + f + "(float a, " + T + " b) { return " + f + "(b, a); }\n";
        }
    }
    for (const char* f : {"max", "min"}) {
        // int against float, as HLSL allows.
        out += std::string("float glint_") + f + "(float a, int b) { return " + f +
               "(a, float(b)); }\n";
        out += std::string("float glint_") + f + "(int a, float b) { return " + f +
               "(float(a), b); }\n";
        for (const char* t : types) {
            std::string T = t;
            out += T + " glint_" + f + "(" + T + " a, int b) { return " + f + "(a, float(b)); }\n";
        }
    }
    out += "float glint_pow(float a, float b) { return pow(a, b); }\n";
    for (const char* t : types) {
        std::string T = t;
        out += T + " glint_pow(" + T + " a, " + T + " b) { return pow(a, b); }\n";
        out += T + " glint_pow(" + T + " a, float b) { return pow(a, " + T + "(b)); }\n";
        out += T + " glint_pow(float a, " + T + " b) { return pow(" + T + "(a), b); }\n";
    }
    // mix with a scalar on one side, which HLSL's lerp allows.
    out += "float glint_mix(float a, float b, float t) { return mix(a, b, t); }\n";
    for (const char* t : types) {
        std::string T = t;
        out += T + " glint_mix(" + T + " a, " + T + " b, float t) { return mix(a, b, t); }\n";
        out += T + " glint_mix(" + T + " a, " + T + " b, " + T + " t) { return mix(a, b, t); }\n";
        out +=
            T + " glint_mix(" + T + " a, float b, float t) { return mix(a, " + T + "(b), t); }\n";
        out +=
            T + " glint_mix(float a, " + T + " b, float t) { return mix(" + T + "(a), b, t); }\n";
    }
    // texSample2D with a float3 or float4 coordinate: HLSL truncates to the first two.
    out += "vec4 glint_tex(sampler2D s, vec2 uv) { return texture(s, uv); }\n";
    out += "vec4 glint_tex(sampler2D s, vec3 uv) { return texture(s, uv.xy); }\n";
    out += "vec4 glint_tex(sampler2D s, vec4 uv) { return texture(s, uv.xy); }\n";
    out += "vec4 glint_texlod(sampler2D s, vec2 uv, float l) { return textureLod(s, uv, l); }\n";
    out += "vec4 glint_texlod(sampler2D s, vec3 uv, float l) { return textureLod(s, uv.xy, l); }\n";
    out += "vec4 glint_texlod(sampler2D s, vec4 uv, float l) { return textureLod(s, uv.xy, l); }\n";
    // lerp between vectors of different widths: the result has the shorter's.
    out += "vec3 glint_mix(vec4 a, vec3 b, float t) { return mix(a.xyz, b, t); }\n";
    out += "vec3 glint_mix(vec3 a, vec4 b, float t) { return mix(a, b.xyz, t); }\n";
    out += "vec3 glint_mix(vec4 a, vec3 b, vec3 t) { return mix(a.xyz, b, t); }\n";
    out += "vec3 glint_mix(vec3 a, vec4 b, vec3 t) { return mix(a, b.xyz, t); }\n";
    out += "vec2 glint_mix(vec4 a, vec2 b, float t) { return mix(a.xy, b, t); }\n";
    out += "vec2 glint_mix(vec2 a, vec4 b, float t) { return mix(a, b.xy, t); }\n";
    out += "vec2 glint_mix(vec3 a, vec2 b, float t) { return mix(a.xy, b, t); }\n";
    out += "vec2 glint_mix(vec2 a, vec3 b, float t) { return mix(a, b.xy, t); }\n";
    // % on floats: HLSL's fmod.
    out += "int glint_mod(int a, int b) { return a % b; }\n";
    out += "uint glint_mod(uint a, uint b) { return a % b; }\n";
    out += "uint glint_mod(uint a, int b) { return a % uint(b); }\n";
    out += "float glint_mod(float a, float b) { return a - b * trunc(a / b); }\n";
    out += "float glint_mod(float a, int b) { return glint_mod(a, float(b)); }\n";
    out += "float glint_mod(int a, float b) { return glint_mod(float(a), b); }\n";
    return out;
}

bool convertible(const std::string& type) {
    static const std::set<std::string> types = {
        "float", "int",   "uint",  "bool",  "vec2",  "vec3",  "vec4",  "ivec2",
        "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4"};
    return types.contains(type);
}

// True if the expression has a comma outside parentheses (a declarator list).
bool top_level_comma(std::string_view expr) {
    int depth = 0;
    for (char c : expr) {
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) return true;
    }
    return false;
}

// HLSL converts implicitly where GLSL wants a constructor: a scalar into a vector, a vector
// into a shorter one, bool into float, float into int. glint wraps the right-hand side of
// initialised declarations and plain assignments in a constructor of the target type, which
// does all of these, and the condition of an if in bool(). Types come from a table of the
// stage's globals and, per function, its parameters and locals.
void hlsl_conversions(std::string& body, std::map<std::string, std::string> globals) {
    static const std::regex declaration(
        R"(^(\s*)((?:const\s+)?)(float|int|uint|bool|[biu]?vec[234])\s+(\w+)\s*=\s*(.+);\s*$)");
    static const std::regex assignment(R"(^(\s*)(\w+)\s*=\s*([^=].*);\s*$)");
    static const std::regex compound(R"(^(\s*)(\w+)\s*([-+*/])=\s*(.*);\s*$)");
    // (a < b) * x and x * (a < b): a comparison used as a number.
    static const std::regex compare_times(R"(\(([^()]*?(?:<=|>=|==|!=|<|>)[^()]*?)\)(\s*\*))");
    static const std::regex times_compare(R"((\*\s*)\(([^()]*?(?:<=|>=|==|!=|<|>)[^()]*?)\))");
    // a % b on anything: an overload set sorts out ints and floats.
    static const std::regex modulo(
        R"(((?:[\w.]+\([^()]*\)|[\w.]+|\([^()]*\))(?:\[[^\]]*\])?)\s*%\s*((?:[\w.]+\([^()]*\)|[\w.]+|\([^()]*\))(?:\[[^\]]*\])?))");
    static const std::regex function(R"(^\s*(\w+)\s+(\w+)\s*\(([^)]*)\)\s*\{?\s*$)");
    static const std::regex parameter(R"((?:in|out|inout|const|\s)*\b(\w+)\s+(\w+)\s*$)");
    // float2 a = 0.0, b = 0.0, c = x: every declarator gets its constructor.
    static const std::regex declarators(
        R"(^(\s*)((?:const\s+)?)(float|int|uint|bool|[biu]?vec[234])\s+(.+);\s*$)");
    // a.xy = ...: the swizzle's width is the target type.
    static const std::regex swizzle_assignment(
        R"(^(\s*)(\w+)\.([xyzwrgba]{1,4})\s*=\s*([^=].*);\s*$)");
    // for (int i = f; ...): the initialiser converted like a declaration's.
    static const std::regex for_init(
        R"(^(\s*for\s*\(\s*)(float|int|uint|[biu]?vec[234])\s+(\w+)\s*=\s*([^;]+);(.*)$)");
    static const std::regex uniform_ref(R"(\b[ug]_\w+)");
    std::istringstream in(body);
    std::string out, line;
    std::map<std::string, std::string> locals;
    int depth = 0;

    // The shader's own functions and their parameter types, for calls that pass a longer
    // vector than the parameter (HLSL truncates, GLSL refuses).
    std::map<std::string, std::vector<std::string>> functions;
    {
        std::istringstream scan(body);
        std::string l;
        while (std::getline(scan, l)) {
            std::smatch m;
            if (!std::regex_match(l, m, function)) continue;
            std::vector<std::string> types;
            std::string params = m[3].str();
            std::size_t start = 0;
            while (start <= params.size()) {
                std::size_t end = params.find(',', start);
                if (end == std::string::npos) end = params.size();
                std::string one = params.substr(start, end - start);
                std::smatch p;
                types.push_back(std::regex_search(one, p, parameter) ? p[1].str() : "");
                start = end + 1;
            }
            if (!types.empty() && !params.empty()) functions[m[2].str()] = types;
        }
    }
    auto width = [](const std::string& type) {
        if (type == "vec2") return 2;
        if (type == "vec3") return 3;
        if (type == "vec4") return 4;
        if (type == "float") return 1;
        return 0;
    };
    auto type_of = [&](const std::string& name) {
        if (auto it = locals.find(name); it != locals.end()) return it->second;
        if (auto g = globals.find(name); g != globals.end()) return g->second;
        return std::string();
    };
    // Calls of the shader's functions on this line: an argument that is a bare variable
    // wider than the parameter gets the swizzle that truncates it.
    auto truncate_arguments = [&](std::string& text) {
        for (const auto& [name, types] : functions) {
            std::size_t at = 0;
            while ((at = text.find(name + "(", at)) != std::string::npos) {
                bool word = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) ||
                                         text[at - 1] == '_');
                std::size_t open = at + name.size();
                if (!word) {
                    at = open;
                    continue;
                }
                int level = 0;
                std::size_t close = std::string::npos;
                std::vector<std::size_t> commas;
                for (std::size_t i = open; i < text.size(); ++i) {
                    if (text[i] == '(') ++level;
                    if (text[i] == ')' && --level == 0) {
                        close = i;
                        break;
                    }
                    if (text[i] == ',' && level == 1) commas.push_back(i);
                }
                if (close == std::string::npos) break;
                std::vector<std::pair<std::size_t, std::size_t>> args;
                std::size_t from = open + 1;
                for (std::size_t c : commas) {
                    args.push_back({from, c});
                    from = c + 1;
                }
                args.push_back({from, close});
                std::string rebuilt = text.substr(0, open + 1);
                for (std::size_t i = 0; i < args.size(); ++i) {
                    std::string arg = text.substr(args[i].first, args[i].second - args[i].first);
                    std::string trimmed = arg;
                    while (!trimmed.empty() &&
                           std::isspace(static_cast<unsigned char>(trimmed.front())))
                        trimmed.erase(trimmed.begin());
                    while (!trimmed.empty() &&
                           std::isspace(static_cast<unsigned char>(trimmed.back())))
                        trimmed.pop_back();
                    bool identifier = !trimmed.empty() && std::ranges::all_of(trimmed, [](char c) {
                        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
                    }) && !std::isdigit(static_cast<unsigned char>(trimmed[0]));
                    if (identifier && i < types.size()) {
                        int have = width(type_of(trimmed)), want = width(types[i]);
                        if (want >= 2 && have > want) {
                            arg =
                                " " + trimmed +
                                std::string(".xyzw").substr(0, static_cast<std::size_t>(want) + 1);
                        }
                    }
                    rebuilt += arg;
                    if (i + 1 < args.size()) rebuilt += ",";
                }
                rebuilt += text.substr(close);
                text = std::move(rebuilt);
                at = open;
            }
        }
    };
    // Identifiers of a wider vector than `want`, used in arithmetic at the top level of
    // `text` (not inside a call), get the swizzle HLSL's truncation implies.
    auto truncate_operands = [&](std::string& text, int want) {
        std::string rebuilt;
        int level = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            char c = text[i];
            if (c == '(' || c == '[') {
                ++level;
                rebuilt += c;
                ++i;
                continue;
            }
            if (c == ')' || c == ']') {
                --level;
                rebuilt += c;
                ++i;
                continue;
            }
            bool start = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
            if (!start) {
                rebuilt += c;
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_'))
                ++j;
            std::string token = text.substr(i, j - i);
            bool before_ok = i == 0 || !(text[i - 1] == '.');
            std::size_t k = j;
            while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
            bool call_or_member =
                k < text.size() && (text[k] == '(' || text[k] == '.' || text[k] == '[');
            rebuilt += token;
            if (level == 0 && before_ok && !call_or_member && width(type_of(token)) > want &&
                want >= 2) {
                rebuilt += std::string(".xyzw").substr(0, static_cast<std::size_t>(want) + 1);
            }
            i = j;
        }
        text = std::move(rebuilt);
    };
    // The coordinate of a texture read is two wide, whatever is added to it.
    auto truncate_coordinates = [&](std::string& text) {
        for (const char* fn : {"glint_tex(", "glint_texlod("}) {
            std::size_t at = 0;
            std::string name = fn;
            while ((at = text.find(name, at)) != std::string::npos) {
                std::size_t open = at + name.size() - 1;
                int level = 0;
                std::size_t first_comma = std::string::npos, second = std::string::npos;
                for (std::size_t i = open; i < text.size(); ++i) {
                    if (text[i] == '(') ++level;
                    if (text[i] == ')' && --level == 0) {
                        second = i;
                        break;
                    }
                    if (text[i] == ',' && level == 1) {
                        if (first_comma == std::string::npos) {
                            first_comma = i;
                        } else {
                            second = i;
                            break;
                        }
                    }
                }
                if (first_comma == std::string::npos || second == std::string::npos) break;
                std::string coord = text.substr(first_comma + 1, second - first_comma - 1);
                truncate_operands(coord, 2);
                text = text.substr(0, first_comma + 1) + coord + text.substr(second);
                at = open + 1;
            }
        }
    };
    // x ? a : b with a number or a plain variable for x: HLSL's truthiness.
    auto bool_ternaries = [&](std::string& text) {
        for (std::size_t q = text.find('?'); q != std::string::npos; q = text.find('?', q + 1)) {
            std::size_t end = q;
            while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
            std::size_t start = end;
            while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) ||
                                 text[start - 1] == '_' || text[start - 1] == '.')) {
                --start;
            }
            if (start == end) continue;
            if (start > 0 && (text[start - 1] == ')' || text[start - 1] == ']')) continue;
            // Only when the token is the whole condition: what precedes it must open it.
            std::size_t before = start;
            while (before > 0 && std::isspace(static_cast<unsigned char>(text[before - 1])))
                --before;
            if (before > 0) {
                char c = text[before - 1];
                bool opens = c == '(' || c == ',' || c == '?' || c == ':' || c == '{' || c == ';';
                if (c == '=' && before > 1) {
                    char d = text[before - 2];
                    opens = d != '=' && d != '!' && d != '<' && d != '>';
                }
                if (!opens && std::isalpha(static_cast<unsigned char>(c))) {
                    opens = before >= 6 && text.compare(before - 6, 6, "return") == 0;
                }
                if (!opens) continue;
            }
            std::string token = text.substr(start, end - start);
            if (token == "true" || token == "false") continue;
            text.insert(end, ")");
            text.insert(start, "bool(");
            q += 6;
        }
    };

    while (std::getline(in, line)) {
        std::smatch m;
        if (depth == 0 && std::regex_match(line, m, function)) {
            locals.clear();
            std::string params = m[3].str();
            std::size_t start = 0;
            while (start <= params.size()) {
                std::size_t end = params.find(',', start);
                if (end == std::string::npos) end = params.size();
                std::string one = params.substr(start, end - start);
                std::smatch p;
                if (std::regex_search(one, p, parameter)) locals[p[2].str()] = p[1].str();
                start = end + 1;
            }
        } else if (std::regex_match(line, m, declaration) && !top_level_comma(m[5].str())) {
            std::string type = m[3].str();
            (depth == 0 ? globals : locals)[m[4].str()] = type;
            std::string qualifier = m[2].str();
            // A global const from a uniform is no constant in GLSL.
            if (depth == 0 && !qualifier.empty() && std::regex_search(m[5].str(), uniform_ref)) {
                qualifier.clear();
            }
            std::string init = m[5].str();
            if (depth > 0 && (type == "vec2" || type == "vec3"))
                truncate_operands(init, width(type));
            line =
                m[1].str() + qualifier + type + " " + m[4].str() + " = " + type + "(" + init + ");";
        } else if (std::regex_match(line, m, declarators) && top_level_comma(m[4].str())) {
            std::string type = m[3].str();
            std::string list = m[4].str(), rebuilt;
            int level = 0;
            std::size_t start = 0;
            auto one = [&](std::string item) {
                while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
                    item.erase(item.begin());
                std::size_t eq = item.find('=');
                std::string name = item.substr(0, eq);
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
                    name.pop_back();
                (depth == 0 ? globals : locals)[name] = type;
                if (eq == std::string::npos) return item;
                return name + " = " + type + "(" + item.substr(eq + 1) + ")";
            };
            for (std::size_t i = 0; i <= list.size(); ++i) {
                if (i < list.size() && (list[i] == '(' || list[i] == '[')) ++level;
                if (i < list.size() && (list[i] == ')' || list[i] == ']')) --level;
                if (i == list.size() || (list[i] == ',' && level == 0)) {
                    if (!rebuilt.empty()) rebuilt += ", ";
                    rebuilt += one(list.substr(start, i - start));
                    start = i + 1;
                }
            }
            line = m[1].str() + m[2].str() + type + " " + rebuilt + ";";
        } else if (depth > 0 && std::regex_match(line, m, for_init)) {
            std::string type = m[2].str();
            locals[m[3].str()] = type;
            line = m[1].str() + type + " " + m[3].str() + " = " + type + "(" + m[4].str() + ");" +
                   m[5].str();
        } else if (depth > 0 && std::regex_match(line, m, swizzle_assignment)) {
            std::string base = type_of(m[2].str());
            std::size_t n = m[3].str().size();
            if (base == "vec2" || base == "vec3" || base == "vec4") {
                const char* targets[] = {"float", "vec2", "vec3", "vec4"};
                line = m[1].str() + m[2].str() + "." + m[3].str() + " = " + targets[n - 1] + "(" +
                       m[4].str() + ");";
            }
        } else if (depth > 0 && std::regex_match(line, m, assignment)) {
            std::string name = m[2].str();
            auto it = locals.find(name);
            std::string type = it != locals.end() ? it->second : "";
            if (type.empty()) {
                if (auto g = globals.find(name); g != globals.end()) type = g->second;
            }
            if (convertible(type) && !top_level_comma(m[3].str())) {
                std::string value = m[3].str();
                if (type == "vec2" || type == "vec3") truncate_operands(value, width(type));
                line = m[1].str() + name + " = " + type + "(" + value + ");";
            }
        } else if (depth > 0 && std::regex_match(line, m, compound)) {
            // x /= a vec4 with a vec2 x: HLSL truncates, GLSL wants the constructor.
            std::string name = m[2].str();
            auto it = locals.find(name);
            std::string type = it != locals.end() ? it->second : "";
            if (type.empty()) {
                if (auto g = globals.find(name); g != globals.end()) type = g->second;
            }
            if (convertible(type) && type != "float" && type != "int" && type != "uint" &&
                !top_level_comma(m[4].str())) {
                line = m[1].str() + name + " " + m[3].str() + "= " + type + "(" + m[4].str() + ");";
            }
        }
        if (line.find('%') != std::string::npos && line.find("%=") == std::string::npos &&
            line.find('#') == std::string::npos) {
            line = std::regex_replace(line, modulo, "glint_mod($1, $2)");
        }
        if (line.find('*') != std::string::npos) {
            line = std::regex_replace(line, compare_times, "float($1)$2");
            line = std::regex_replace(line, times_compare, "$1float($2)");
        }
        if (!functions.empty() && line.find('(') != std::string::npos && depth > 0) {
            truncate_arguments(line);
        }
        if (depth > 0 && line.find("glint_tex") != std::string::npos) truncate_coordinates(line);
        if (line.find('?') != std::string::npos && line.find('#') == std::string::npos) {
            bool_ternaries(line);
        }
        // if (x) with a float x: HLSL's truthiness.
        for (std::size_t at = line.find("if"); at != std::string::npos;
             at = line.find("if", at + 2)) {
            bool word = (at == 0 || !(std::isalnum(static_cast<unsigned char>(line[at - 1])) ||
                                      line[at - 1] == '_'));
            std::size_t open = line.find_first_not_of(" \t", at + 2);
            if (!word || open == std::string::npos || line[open] != '(') continue;
            int level = 0;
            std::size_t close = std::string::npos;
            for (std::size_t i = open; i < line.size(); ++i) {
                if (line[i] == '(') ++level;
                if (line[i] == ')' && --level == 0) {
                    close = i;
                    break;
                }
            }
            if (close == std::string::npos) continue;
            line.insert(close, ")");
            line.insert(open + 1, "bool(");
            at = close + 6;
        }
        // Locals are scoped by function; a declaration seen at any depth stays until the
        // function ends, which is close enough for the shaders seen.
        for (char c : line) {
            if (c == '{') ++depth;
            if (c == '}') --depth;
        }
        if (depth == 0) locals.clear();
        out += line;
        out += '\n';
    }
    body = std::move(out);
}

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

// Wallpaper Engine tolerates an #endif too many (a Workshop shader of 3510729512 has one) and,
// presumably, one too few; glslang does not. Stray #endif lines go, missing ones are added.
void balance_conditionals(std::string& text) {
    static const std::regex open(R"(^\s*#\s*(if|ifdef|ifndef)\b)");
    static const std::regex close(R"(^\s*#\s*endif\b)");
    std::istringstream in(text);
    std::string out, line;
    int depth = 0;
    while (std::getline(in, line)) {
        if (std::regex_search(line, open)) {
            ++depth;
        } else if (std::regex_search(line, close)) {
            if (depth == 0) {
                out += "// " + line + " (unmatched, dropped)\n";
                continue;
            }
            --depth;
        }
        out += line;
        out += '\n';
    }
    while (depth-- > 0) out += "#endif\n";
    text = std::move(out);
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
        balance_conditionals(sources[i]);
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

    for (int i = 0; i < 2; ++i) {
        std::map<std::string, std::string> globals;
        for (const auto* list : {&stages[i].uniforms, &stages[i].inputs, &stages[i].outputs}) {
            for (const auto& d : *list) {
                if (d.array.empty()) globals[d.name] = d.type;
            }
        }
        if (i == 1) globals["glint_FragColor"] = "vec4";
        hlsl_conversions(stages[i].body, globals);
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
    std::string header = "#version 450\n" + overload_helpers();
    static const std::regex own_log10(R"(\b(?:float|vec[234])\s+(?:glint_)?log10\s*\()");
    if (!std::regex_search(stages[0].body, own_log10) &&
        !std::regex_search(stages[1].body, own_log10)) {
        header += "float glint_log10(float x) { return log2(x) * 0.30103; }\n";
        header += "vec2 glint_log10(vec2 x) { return log2(x) * 0.30103; }\n";
        header += "vec3 glint_log10(vec3 x) { return log2(x) * 0.30103; }\n";
        header += "vec4 glint_log10(vec4 x) { return log2(x) * 0.30103; }\n";
    }
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
            std::uint32_t count = 1;
            if (!d.array.empty())
                count = static_cast<std::uint32_t>(std::max(1, std::atoi(d.array.c_str() + 1)));
            next += locations_of(d.type) * count;
        }
    }
    // Direct3D lets the stages disagree on a varying's width; Vulkan does not. The fragment
    // stage then reads the vertex stage's type under another name and converts at the top of
    // main: truncated, or padded with zeros.
    std::map<std::string, std::string> vertex_types;
    for (const auto& d : stages[0].outputs) vertex_types[d.name] = d.type;
    std::string vertex_io, fragment_io, fragment_globals, fragment_prologue;
    std::uint32_t attribute = 0;
    // HLSL lets a shader write to its inputs; GLSL does not. Inputs arrive under another name
    // and are copied into a global of the shader's name at the top of main.
    std::string vertex_globals, vertex_prologue;
    for (const auto& d : stages[0].inputs) {
        result.attributes[d.name] = attribute;
        std::string where = "layout(location = " + std::to_string(attribute) + ") in ";
        if (d.array.empty()) {
            vertex_io += where + d.type + " glint_in_" + d.name + ";\n";
            vertex_globals += d.type + " " + d.name + ";\n";
            vertex_prologue += "    " + d.name + " = glint_in_" + d.name + ";\n";
        } else {
            vertex_io += where + d.type + " " + d.name + d.array + ";\n";
        }
        attribute +=
            locations_of(d.type) *
            (d.array.empty()
                 ? 1u
                 : static_cast<std::uint32_t>(std::max(1, std::atoi(d.array.c_str() + 1))));
    }
    vertex_io += vertex_globals;
    for (const auto& d : stages[0].outputs) {
        vertex_io += "layout(location = " + std::to_string(varying_locations[d.name]) + ") out " +
                     d.type + " " + d.name + d.array + ";\n";
    }
    for (const auto& d : stages[1].inputs) {
        std::string location = std::to_string(varying_locations[d.name]);
        auto vt = vertex_types.find(d.name);
        if (vt == vertex_types.end()) {
            // Nothing feeds it: a global, zero, instead of an input the pipeline would
            // leave undefined.
            fragment_globals += d.type + " " + d.name + d.array + ";\n";
            if (d.array.empty()) fragment_prologue += "    " + d.name + " = " + d.type + "(0);\n";
            continue;
        }
        if (!d.array.empty()) {
            fragment_io +=
                "layout(location = " + location + ") in " + d.type + " " + d.name + d.array + ";\n";
            continue;
        }
        if (vt->second == d.type) {
            fragment_io +=
                "layout(location = " + location + ") in " + d.type + " glint_in_" + d.name + ";\n";
            fragment_globals += d.type + " " + d.name + ";\n";
            fragment_prologue += "    " + d.name + " = glint_in_" + d.name + ";\n";
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
    if (!vertex_prologue.empty()) {
        static const std::regex vertex_main(R"(void\s+main\s*\(\s*(void)?\s*\)\s*\{)");
        std::smatch m;
        if (!std::regex_search(stages[0].body, m, vertex_main)) {
            result.error = "vertex stage has no main()";
            return result;
        }
        auto at = static_cast<std::size_t>(m.position(0) + m.length(0));
        stages[0].body.insert(at, "\n" + vertex_prologue);
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

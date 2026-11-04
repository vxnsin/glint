#include "wallpaper/scene/project.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace glint::scene {

namespace {

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string string_of(const nlohmann::json& json, const char* key) {
    if (!json.contains(key)) return {};
    const auto& value = json[key];
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    return {};
}

// ---- Condition expressions -------------------------------------------------------------

struct Value {
    enum class Type { undefined, boolean, number, string } type = Type::undefined;
    bool b = false;
    double n = 0;
    std::string s;
};

Value from_json(const nlohmann::json& json) {
    Value v;
    if (json.is_boolean()) {
        v.type = Value::Type::boolean;
        v.b = json.get<bool>();
    } else if (json.is_number()) {
        v.type = Value::Type::number;
        v.n = json.get<double>();
    } else if (json.is_string()) {
        v.type = Value::Type::string;
        v.s = json.get<std::string>();
    }
    return v;
}

bool truthy(const Value& v) {
    switch (v.type) {
        case Value::Type::undefined: return false;
        case Value::Type::boolean: return v.b;
        case Value::Type::number: return v.n != 0 && !std::isnan(v.n);
        case Value::Type::string: return !v.s.empty();
    }
    return false;
}

double number(const Value& v) {
    switch (v.type) {
        case Value::Type::undefined: return std::nan("");
        case Value::Type::boolean: return v.b ? 1 : 0;
        case Value::Type::number: return v.n;
        case Value::Type::string: {
            if (v.s.empty()) return 0;
            double out = 0;
            auto [p, ec] = std::from_chars(v.s.data(), v.s.data() + v.s.size(), out);
            return ec == std::errc() && p == v.s.data() + v.s.size() ? out : std::nan("");
        }
    }
    return 0;
}

// JavaScript's loose ==, enough for the values properties have.
bool loose_equal(const Value& a, const Value& b) {
    if (a.type == Value::Type::undefined || b.type == Value::Type::undefined) {
        return a.type == b.type;
    }
    if (a.type == Value::Type::string && b.type == Value::Type::string) return a.s == b.s;
    return number(a) == number(b);
}

bool strict_equal(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    return loose_equal(a, b);
}

class Parser {
public:
    Parser(std::string_view text, const Values& values) : text_(text), values_(values) {}

    bool parse(bool& out) {
        Value v = expression();
        skip();
        if (failed_ || pos_ != text_.size()) return false;
        out = truthy(v);
        return true;
    }

private:
    void skip() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }
    bool take(std::string_view token) {
        skip();
        if (text_.substr(pos_, token.size()) != token) return false;
        pos_ += token.size();
        return true;
    }

    Value expression() { return disjunction(); }

    Value disjunction() {
        Value left = conjunction();
        while (take("||")) {
            Value right = conjunction();
            left = truthy(left) ? left : right;
        }
        return left;
    }

    Value conjunction() {
        Value left = comparison();
        while (take("&&")) {
            Value right = comparison();
            left = truthy(left) ? right : left;
        }
        return left;
    }

    Value comparison() {
        Value left = unary();
        for (;;) {
            Value result;
            result.type = Value::Type::boolean;
            if (take("===")) {
                result.b = strict_equal(left, unary());
            } else if (take("!==")) {
                result.b = !strict_equal(left, unary());
            } else if (take("==")) {
                result.b = loose_equal(left, unary());
            } else if (take("!=")) {
                result.b = !loose_equal(left, unary());
            } else if (take("<=")) {
                result.b = number(left) <= number(unary());
            } else if (take(">=")) {
                result.b = number(left) >= number(unary());
            } else if (take("<")) {
                result.b = number(left) < number(unary());
            } else if (take(">")) {
                result.b = number(left) > number(unary());
            } else {
                return left;
            }
            left = result;
        }
    }

    Value unary() {
        if (take("!")) {
            Value v;
            v.type = Value::Type::boolean;
            v.b = !truthy(unary());
            return v;
        }
        return primary();
    }

    Value primary() {
        skip();
        if (pos_ >= text_.size()) return fail();
        char c = text_[pos_];
        if (c == '(') {
            ++pos_;
            Value v = expression();
            if (!take(")")) return fail();
            return v;
        }
        if (c == '\'' || c == '"') {
            std::size_t end = text_.find(c, pos_ + 1);
            if (end == std::string_view::npos) return fail();
            Value v;
            v.type = Value::Type::string;
            v.s = std::string(text_.substr(pos_ + 1, end - pos_ - 1));
            pos_ = end + 1;
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.') {
            std::size_t start = pos_++;
            while (pos_ < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '.')) {
                ++pos_;
            }
            Value v;
            v.type = Value::Type::number;
            std::string digits(text_.substr(start, pos_ - start));
            v.n = std::strtod(digits.c_str(), nullptr);
            return v;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t start = pos_;
            while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
                                           text_[pos_] == '_' || text_[pos_] == '.')) {
                ++pos_;
            }
            std::string_view word = text_.substr(start, pos_ - start);
            Value v;
            if (word == "true" || word == "false") {
                v.type = Value::Type::boolean;
                v.b = word == "true";
                return v;
            }
            std::string_view name = word;
            if (name.ends_with(".value")) name.remove_suffix(6);
            if (auto it = values_.find(name); it != values_.end()) return from_json(it->second);
            return v;  // undefined, like a missing property in WE
        }
        return fail();
    }

    Value fail() {
        failed_ = true;
        pos_ = text_.size();
        return {};
    }

    std::string_view text_;
    const Values& values_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

}  // namespace

const ProjectProperty* Project::property(std::string_view name) const {
    for (const auto& p : properties) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

std::string parse_project(std::string_view text, Project& out, std::string_view language) {
    nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return "project.json is not valid JSON";
    out.title = string_of(json, "title");
    out.type = lower(string_of(json, "type"));
    out.file = string_of(json, "file");
    out.preview = string_of(json, "preview");
    out.rating = string_of(json, "contentrating");
    out.workshop_id = string_of(json, "workshopid");
    if (json.contains("tags") && json["tags"].is_array()) {
        for (const auto& tag : json["tags"]) {
            if (tag.is_string()) out.tags.push_back(tag.get<std::string>());
        }
    }
    if (out.type.empty()) return "project.json has no type";

    const nlohmann::json general = json.value("general", nlohmann::json::object());
    if (general.contains("supportsaudioprocessing")) {
        out.audio = general["supportsaudioprocessing"].is_boolean() &&
                    general["supportsaudioprocessing"].get<bool>();
    }
    // Labels may be localisation keys; the block is keyed by language, then by key.
    nlohmann::json strings = nlohmann::json::object();
    if (general.contains("localization") && general["localization"].is_object()) {
        const auto& loc = general["localization"];
        if (loc.contains(std::string(language))) {
            strings = loc[std::string(language)];
        } else if (loc.contains("en-us")) {
            strings = loc["en-us"];
        }
    }
    auto label = [&](const std::string& key) {
        if (strings.is_object() && strings.contains(key) && strings[key].is_string()) {
            return strings[key].get<std::string>();
        }
        return key;
    };

    if (general.contains("properties") && general["properties"].is_object()) {
        for (const auto& [name, entry] : general["properties"].items()) {
            if (!entry.is_object()) continue;
            ProjectProperty p;
            p.name = name;
            p.type = lower(string_of(entry, "type"));
            if (p.type.empty()) p.type = "text";
            p.text = label(string_of(entry, "text"));
            p.value = entry.value("value", nlohmann::json());
            p.min = entry.value("min", nlohmann::json());
            p.max = entry.value("max", nlohmann::json());
            p.step = entry.value("step", nlohmann::json());
            p.precision = entry.value("precision", nlohmann::json());
            p.condition = string_of(entry, "condition");
            if (entry.contains("order") && entry["order"].is_number()) {
                p.order = entry["order"].get<int>();
            }
            if (entry.contains("options") && entry["options"].is_array()) {
                for (const auto& option : entry["options"]) {
                    if (!option.is_object()) continue;
                    std::string value = option.contains("value") && option["value"].is_string()
                                            ? option["value"].get<std::string>()
                                            : option.value("value", nlohmann::json()).dump();
                    p.options.emplace_back(label(string_of(option, "label")), value);
                }
            }
            out.properties.push_back(std::move(p));
        }
    }
    std::ranges::stable_sort(out.properties, {}, &ProjectProperty::order);
    return {};
}

Values default_values(const Project& project) {
    Values values;
    for (const auto& p : project.properties) values[p.name] = p.value;
    return values;
}

bool evaluate_condition(std::string_view expression, const Values& values, std::string* error) {
    std::string_view trimmed = expression;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.remove_prefix(1);
    }
    if (trimmed.empty()) return true;
    Parser parser(trimmed, values);
    bool result = true;
    if (!parser.parse(result)) {
        if (error != nullptr) *error = "cannot read condition: " + std::string(expression);
        return true;
    }
    return result;
}

std::string color_to_hex(std::string_view rgb) {
    std::istringstream in{std::string(rgb)};
    double c[3] = {0, 0, 0};
    in >> c[0] >> c[1] >> c[2];
    // Some files store 0-255 instead of 0-1.
    bool bytes = c[0] > 1.0 || c[1] > 1.0 || c[2] > 1.0;
    char hex[8];
    auto byte = [&](double v) {
        double scaled = bytes ? v : v * 255.0;
        return static_cast<int>(std::lround(std::clamp(scaled, 0.0, 255.0)));
    };
    std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]));
    return hex;
}

std::optional<std::string> hex_to_color(std::string_view hex) {
    if (hex.size() != 7 || hex[0] != '#') return std::nullopt;
    int parts[3];
    for (int i = 0; i < 3; ++i) {
        auto digits = hex.substr(static_cast<std::size_t>(1 + i * 2), 2);
        auto [p, ec] = std::from_chars(digits.data(), digits.data() + 2, parts[i], 16);
        if (ec != std::errc() || p != digits.data() + 2) return std::nullopt;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.5g %.5g %.5g", parts[0] / 255.0, parts[1] / 255.0,
                  parts[2] / 255.0);
    return std::string(text);
}

}  // namespace glint::scene

// Tests for the Wallpaper Engine readers. Synthetic files always; the corpus items too when
// they are installed (GLINT_WORKSHOP, else Steam's default Workshop folder).

#include <lz4.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/tex.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace {

using namespace glint::scene;

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++checks;                                                                         \
        if (!(cond)) {                                                                    \
            ++failures;                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (false)

template <typename T>
void put(std::vector<std::byte>& out, T value) {
    auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

void put_magic(std::vector<std::byte>& out, const char* magic) {
    for (int i = 0; i < 9; ++i) out.push_back(static_cast<std::byte>(i < 8 ? magic[i] : 0));
}

void put_string(std::vector<std::byte>& out, const std::string& text) {
    put(out, static_cast<std::uint32_t>(text.size()));
    for (char c : text) out.push_back(static_cast<std::byte>(c));
}

void test_conditions() {
    Values values = {{"snow", true},  {"season", "winter"}, {"combo", "1"},
                     {"level", 0.55}, {"off", false},       {"empty", ""}};
    CHECK(evaluate_condition("", values));
    CHECK(evaluate_condition("snow.value", values));
    CHECK(!evaluate_condition("off.value", values));
    CHECK(evaluate_condition("!off.value", values));
    CHECK(evaluate_condition("season.value == 'winter' && snow.value", values));
    CHECK(!evaluate_condition("season.value == \"summer\" || off.value", values));
    CHECK(evaluate_condition("combo.value == 1", values));    // loose, as in JavaScript
    CHECK(!evaluate_condition("combo.value === 1", values));  // strict
    CHECK(evaluate_condition("combo.value != 2", values));
    CHECK(evaluate_condition("level.value > 0.5 && level.value <= 0.55", values));
    CHECK(evaluate_condition("(off.value || snow.value) && !(combo.value == 2)", values));
    CHECK(evaluate_condition("snow.value==true", values));
    CHECK(!evaluate_condition("missing.value", values));
    CHECK(!evaluate_condition("empty.value", values));
    std::string error;
    CHECK(evaluate_condition("snow.value &&", values, &error));  // unreadable stays visible
    CHECK(!error.empty());
}

void test_colors() {
    CHECK(color_to_hex("1 0.5 0") == "#ff8000");
    CHECK(color_to_hex("0.25882352941176473 0.4235294117647059 0.8470588235294118") == "#426cd8");
    CHECK(color_to_hex("255 128 0") == "#ff8000");
    auto back = hex_to_color("#ff8000");
    CHECK(back.has_value() && color_to_hex(*back) == "#ff8000");
    CHECK(!hex_to_color("ff8000"));
}

void test_project() {
    const char* text = R"({"title": "T", "type": "Scene", "file": "scene.json",
        "general": {"supportsaudioprocessing": true,
          "localization": {"en-us": {"ui_key": "Snow"}},
          "properties": {
            "b": {"type": "bool", "text": "ui_key", "value": true, "order": 2},
            "a": {"type": "combo", "text": "Season", "value": "1", "order": 1,
                  "options": [{"label": "Summer", "value": "1"}, {"label": "Winter", "value": "2"}]},
            "c": {"text": "<b>label</b>", "value": "", "order": 3, "condition": "a.value == 2"}}}})";
    Project project;
    CHECK(parse_project(text, project).empty());
    CHECK(project.type == "scene");
    CHECK(project.audio);
    CHECK(project.properties.size() == 3);
    CHECK(project.properties[0].name == "a" && project.properties[0].options.size() == 2);
    CHECK(project.properties[1].text == "Snow");
    CHECK(project.properties[2].type == "text");
    Values values = default_values(project);
    CHECK(!evaluate_condition(project.properties[2].condition, values));
    values["a"] = "2";
    CHECK(evaluate_condition(project.properties[2].condition, values));
}

void test_package() {
    std::vector<std::byte> pkg;
    put_string(pkg, "PKGV0024");
    put(pkg, std::uint32_t{2});
    put_string(pkg, "scene.json");
    put(pkg, std::uint32_t{0});
    put(pkg, std::uint32_t{2});
    put_string(pkg, "materials/a.json");
    put(pkg, std::uint32_t{2});
    put(pkg, std::uint32_t{3});
    for (char c : std::string("{}abc")) pkg.push_back(static_cast<std::byte>(c));

    auto dir = std::filesystem::temp_directory_path() / ("glint-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "scene.pkg", std::ios::binary);
        out.write(reinterpret_cast<const char*>(pkg.data()),
                  static_cast<std::streamsize>(pkg.size()));
        std::ofstream loose(dir / "materials.txt");
        loose << "loose";
    }
    Vfs vfs(dir, {});
    CHECK(vfs.open_package().empty());
    CHECK(vfs.package() != nullptr && vfs.package()->version() == "PKGV0024");
    auto scene = vfs.read("scene.json");
    CHECK(scene && scene->text() == "{}");
    auto material = vfs.read("materials/a.json");
    CHECK(material && material->text() == "abc");
    CHECK(vfs.locate("materials.txt") == Vfs::Source::folder);
    CHECK(vfs.locate("../etc/passwd") == Vfs::Source::none);
    CHECK(vfs.locate("/etc/passwd") == Vfs::Source::none);
    CHECK(!vfs.read("missing.json"));

    // Truncated: the table says more than there is.
    pkg.resize(pkg.size() - 2);
    {
        std::ofstream out(dir / "bad.pkg", std::ios::binary);
        out.write(reinterpret_cast<const char*>(pkg.data()),
                  static_cast<std::streamsize>(pkg.size()));
    }
    std::string error;
    CHECK(!Package::open(dir / "bad.pkg", error) && !error.empty());
    std::filesystem::remove_all(dir);
}

void test_tex() {
    // TEXB0004, R8, 8x4, one LZ4 mip and one raw mip.
    std::vector<std::byte> raw(32);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::byte>(i % 3);
    std::vector<char> packed(static_cast<std::size_t>(LZ4_compressBound(32)));
    int packed_size = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()), packed.data(),
                                           32, static_cast<int>(packed.size()));
    std::vector<std::byte> tex;
    put_magic(tex, "TEXV0005");
    put_magic(tex, "TEXI0001");
    put(tex, std::int32_t{9});  // R8
    put(tex, std::uint32_t{2});
    put(tex, std::int32_t{8});
    put(tex, std::int32_t{4});
    put(tex, std::int32_t{7});
    put(tex, std::int32_t{3});
    put(tex, std::uint32_t{0});
    put_magic(tex, "TEXB0004");
    put(tex, std::int32_t{1});   // images
    put(tex, std::int32_t{-1});  // image format
    put(tex, std::int32_t{0});   // video
    put(tex, std::int32_t{2});   // mips
    put(tex, std::int32_t{8});
    put(tex, std::int32_t{4});
    put(tex, std::int32_t{1});
    put(tex, std::int32_t{32});
    put(tex, std::int32_t{packed_size});
    for (int i = 0; i < packed_size; ++i)
        tex.push_back(static_cast<std::byte>(packed[static_cast<std::size_t>(i)]));
    put(tex, std::int32_t{4});
    put(tex, std::int32_t{2});
    put(tex, std::int32_t{0});
    put(tex, std::int32_t{8});
    put(tex, std::int32_t{8});
    for (int i = 0; i < 8; ++i) tex.push_back(std::byte{7});

    Tex parsed;
    CHECK(parse_tex(tex, parsed).empty());
    CHECK(parsed.format == 9 && parsed.img_width == 7 && parsed.tex_width == 8);
    CHECK(parsed.images.size() == 1 && parsed.images[0].size() == 2);
    std::vector<std::byte> bytes;
    CHECK(mip_bytes(parsed.images[0][0], bytes) && bytes == raw);
    CHECK(mip_bytes(parsed.images[0][1], bytes) && bytes.size() == 8);

    tex.resize(tex.size() - 3);
    Tex truncated;
    CHECK(!parse_tex(tex, truncated).empty());
}

void test_corpus() {
    std::filesystem::path workshop;
    if (const char* env = std::getenv("GLINT_WORKSHOP")) {
        workshop = env;
    } else if (const char* home = std::getenv("HOME")) {
        workshop =
            std::filesystem::path(home) / ".local/share/Steam/steamapps/workshop/content/431960";
    }
    const char* corpus[] = {"3780525249", "3426865175", "1705122927", "1139304621", "1275921440"};
    int found = 0;
    for (const char* id : corpus) {
        auto folder = workshop / id;
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) continue;
        ++found;
        Vfs vfs(folder, default_assets());
        CHECK(vfs.open_package().empty());
        auto file = vfs.read("project.json");
        CHECK(file.has_value());
        Project project;
        CHECK(file && parse_project(file->text(), project).empty());
        CHECK(project.type == "scene");
        CHECK(vfs.exists(project.file));
        Values values = default_values(project);
        for (const auto& p : project.properties) {
            std::string error;
            evaluate_condition(p.condition, values, &error);
            CHECK(error.empty());
        }
        if (vfs.package() == nullptr) continue;
        int textures = 0;
        for (const auto& entry : vfs.package()->entries()) {
            if (!entry.path.ends_with(".tex")) continue;
            ++textures;
            auto data = vfs.read(entry.path);
            Tex tex;
            std::string error = data ? parse_tex(data->bytes(), tex) : "unreadable";
            if (!error.empty())
                std::fprintf(stderr, "%s %s: %s\n", id, entry.path.c_str(), error.c_str());
            CHECK(error.empty());
            for (const auto& image : tex.images) {
                for (const auto& mip : image) {
                    std::vector<std::byte> bytes;
                    CHECK(mip_bytes(mip, bytes));
                }
            }
        }
        std::printf("corpus %s: %d textures\n", id, textures);
    }
    std::printf("corpus: %d of 5 items installed\n", found);
}

}  // namespace

int main() {
    test_conditions();
    test_colors();
    test_project();
    test_package();
    test_tex();
    test_corpus();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

#include "cli/library.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            ++failures;                                     \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x); \
        }                                                   \
    } while (false)
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("glint-library-test-" + std::to_string(getpid()));
    for (const auto* id : {"42", "43", "44"}) std::filesystem::create_directories(root / id);
    std::ofstream(root / "42/project.json") << R"({"title":"Nozomi Hikari","type":"scene",
        "file":"scene.json","tags":["Anime"],"general":{"properties":{
          "flag":{"type":"bool","value":false},
          "strength":{"type":"slider","value":2,"min":0,"max":5,"condition":"flag.value"}}}})";
    std::ofstream(root / "42/scene.json") << R"({"objects":[]})";
    std::ofstream(root / "43/project.json")
        << R"({"title":"Roxy","type":"video","file":"movie.mp4"})";
    std::ofstream(root / "43/movie.mp4");
    std::ofstream(root / "44/project.json") << "{broken";
    std::string error;
    auto list = glint::cli::list_library(root, "", error);
    CHECK(error.empty() && list.size() == 2);
    list = glint::cli::list_library(root, "HIKARI", error);
    CHECK(error.empty() && list.size() == 1 && list[0]["id"] == "42");
    CHECK(glint::cli::list_library(root, "anime", error).size() == 1);
    CHECK(glint::cli::list_library(root, "missing", error).empty());
    auto item = glint::cli::inspect_wallpaper(root / "42", error);
    CHECK(error.empty() && item["entry_exists"] == true);
    CHECK(item["scene"]["images"] == 0);
    CHECK(item["properties"].size() == 2);
    for (const auto& p : item["properties"]) {
        if (p["name"] == "strength") CHECK(p["visible"] == false && p["max"] == 5);
    }
    item = glint::cli::inspect_wallpaper(root / "43", error);
    CHECK(error.empty() && item["type"] == "video" && item["entry_exists"] == true);
    CHECK(!item.contains("scene"));
    glint::cli::inspect_wallpaper(root / "44", error);
    CHECK(!error.empty());
    glint::cli::list_library(root / "absent", "", error);
    CHECK(!error.empty());
    std::filesystem::remove_all(root);
    std::printf("library regressions: %d failures\n", failures);
    return failures != 0;
}

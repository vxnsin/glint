#include "cli/library.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include "ipc/library.hpp"

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
    const char* old_data = std::getenv("XDG_DATA_HOME");
    const std::optional<std::string> saved_data =
        old_data ? std::optional<std::string>(old_data) : std::nullopt;
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    const auto managed = glint::cli::download_path();
    CHECK(glint::cli::register_workshop_download(root / "43", managed).empty());
    CHECK(glint::cli::register_workshop_download(root / "43", managed).empty());
    CHECK(!glint::cli::register_workshop_download(root / "44", managed).empty());
    CHECK(glint::cli::list_installed_library(root, managed, "", error).size() == 2);
    CHECK(glint::cli::list_installed_library(root / "no-steam", managed, "", error).size() == 1 &&
          error.empty());
    const char* old_workshop = std::getenv("GLINT_WORKSHOP");
    const std::optional<std::string> saved =
        old_workshop ? std::optional<std::string>(old_workshop) : std::nullopt;
    setenv("GLINT_WORKSHOP", root.c_str(), 1);
    auto request = glint::ipc::parse_library_request({{"op", "list"}, {"search", "HIKARI"}}, error);
    CHECK(request && error.empty());
    if (request) {
        auto result = glint::ipc::execute_library_request(*request, error);
        CHECK(error.empty() && result.size() == 1 && result[0]["id"] == "42");
    }
    request = glint::ipc::parse_library_request({{"op", "inspect"}, {"path", "42"}}, error);
    CHECK(request && error.empty());
    if (request) {
        auto result = glint::ipc::execute_library_request(*request, error);
        CHECK(error.empty() && result["title"] == "Nozomi Hikari");
        CHECK(result["properties"].is_array() && result["scene"]["images"] == 0);
    }
    CHECK(!glint::ipc::parse_library_request({{"op", "list"}, {"search", 1}}, error));
    CHECK(!error.empty());
    CHECK(!glint::ipc::parse_library_request({{"op", "inspect"}, {"path", ""}}, error));
    CHECK(!error.empty());
    request = glint::ipc::parse_library_request({{"op", "inspect"}, {"path", "44"}}, error);
    CHECK(request);
    if (request) {
        glint::ipc::execute_library_request(*request, error);
        CHECK(!error.empty());
    }
    if (saved)
        setenv("GLINT_WORKSHOP", saved->c_str(), 1);
    else
        unsetenv("GLINT_WORKSHOP");
    if (saved_data)
        setenv("XDG_DATA_HOME", saved_data->c_str(), 1);
    else
        unsetenv("XDG_DATA_HOME");
    std::filesystem::remove_all(root);
    std::printf("library regressions: %d failures\n", failures);
    return failures != 0;
}

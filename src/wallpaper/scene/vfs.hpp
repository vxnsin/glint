#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The files a Wallpaper Engine item sees: its own folder, then its scene.pkg, then the
// assets/ folder of the Wallpaper Engine install, then each assets/effects/<name>/ folder
// (an effect keeps its materials and shaders there). Paths are relative and use "/".
namespace glint::scene {

// A scene.pkg, memory-mapped. Entries are views into the mapping.
class Package {
public:
    struct Entry {
        std::string path;
        std::size_t offset;  // from the start of the file
        std::size_t size;
    };

    // Returns null with an error on a file that is not a package.
    static std::unique_ptr<Package> open(const std::filesystem::path& path, std::string& error);
    ~Package();
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    const std::string& version() const { return version_; }  // "PKGV0024"
    const std::vector<Entry>& entries() const { return entries_; }
    std::optional<std::span<const std::byte>> find(std::string_view path) const;

private:
    Package() = default;

    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::string version_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};

// A file's bytes: either a view into a package or owned data read from disk.
class File {
public:
    File() = default;
    explicit File(std::span<const std::byte> view) : view_(view) {}
    explicit File(std::vector<std::byte> data) : owned_(std::move(data)), view_(owned_) {}
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::span<const std::byte> bytes() const { return view_; }
    std::string_view text() const {
        return {reinterpret_cast<const char*>(view_.data()), view_.size()};
    }

private:
    std::vector<std::byte> owned_;
    std::span<const std::byte> view_;
};

class Vfs {
public:
    // item: the Workshop folder (or any folder with project.json). assets: Wallpaper Engine's
    // assets/ folder, may be empty.
    Vfs(std::filesystem::path item, std::filesystem::path assets);

    // Opens scene.pkg if there is one. Returns an error message or empty.
    std::string open_package(std::string_view name = "scene.pkg");

    // Where a path comes from, for messages and we-inspect.
    enum class Source { none, folder, package, assets };
    Source locate(std::string_view path) const;

    std::optional<File> read(std::string_view path) const;
    bool exists(std::string_view path) const { return locate(path) != Source::none; }

    const std::filesystem::path& item() const { return item_; }
    const std::filesystem::path& assets() const { return assets_; }
    const Package* package() const { return package_.get(); }

private:
    std::filesystem::path item_;
    std::filesystem::path assets_;
    std::vector<std::filesystem::path> effect_roots_;
    std::unique_ptr<Package> package_;
};

// Wallpaper Engine's assets folder: $GLINT_ASSETS, else the default Steam library.
std::filesystem::path default_assets();

const char* source_name(Vfs::Source source);

}  // namespace glint::scene

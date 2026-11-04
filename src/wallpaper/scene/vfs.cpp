#include "wallpaper/scene/vfs.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace glint::scene {

namespace {

bool read_u32(const std::byte* data, std::size_t size, std::size_t& offset, std::uint32_t& out) {
    if (offset + 4 > size) return false;
    std::memcpy(&out, data + offset, 4);
    offset += 4;
    return true;
}

bool read_string(const std::byte* data, std::size_t size, std::size_t& offset, std::string& out) {
    std::uint32_t length = 0;
    if (!read_u32(data, size, offset, length) || offset + length > size) return false;
    out.assign(reinterpret_cast<const char*>(data + offset), length);
    offset += length;
    return true;
}

// A relative path that stays inside its root: no leading "/", no "..".
bool safe_relative(std::string_view path) {
    if (path.empty() || path.front() == '/') return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        if (path.substr(start, end - start) == "..") return false;
        start = end + 1;
    }
    return true;
}

}  // namespace

std::unique_ptr<Package> Package::open(const std::filesystem::path& path, std::string& error) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    struct stat info{};
    ::fstat(fd, &info);
    auto size = static_cast<std::size_t>(info.st_size);
    void* map = size > 0 ? ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ::close(fd);
    if (map == MAP_FAILED) {
        error = "cannot map " + path.string();
        return nullptr;
    }
    std::unique_ptr<Package> package(new Package());
    package->data_ = static_cast<const std::byte*>(map);
    package->size_ = size;

    const std::byte* data = package->data_;
    std::size_t offset = 0;
    std::uint32_t count = 0;
    if (!read_string(data, size, offset, package->version_) ||
        !package->version_.starts_with("PKGV") || !read_u32(data, size, offset, count)) {
        error = "not a scene.pkg";
        return nullptr;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Entry entry;
        std::uint32_t at = 0, length = 0;
        if (!read_string(data, size, offset, entry.path) || !read_u32(data, size, offset, at) ||
            !read_u32(data, size, offset, length)) {
            error = "truncated package table";
            return nullptr;
        }
        entry.offset = at;
        entry.size = length;
        package->entries_.push_back(std::move(entry));
    }
    // Offsets count from the end of the table.
    for (std::size_t i = 0; i < package->entries_.size(); ++i) {
        Entry& entry = package->entries_[i];
        entry.offset += offset;
        if (entry.offset + entry.size > size) {
            error = "package entry " + entry.path + " runs past the end";
            return nullptr;
        }
        package->index_[entry.path] = i;
    }
    return package;
}

Package::~Package() {
    if (data_ != nullptr) ::munmap(const_cast<std::byte*>(data_), size_);
}

std::optional<std::span<const std::byte>> Package::find(std::string_view path) const {
    auto it = index_.find(std::string(path));
    if (it == index_.end()) return std::nullopt;
    const Entry& entry = entries_[it->second];
    return std::span<const std::byte>(data_ + entry.offset, entry.size);
}

File::File(File&& other) noexcept {
    *this = std::move(other);
}

File& File::operator=(File&& other) noexcept {
    if (this == &other) return *this;
    bool owned = !other.owned_.empty() && other.view_.data() == other.owned_.data();
    owned_ = std::move(other.owned_);
    view_ = owned ? std::span<const std::byte>(owned_) : other.view_;
    other.view_ = {};
    return *this;
}

Vfs::Vfs(std::filesystem::path item, std::filesystem::path assets)
    : item_(std::move(item)), assets_(std::move(assets)) {}

std::string Vfs::open_package(std::string_view name) {
    auto path = item_ / name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    std::string error;
    package_ = Package::open(path, error);
    return error;
}

Vfs::Source Vfs::locate(std::string_view path) const {
    if (!safe_relative(path)) return Source::none;
    std::error_code ec;
    if (std::filesystem::is_regular_file(item_ / path, ec)) return Source::folder;
    if (package_ && package_->find(path)) return Source::package;
    if (!assets_.empty() && std::filesystem::is_regular_file(assets_ / path, ec)) {
        return Source::assets;
    }
    return Source::none;
}

std::optional<File> Vfs::read(std::string_view path) const {
    std::filesystem::path disk;
    switch (locate(path)) {
        case Source::none: return std::nullopt;
        case Source::package: return File(*package_->find(path));
        case Source::folder: disk = item_ / path; break;
        case Source::assets: disk = assets_ / path; break;
    }
    std::ifstream in(disk, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    auto size = static_cast<std::size_t>(in.tellg());
    std::vector<std::byte> data(size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!in) return std::nullopt;
    return File(std::move(data));
}

std::filesystem::path default_assets() {
    if (const char* env = std::getenv("GLINT_ASSETS"); env != nullptr && env[0] == '/') {
        return env;
    }
    const char* home = std::getenv("HOME");
    const char* data = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base =
        data != nullptr && data[0] == '/'
            ? std::filesystem::path(data)
            : std::filesystem::path(home != nullptr ? home : "/") / ".local/share";
    auto path = base / "Steam/steamapps/common/wallpaper_engine/assets";
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) ? path : std::filesystem::path();
}

const char* source_name(Vfs::Source source) {
    switch (source) {
        case Vfs::Source::none: return "missing";
        case Vfs::Source::folder: return "folder";
        case Vfs::Source::package: return "pkg";
        case Vfs::Source::assets: return "assets";
    }
    return "?";
}

}  // namespace glint::scene

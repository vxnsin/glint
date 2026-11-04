// we-inspect: dumps what glint reads from a Wallpaper Engine item, a scene.pkg or a .tex.
//
//   we-inspect <item folder>     project.json, properties, the package listing
//   we-inspect <file.pkg>        the package listing
//   we-inspect <file.tex>        texture header, mips, sprite frames
//   we-inspect <item> <path>     one file through the item's file system (.tex or text)

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/tex.hpp"
#include "wallpaper/scene/vfs.hpp"

namespace {

using namespace glint::scene;

int dump_tex(std::span<const std::byte> data, const std::string& name) {
    Tex tex;
    if (std::string error = parse_tex(data, tex); !error.empty()) {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.c_str());
        return 1;
    }
    std::printf(
        "%s: %s, %s, %ux%u (stored %ux%u), flags 0x%x%s%s%s\n", name.c_str(), tex.container.c_str(),
        tex_format_name(tex.format), tex.img_width, tex.img_height, tex.tex_width, tex.tex_height,
        tex.flags,
        tex.embedded() ? (tex.image_format == 13 ? ", PNG per mip" : ", embedded files") : "",
        tex.sprite_sheet() ? ", sprite sheet" : "", tex.video ? ", video" : "");
    for (std::size_t i = 0; i < tex.images.size(); ++i) {
        std::printf("  image %zu: %zu mips\n", i, tex.images[i].size());
        for (const TexMip& mip : tex.images[i]) {
            std::vector<std::byte> bytes;
            bool ok = mip_bytes(mip, bytes);
            std::printf("    %ux%u %s %zu bytes%s\n", mip.width, mip.height,
                        mip.lz4 ? "lz4" : "raw", bytes.size(), ok ? "" : " (corrupt)");
        }
    }
    for (const TexFrame& frame : tex.frames) {
        std::printf("  frame image %d %.3fs at %.0f,%.0f %.0fx%.0f\n", frame.image, frame.time,
                    frame.x, frame.y, frame.width, frame.height);
    }
    return 0;
}

void dump_package(const Package& package) {
    std::printf("%s, %zu entries\n", package.version().c_str(), package.entries().size());
    for (const auto& entry : package.entries()) {
        std::printf("  %10zu  %s\n", entry.size, entry.path.c_str());
    }
}

int dump_item(const std::filesystem::path& folder) {
    Vfs vfs(folder, default_assets());
    if (std::string error = vfs.open_package(); !error.empty()) {
        std::fprintf(stderr, "scene.pkg: %s\n", error.c_str());
    }
    auto file = vfs.read("project.json");
    if (!file) {
        std::fprintf(stderr, "%s: no project.json\n", folder.c_str());
        return 1;
    }
    Project project;
    if (std::string error = parse_project(file->text(), project); !error.empty()) {
        std::fprintf(stderr, "project.json: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n  type %s, file %s, rating %s, audio %s\n", project.title.c_str(),
                project.type.c_str(), project.file.c_str(), project.rating.c_str(),
                project.audio ? "yes" : "no");
    std::printf("  assets %s\n", vfs.assets().empty() ? "(not found)" : vfs.assets().c_str());
    Values values = default_values(project);
    std::printf("properties:\n");
    for (const auto& p : project.properties) {
        std::string error;
        bool visible = evaluate_condition(p.condition, values, &error);
        // Cut long labels at a character boundary, not inside a UTF-8 sequence.
        std::string text = p.text;
        if (text.size() > 40) {
            std::size_t cut = 40;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            text = text.substr(0, cut) + "...";
        }
        std::printf("  %-14s %-8s %-6s %-24s %s%s%s\n", p.name.c_str(), p.type.c_str(),
                    visible ? "shown" : "hidden", p.value.dump().c_str(), text.c_str(),
                    p.condition.empty() ? "" : ("  if " + p.condition).c_str(),
                    error.empty() ? "" : "  (unreadable)");
    }
    if (vfs.package() != nullptr) dump_package(*vfs.package());
    if (vfs.exists(project.file)) {
        std::printf("%s from %s\n", project.file.c_str(), source_name(vfs.locate(project.file)));
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: we-inspect <item folder | file.pkg | file.tex> [path]\n");
        return 2;
    }
    std::filesystem::path target = argv[1];
    std::error_code ec;
    if (argc == 3) {
        Vfs vfs(target, default_assets());
        vfs.open_package();
        std::string path = argv[2];
        auto file = vfs.read(path);
        if (!file) {
            std::fprintf(stderr, "%s: not found in the item, its package or assets\n",
                         path.c_str());
            return 1;
        }
        std::fprintf(stderr, "%s from %s\n", path.c_str(), source_name(vfs.locate(path)));
        if (path.ends_with(".tex")) return dump_tex(file->bytes(), path);
        std::fwrite(file->bytes().data(), 1, file->bytes().size(), stdout);
        return 0;
    }
    if (std::filesystem::is_directory(target, ec)) return dump_item(target);
    if (target.extension() == ".pkg") {
        std::string error;
        auto package = Package::open(target, error);
        if (!package) {
            std::fprintf(stderr, "%s: %s\n", target.c_str(), error.c_str());
            return 1;
        }
        dump_package(*package);
        return 0;
    }
    Vfs vfs(target.parent_path(), {});
    auto file = vfs.read(target.filename().string());
    if (!file) {
        std::fprintf(stderr, "%s: cannot read\n", target.c_str());
        return 1;
    }
    return dump_tex(file->bytes(), target.filename().string());
}

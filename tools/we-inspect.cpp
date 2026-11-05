// we-inspect: dumps what glint reads from a Wallpaper Engine item, a scene.pkg or a .tex.
//
//   we-inspect <item folder>     project.json, properties, the package listing
//   we-inspect <file.pkg>        the package listing
//   we-inspect <file.tex>        texture header, mips, sprite frames
//   we-inspect <item> <path>     one file through the item's file system (.tex or text)
//   we-inspect <item> --shaders  compile every pass of the scene

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/scene.hpp"
#include "wallpaper/scene/shader.hpp"
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

// Compiles every pass of every image object and effect the way the renderer would.
int compile_all(const Vfs& vfs, const SceneDesc& scene, bool verbose) {
    ShaderCompiler compiler(vfs, {});
    int ok = 0, failed = 0;
    auto run = [&](const std::string& what, ShaderRequest request) {
        CompiledShader shader = compiler.compile(request);
        if (shader.error.empty()) {
            ++ok;
            if (verbose) {
                std::printf("  ok   %-40s %zu uniforms (%u bytes), %zu samplers\n", what.c_str(),
                            shader.uniforms.size(), shader.block_size, shader.samplers.size());
            }
        } else {
            ++failed;
            std::printf("  FAIL %s (%s)\n%s\n", what.c_str(), request.name.c_str(),
                        shader.error.c_str());
            // GLINT_SHADER_DUMP=<dir>: the generated sources of what failed, to read the
            // error's line numbers against.
            if (const char* dir = std::getenv("GLINT_SHADER_DUMP"); dir != nullptr) {
                std::string base = request.name;
                for (char& c : base) {
                    if (c == '/' || c == ' ') c = '_';
                }
                std::string path = std::string(dir) + "/" + base + "-" + std::to_string(failed);
                if (FILE* f = std::fopen((path + ".vert").c_str(), "w")) {
                    std::fputs(compiler.last_vertex_source().c_str(), f);
                    std::fclose(f);
                }
                if (FILE* f = std::fopen((path + ".frag").c_str(), "w")) {
                    std::fputs(compiler.last_fragment_source().c_str(), f);
                    std::fclose(f);
                }
            }
        }
    };
    for (const auto& image : scene.images) {
        for (const auto& pass : image.material.passes) {
            ShaderRequest request{pass.shader, pass.combos, 0};
            for (std::size_t i = 0; i < pass.textures.size(); ++i) {
                if (pass.textures[i]) request.bound_textures |= 1u << i;
            }
            run(image.name + " material", request);
        }
        for (const auto& effect : image.effects) {
            for (std::size_t p = 0; p < effect.passes.size(); ++p) {
                const EffectPass& pass = effect.passes[p];
                for (const auto& mp : pass.material.passes) {
                    ShaderRequest request{mp.shader, mp.combos, 1u};  // slot 0: the input
                    auto textures = mp.textures;
                    if (p < effect.overrides.size()) {
                        const auto& o = effect.overrides[p];
                        for (const auto& [k, v] : o.combos) request.combos[k] = v;
                        if (o.textures.size() > textures.size()) textures.resize(o.textures.size());
                        for (std::size_t i = 0; i < o.textures.size(); ++i) {
                            if (o.textures[i]) textures[i] = o.textures[i];
                        }
                    }
                    for (std::size_t i = 0; i < textures.size(); ++i) {
                        if (textures[i]) request.bound_textures |= 1u << i;
                    }
                    for (const auto& bind : pass.binds) request.bound_textures |= 1u << bind.index;
                    run(image.name + " " + mp.shader, request);
                }
            }
        }
    }
    std::printf("shaders: %d compiled, %d failed\n", ok, failed);
    return failed == 0 ? 0 : 1;
}

int dump_item(const std::filesystem::path& folder, bool shaders) {
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
    if (project.type != "scene") return 0;

    SceneDesc scene;
    if (std::string error = load_scene(vfs, project.file, scene); !error.empty()) {
        std::fprintf(stderr, "%s: %s\n", project.file.c_str(), error.c_str());
        return 1;
    }
    std::printf("scene: canvas %.0fx%.0f, clear %s, parallax %s\n", scene.general.ortho[0],
                scene.general.ortho[1], scene.general.clear_color.resolve(values).dump().c_str(),
                scene.general.camera_parallax ? "on" : "off");
    for (const auto& image : scene.images) {
        Vec3 origin = parse_vec3(image.origin.resolve(values), {0, 0, 0});
        Vec2 size = parse_vec2(image.size.resolve(values), image.model_size);
        std::printf("  image %d %s: at %.0f,%.0f size %.0fx%.0f%s%s%s%s\n", image.id,
                    image.name.c_str(), origin[0], origin[1], size[0], size[1],
                    image.visible.resolve_bool(values, true) ? "" : " hidden",
                    image.parent >= 0 ? (" parent " + std::to_string(image.parent)).c_str() : "",
                    image.puppet ? " puppet" : "", image.fullscreen ? " fullscreen" : "");
        for (const auto& pass : image.material.passes) {
            std::string textures;
            for (const auto& t : pass.textures) textures += (t ? *t : "(input)") + " ";
            std::printf("      material %s, %s, textures %s\n", pass.shader.c_str(),
                        pass.blending.c_str(), textures.c_str());
        }
        for (const auto& effect : image.effects) {
            std::printf("      effect %s%s: %zu passes, %zu fbos\n", effect.file.c_str(),
                        effect.visible.resolve_bool(values, true) ? "" : " (hidden)",
                        effect.passes.size(), effect.fbos.size());
            for (const auto& pass : effect.passes) {
                for (const auto& mp : pass.material.passes) {
                    std::printf("        %s%s%s\n", mp.shader.c_str(),
                                pass.target.empty() ? "" : (" -> " + pass.target).c_str(),
                                pass.command.empty() ? "" : (" " + pass.command).c_str());
                }
            }
        }
    }
    for (const auto& [kind, count] : scene.skipped) {
        std::printf("  not drawn yet: %d %s\n", count, kind.c_str());
    }
    for (const auto& warning : scene.warnings) std::printf("  warning: %s\n", warning.c_str());
    if (shaders) return compile_all(vfs, scene, true);
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
    if (argc == 3 && std::string_view(argv[2]) == "--shaders") return dump_item(target, true);
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
    if (std::filesystem::is_directory(target, ec)) return dump_item(target, false);
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

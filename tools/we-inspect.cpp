// we-inspect: dumps what glint reads from a Wallpaper Engine item, a scene.pkg or a .tex.
//
//   we-inspect <item folder>     project.json, properties, the package listing
//   we-inspect <file.pkg>        the package listing
//   we-inspect <file.tex>        texture header, mips, sprite frames
//   we-inspect <item> <path>     one file through the item's file system (.tex or text)
//   we-inspect <item> --shaders  compile every pass of the scene
//   we-inspect <item> --text     rasterise every text object to text-<id>.pam in the cwd

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "wallpaper/scene/mdl.hpp"
#include "wallpaper/scene/project.hpp"
#include "wallpaper/scene/scene.hpp"
#include "wallpaper/scene/shader.hpp"
#include "wallpaper/scene/tex.hpp"
#include "wallpaper/scene/text.hpp"
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
    // GLINT_TEX_RAW=<file>: the first mip's payload after LZ4, for looking at outside.
    if (const char* raw = std::getenv("GLINT_TEX_RAW");
        raw != nullptr && !tex.images.empty() && !tex.images[0].empty()) {
        std::vector<std::byte> bytes;
        if (mip_bytes(tex.images[0][0], bytes)) {
            if (FILE* f = std::fopen(raw, "wb")) {
                std::fwrite(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);
            }
        }
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
        // GLINT_SHADER_DUMP_ALL=<dir>: every generated source, failed or not.
        if (const char* dir = std::getenv("GLINT_SHADER_DUMP_ALL"); dir != nullptr) {
            std::string base = request.name;
            for (char& c : base) {
                if (c == '/' || c == ' ') c = '_';
            }
            std::string path = std::string(dir) + "/" + base + "-" + std::to_string(ok + failed);
            if (FILE* f = std::fopen((path + ".vert").c_str(), "w")) {
                std::fputs(compiler.last_vertex_source().c_str(), f);
                std::fclose(f);
            }
            if (FILE* f = std::fopen((path + ".frag").c_str(), "w")) {
                std::fputs(compiler.last_fragment_source().c_str(), f);
                std::fclose(f);
            }
        }
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

// Every text object of the scene as a PAM (P7 RGBA) file in the current folder.
int dump_texts(const Vfs& vfs, const SceneDesc& scene, const Values& values) {
    TextRenderer renderer;
    std::vector<std::string> warnings;
    for (const TextObject& text : scene.texts) {
        TextSpec spec;
        nlohmann::json value = text.text.resolve(values);
        spec.text = value.is_string() ? value.get<std::string>() : value.dump();
        spec.font = text.font;
        spec.pointsize = text.pointsize;
        spec.horizontal_align = text.horizontal_align;
        spec.vertical_align = text.vertical_align;
        spec.padding = text.padding;
        spec.spacing = text.spacing;
        spec.max_width = text.max_width;
        spec.max_rows = text.max_rows;
        spec.limit_rows = text.limit_rows;
        spec.limit_width = text.limit_width;
        spec.ellipsis = text.ellipsis;
        spec.block_align = text.block_align;
        spec.background = text.background;
        spec.opaque_background = text.opaque_background;
        TextBitmap bitmap = renderer.render(spec, vfs, warnings);
        std::string name = "text-" + std::to_string(text.id) + ".pam";
        std::printf("  text %d %s: \"%s\" in %s at %.0f pt, block %.0fx%.0f at %.0f,%.0f -> %s\n",
                    text.id, text.name.c_str(), spec.text.c_str(), text.font.c_str(),
                    text.pointsize, bitmap.size[0], bitmap.size[1], bitmap.center[0],
                    bitmap.center[1], name.c_str());
        if (bitmap.width == 0) continue;
        FILE* out = std::fopen(name.c_str(), "wb");
        if (out == nullptr) continue;
        std::fprintf(out,
                     "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                     bitmap.width, bitmap.height);
        std::fwrite(bitmap.rgba.data(), 1, bitmap.rgba.size(), out);
        std::fclose(out);
    }
    for (const auto& warning : warnings) std::printf("  warning: %s\n", warning.c_str());
    return 0;
}

int dump_item(const std::filesystem::path& folder, bool shaders, bool texts = false) {
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
    if (texts) return dump_texts(vfs, scene, values);
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
    if (argc == 3 && std::string_view(argv[2]) == "--text") return dump_item(target, false, true);
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
        if (path.ends_with(".mdl")) {
            Mesh mesh;
            std::string error = parse_mdl(file->bytes(), mesh);
            std::printf("%s: %s, %zu vertices, %zu triangles, material %s\n", path.c_str(),
                        error.empty() ? "ok" : error.c_str(), mesh.vertices.size() / 5,
                        mesh.indices.size() / 3, mesh.material.c_str());
            std::printf("  %zu bones, %zu attachments, %zu animations\n", mesh.bones.size(),
                        mesh.attachments.size(), mesh.animations.size());
            for (std::size_t b = 0; b < mesh.bones.size(); ++b) {
                const Mesh::Bone& bone = mesh.bones[b];
                std::printf("  bone %zu: parent %d, at %.1f %.1f\n", b, bone.parent,
                            bone.matrix[12], bone.matrix[13]);
            }
            for (const Mesh::Animation& a : mesh.animations) {
                std::printf("  animation %d \"%s\": %s, %.1f fps, %u frames, %zu tracks\n", a.id,
                            a.name.c_str(),
                            a.mode == Mesh::Animation::Mode::loop     ? "loop"
                            : a.mode == Mesh::Animation::Mode::mirror ? "mirror"
                                                                      : "single",
                            static_cast<double>(a.fps), a.length, a.tracks.size());
            }
            return 0;
        }
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

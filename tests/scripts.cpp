#include <cmath>
#include <cstdio>

#include "wallpaper/scene/script.hpp"

using namespace glint::scene;

namespace {
int failures = 0;
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            ++failures;                                     \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x); \
        }                                                   \
    } while (false)

SceneDesc scene_with_script(const char* source, bool text = false) {
    SceneDesc scene;
    scene.bound_count = 3;
    ImageObject image;
    image.id = 1;
    image.name = "layer";
    image.origin.key = 0;
    image.origin.value = "0 0 0";
    image.origin.script = true;
    image.origin.script_source = source;
    image.alpha.key = 1;
    image.alpha.value = 1;
    image.puppet_path = "rig.mdl";
    if (text) {
        image.text = 0;
        TextObject t;
        t.id = 1;
        t.text.key = 2;
        t.text.value = "hello";
        scene.texts.push_back(t);
    }
    scene.images.push_back(image);
    return scene;
}

void test_text_changes() {
    auto scene = scene_with_script(R"JS(
        export function init() {
            thisLayer.pointsize = 48;
            thisLayer.padding = new Vec2(12, 15);
            thisLayer.font = 'systemfont_verdana';
            thisLayer.horizontalalign = 'left';
            thisLayer.verticalalign = 'top';
            thisLayer.maxwidth = 240;
            thisLayer.limitwidth = true;
            thisLayer.maxrows = 3;
            thisLayer.limitrows = true;
            thisLayer.backgroundcolor = new Vec3(0.1, 0.2, 0.3);
            thisLayer.opaquebackground = true;
        }
        export function update() {
            thisLayer.text = 'hello';
            thisLayer.pointsize = 48;
            thisLayer.padding = new Vec2(12, 15);
            thisLayer.alignment = 'center';
        }
    )JS",
                                   true);
    Project project;
    ScriptHost host(scene, project, "text test", "");
    Values values;
    values.cells = &host.cells();
    host.start(values, {});
    const auto& t = host.scene().texts[0];
    CHECK(t.pointsize == 48);
    CHECK(t.font == "systemfont_verdana");
    CHECK(t.padding[0] == 12 && t.padding[1] == 15);
    CHECK(t.horizontal_align == "left" && t.vertical_align == "top");
    CHECK(t.limit_width && t.max_width == 240);
    CHECK(t.limit_rows && t.max_rows == 3);
    CHECK(t.opaque_background && std::fabs(t.background[2] - 0.3f) < 1e-5f);
    CHECK(host.take_changed_texts() == std::vector<int>{1});
    host.frame(values, {});
    CHECK(host.take_changed_texts().empty());
    const auto version = host.structure_version();
    host.frame(values, {});
    CHECK(host.structure_version() == version);
    CHECK(host.warnings().empty());
}

void test_puppet_playback() {
    auto scene = scene_with_script(R"JS(
        let a, step = 0;
        function check(v) { if (!v) throw new Error('playback check ' + step); }
        export function init() {
            a = thisLayer.getAnimationLayer('blink');
            check(a !== undefined && a.fps === 60 && a.frameCount === 120);
            check(a.duration === 2 && a.name === 'blink');
            check(thisLayer.getAnimationLayer('missing') === undefined);
            a.pause();
            a.setFrame(30);
            check(a.getFrame() === 30);
        }
        export function update() {
            switch (step++) {
                case 0: check(a.getFrame() === 30); a.play(); break;
                case 1: check(a.getFrame() === 45); a.rate = 2; check(a.getFrame() === 45); break;
                case 2: check(a.getFrame() === 75); a.pause(); break;
                case 3: check(a.getFrame() === 75); a.play(); break;
                case 4: check(a.getFrame() === 105); break;
                case 5: check(a.getFrame() === 120 && !a.isPlaying()); a.play(); check(a.getFrame() === 0); break;
                case 6: check(a.getFrame() === 30); a.stop(); check(a.getFrame() === 0 && !a.isPlaying()); break;
            }
            thisLayer.alpha = step;
        }
    )JS");
    AnimationLayer idle;
    idle.animation = 7;
    idle.name = "idle";
    AnimationLayer blink;
    blink.animation = 8;
    blink.name = "blink";
    scene.images[0].animation_layers = {idle, blink};
    scene.images[0].animation_layers[0].rate = 2;
    Mesh mesh;
    Mesh::Animation a;
    a.id = 7;
    a.fps = 24;
    a.length = 240;
    a.name = "Idle clip";
    mesh.animations.push_back(a);
    a.id = 8;
    a.fps = 60;
    a.length = 120;
    a.name = "Blink clip";
    a.mode = Mesh::Animation::Mode::single;
    mesh.animations.push_back(a);
    Project project;
    ScriptHost host(scene, project, "animation test", "", nullptr,
                    [&](const std::string&) { return &mesh; });
    Values values;
    values.cells = &host.cells();
    host.start(values, {});
    CHECK(host.layer_time(0, 0, 0.5) == 1);
    for (int i = 0; i < 7; ++i) {
        ScriptFrame frame;
        frame.time = (i + 1) * 0.25;
        frame.delta = 0.25;
        host.frame(values, frame);
    }
    CHECK(host.scene().images[0].alpha.at(values, 0) == 7);
    CHECK(host.warnings().empty());
}

void test_media_updates() {
    auto scene = scene_with_script(R"JS(
        export function mediaPropertiesChanged(event) {
            thisLayer.text = event.title + ' / ' + event.artist;
        }
        export function mediaThumbnailChanged(event) {
            thisLayer.alpha = event.hasThumbnail ? event.primaryColor.x : 0;
        }
    )JS",
                                   true);
    Project project;
    ScriptHost host(scene, project, "media test", "");
    Values values;
    values.cells = &host.cells();
    host.start(values, {});
    CHECK(host.uses_media());
    glint::media::StateTracker tracker;
    glint::MediaState state;
    state.source_generation = 1;
    state.available = true;
    state.title = "First song";
    state.artist = "First artist";
    state.properties_version = 1;
    state.thumbnail_version = 1;
    state.has_thumbnail = true;
    state.primary[0] = 0.5f;
    host.media(values, state, tracker.consume(state));
    CHECK(host.scene().texts[0].text.at(values, 0) == "First song / First artist");
    CHECK(host.scene().images[0].alpha.at(values, 0) == 0.5);
    CHECK(host.take_changed_texts() == std::vector<int>{1});
    // A recreated client can send different media with exactly the same local counters.
    state.source_generation = 2;
    state.title = "Second song";
    state.artist = "Second artist";
    state.primary[0] = 0.25f;
    host.media(values, state, tracker.consume(state));
    CHECK(host.scene().texts[0].text.at(values, 0) == "Second song / Second artist");
    CHECK(host.scene().images[0].alpha.at(values, 0) == 0.25);
    state.has_thumbnail = false;
    host.media(values, state, tracker.consume(state));
    CHECK(host.scene().images[0].alpha.at(values, 0) == 0);
    CHECK(host.warnings().empty());
}
}  // namespace

int main() {
    test_text_changes();
    test_puppet_playback();
    test_media_updates();
    std::printf("script regressions: %d failures\n", failures);
    return failures != 0;
}

# Wallpaper Engine formats

What glint has to read, in the order it needs it. From public reverse engineering
(RePKG, we-tex-extract, linux-wallpaperengine, open-wallpaper-engine and its macOS port),
the Steam guides, and our own reading of Workshop files. There is no official spec; every
layout below is verified against real files before it is trusted. All integers little-endian.

Wallpaper Engine's own `assets/` folder (shipped with the Steam app, not with glint) holds
the shared shaders (`shaders/*.vert|frag`, `shaders/effects/*`, `shaders/common*.h`), the
default effects (`effects/*/effect.json`), default materials, textures and fonts. Workshop
items reference them by path. glint finds it under
`.../steamapps/common/wallpaper_engine/assets` or the `assets` setting.

## 1. project.json (M2, also M6 inspect)

One per item, at the root. Keys glint uses:

- `title`, `description`, `preview` (gif/png/jpg), `tags` (array), `contentrating`
  (`Everyone`, `Questionable`, `Mature`), `workshopid`
- `type`: `scene`, `video`, `web`, `application` (never supported), `preset`
- `file`: the entry point. Scene: `scene.json` (inside `scene.pkg` or loose). Video: the
  mp4/webm. Web: the html.
- `general.properties`: object keyed by property name. Each:
  - `type`: `bool`, `slider`, `combo`, `color`, `text` (a label only), `textinput`, `file`,
    `directory`, `scenetexture`, `group` (a heading); entries without `type` are labels too,
    often HTML. The value of `type` is not always lower case (`"Scene"`).
  - `text`: label, often a localisation key `ui_browse_properties_...`; the `localization`
    block maps it per language
  - `value`: default. `color` is `"r g b"` floats 0 to 1. `combo` is one of `options[].value`.
  - `min`, `max`, `step`, `precision` for sliders; `options: [{label, value}]` for combos
  - `order`: sort key
  - `condition`: a JavaScript-like expression over other properties, e.g.
    `"season.value == 'winter' && snow.value"`. glint evaluates a small subset (comparisons,
    `&&`, `||`, `!`, parentheses) and reports `visible`. Comparisons are loose like
    JavaScript's `==`: combo values are strings and are compared with numbers
    (`newproperty6.value == 1` against `"1"`).
- `general.supportsaudioprocessing`: the scene wants the spectrum.
- Built-in properties every scene project has: `schemecolor` (the user's accent colour).

Property values feed the scene as `{"user": "name"}` bindings (see scene.json).

## 2. scene.pkg (M2)

```
u32 len, char[len] magic      "PKGV0001" .. "PKGV0024" seen; version matters little
u32 count
count times:
  u32 len, char[len] path     e.g. "scene.json", "materials/x.json", "x.tex"
  u32 offset                  relative to the end of the table
  u32 size
data                          entries back to back
```

Older items are unpacked (loose files next to `project.json`); both are supported through
one virtual file system: item folder first, then `scene.pkg`, then `assets/`.

## 3. .tex (M2)

```
char[9]  "TEXV0005\0"
char[9]  "TEXI0001\0"
i32 format          0 RGBA8888 (payload may be a whole PNG/JPEG), 1 or 2 DXT3, 4 DXT5,
                    7 DXT1, 8 RG88, 9 R8
i32 flags           bit 0 no interpolation, bit 1 clamp u, bit 2 clamp v, bit 3 sprite sheet
                    (animated), bit 4 LZ4 (older), bit 5 video texture
i32 texWidth, texHeight     padded to 4
i32 imgWidth, imgHeight     real size, crop to this
u32 unknown/hash
char[9]  "TEXB0001\0" .. "TEXB0004\0"
i32 imageCount      usually 1 (more for sprite sheets)
TEXB0003+: i32 freeImageFormat   (payload type for format 0: 13 PNG, 2 JPEG, -1 raw)
TEXB0004: i32 isVideo     (0 in every file seen; 1 means the payload is an mp4)
imageCount times:
  i32 mipmapCount
  mipmapCount times:
    i32 width, height
    TEXB0002+: i32 compression (0 raw, 1 LZ4 block), i32 rawSize
    i32 storedSize
    u8[storedSize]
if sprite sheet:
  char[9] "TEXS0002\0" or "TEXS0003\0"
  i32 frameCount
  TEXS0003: i32 width, height
  frameCount times: i32 imageId, f32 frametime, f32 x, y, width, unused, unused, height
```

- Verified on the 25 textures of the two installed corpus items, all TEXB0004: walking the
  layout above ends exactly at the end of every file. With an embedded format every mip is
  a whole PNG of its own (`rawSize` 0). Masks are R8 with LZ4, the 4K background of
  3426865175 is DXT5 with LZ4 and a stored height padded from 2160 to 2176. `flags` was 2 in
  every file.
- LZ4 is a raw block, not a frame; decompress to `rawSize`.
- DXT1/3/5 upload directly as `VK_FORMAT_BC1_RGBA_UNORM_BLOCK`, `BC2`, `BC3`; mip chain as
  stored. RGBA8 as `R8G8B8A8_UNORM`, RG88 as `R8G8_UNORM`, R8 as `R8_UNORM`. sRGB handling
  follows what WE does (it samples linear; colour management is off).
- Video textures (`flags` bit 5) carry an mp4 in the payload; decode with FFmpeg like a video
  wallpaper.
- Sprite sheets animate by `frametime`; the frame rectangle is in texels of the first image.

## 4. scene.json (M2 onward)

Top level: `general`, `camera`, `objects`, `version`.

`general`: `clearcolor` (`"r g b"`), `clearenabled`, `ambientcolor`, `skylightcolor`,
`orthogonalprojection {width, height}` (2D scenes), `zoom`, `bloom`, `bloomstrength`,
`bloomthreshold`, `bloomhdrfeather`, `bloomhdriterations`, `bloomhdrscatter`,
`bloomhdrstrength`, `camerafade`, `camerapreview`, `cameraparallax`,
`cameraparallaxamount`, `cameraparallaxdelay`, `cameraparallaxmouseinfluence`,
`camerashake`, `camerashakeamplitude`, `camerashakeroughness`, `camerashakespeed`,
`fov`, `nearz`, `farz`, fog (`fogenabled`, `fogcolor`, ...), physics
(`gravitydirection`, `gravitystrength`, `windenabled`, `winddirection`, `windstrength`),
`colorcorrection` block.

`camera`: `center`, `eye`, `up` (`"x y z"`), `paths` (M5), `fade`, `shake`.

`objects[]`: each has `id`, `name`, `origin`, `scale`, `angles` (`"x y z"`), `visible`,
`parallaxdepth`, `size`, and exactly one of:

- `image`: path to a model json (`models/x.json` or `.../assets/...`), plus `effects[]`,
  `color`, `alpha`, `brightness`, `colorblendmode`, `copybackground`, `ledsource`,
  `perspective`, `fullscreen`, `passthrough`, `puppet` (M5), `animationlayers` (M5),
  `solidlayer`, `config` (text objects), `textures` overrides
- `particle`: path to a particle json (M5), `instanceoverride` (count, rate, size,
  lifetime, speed, alpha, colour), `sequencemultiplier`, `controlpoints`
- `sound`: `sound[]` (paths), `volume`, `playbackmode` (`loop`, `random`, `once`),
  `mintime`, `maxtime`, `startsilent` (M3)
- `light`: `color`, `intensity`, `radius`, `light` type (M5)
- `text`: `text`, `font`, `pointsize`, `padding`, alignments, limits (section 12)
- `model`: a `.mdl` (M5, last)

Value sources: any field may be a plain value or `{"value": ..., "user": "propname"}` (bound
to a project property, optionally `"user": {"name": "...", "condition": "..."}`),
`{"value", "script": "..."}` (SceneScript, M5), or `{"value", "animation": ...}` (timeline).

A timeline: `{"c0": [keyframes], "c1": [...], ..., "options": {"fps": 30, "length": 90,
"mode": "single" | "loop" | "mirror"}}`, one channel per component. A keyframe is `{"frame",
"value", "back": {"enabled", "x", "y"}, "front": {...}}`; between two keyframes the value is
a cubic bezier whose handles sit at `x` times the segment's length in frames and `y` in value
(0.5 and 0 give a smooth ease). Over the installed scenes timelines drive effect constants
(about 100), an object's `alpha` (28), `origin` (20), `angles` (11) and `scale` (4); `single`
plays once and holds, `loop` repeats, `mirror` goes back and forth. `user` bindings are what makes `props set` live: glint keeps a table binding -> uniform /
object field / structural (texture variant, visibility) and applies changes without reload.

`version` 1 scenes have objects without `id`; assign them in order.

## 5. Model json (M2)

`models/x.json` (or in assets): `material` (path to material json), `autosize`, `width`,
`height`, `fullscreen`, `passthrough`, `puppet` (path to the puppet mesh, M5). The image
object's `size` is the model's size unless overridden.

## 6. Material json (M2)

```
{"passes": [{
   "shader": "genericimage2",            resolves to assets/shaders/genericimage2.vert|frag
   "blending": "translucent",            normal, translucent, additive, ... (33 modes, M2)
   "cullmode": "nocull", "depthtest": "disabled", "depthwrite": "disabled",
   "textures": ["materials/x", null, "_rt_FullFrameBuffer"],   index = g_TextureN
   "combos": {"VERTICAL": 1},
   "constantshadervalues": {"g_Speed": 1.0, "g_Color": "1 0.5 0"},
   "usershadervalues": {"g_Speed": "speed"},                    property -> uniform
   "constants": {...}}]}
```

Special texture names: `_rt_FullFrameBuffer`, `_rt_imageLayerComposite_<id>_a`,
`_rt_MipMappedFrameBuffer`, `_alias_<name>`, and the effect-local `_rt_...` from `fbos`.
`_rt_imageLayerComposite_<id>_a` is object `<id>` drawn with its material into a target of
its own; scenes keep hidden helper objects for it (3377132665 has a hidden 4x4 "necessary for
layered clouds"). A render target glint does not make reads as transparent black.

A layer samples like the picture it holds: wrapping when the picture's .tex has no clamp
flag. A `scroll` effect moves a repeating picture across the layer's edge.

## 7. Effect json (M2)

`effects/x/effect.json` (own or in assets):

```
{"name": "...", "passes": [{"material": "materials/x.json", "target": "_rt_x",
   "bind": [{"name": "previous", "index": 0}, {"name": "_rt_x", "index": 1}],
   "command": "copy"|"swap"}],
 "fbos": [{"name": "_rt_x", "scale": 2, "format": "rgba16f"}],
 "dependencies": ["shaders/effects/..."],
 "group": "...", "preview": "..."}
```

Image objects list effects as `{"file": "effects/x/effect.json", "id": n, "visible": true,
"passes": [{ "combos", "constantshadervalues", "textures", "id" }]}`, the per-object
overrides. `visible` and overrides can be `user`-bound. Passes, binds and fbos can carry
`conditions` on combos (`{"op": "ge"|"gt"|"le"|"lt", "value": n}` or equality), kept only
when satisfied. Effects render as a chain on the layer: `previous` is the last pass's output,
two ping-pong targets, `fbos` at full/half/quarter scale.

## 8. Shaders (M2)

Wallpaper Engine shaders are GLSL with HLSL habits and a few annotations. Files
`assets/shaders/<name>.vert` and `.frag`, with `#include "common.h"` and the like.

Preprocessor, in order:

1. `#include "file"`: resolved against `shaders/` of the item, then of `assets/`. Included
   text is placed after the last `attribute`/`varying`/`uniform` declaration before `main`
   (and outside an `#if` block), the include line is dropped.
2. `#require <Module>`: a lighting module (`LightingV1`); until M5 it is replaced by a stub
   returning black.
3. `// [COMBO] {"combo": "NAME", "type": "options", "default": 0, "options": {...}}` lines
   declare combos; the material's and object's `combos` override the default. Every combo
   becomes `#define NAME value`, name upper-cased. Vertex and fragment share the set.
4. `uniform T g_Name; // {"material": "key", "label": "...", "default": "1 1 1", "combo":
   "NAME", "require": {...}, "type": "color"}` annotations: `material` is the key in
   `constantshadervalues`/`usershadervalues`; `default` is the fallback; for samplers,
   `combo` says which define to set when the slot is bound, `require`/`requireany` say
   which combos the sampler needs. `g_TextureN` indexes the material's `textures` list;
   `g_TextureNResolution` (`vec4`: tex w, h, real w, h) is filled by glint.
5. `gl_FragColor` -> an `out vec4` and `attribute`/`varying` -> `in`/`out` by stage;
   explicit `layout(location = n)` assigned so glslang emits SPIR-V without automapping
   surprises.
6. A header of HLSL compatibility defines: `float2/3/4 -> vec`, `int2/3/4 -> ivec`,
   `mul(x, y) -> (y * x)`, `lerp -> mix`, `frac -> fract`, `saturate(x) -> clamp(x, 0, 1)`,
   `texSample2D -> texture`, `texSample2DLod -> textureLod`, `atan2 -> atan`,
   `fmod(x, y) -> x - y * trunc(x / y)`, `log10(x) -> log2(x) * 0.30103`, `ddx -> dFdx`,
   `ddy(x) -> dFdy(-x)`, `CAST2/3/4/3X3 -> vec/mat constructors`, `GLSL 1`. Note `max` with
   swapped arguments and `ddy` negated are deliberate quirks of the dialect.
7. Compile with glslang (GLSL 450 input, Vulkan 1.3 target), reflect uniforms into a UBO
   layout glint builds (WE uses loose uniforms; glint packs them into one block per pass).

What glint does, in this order: includes are inlined and the annotations read from the raw
text; glslang's own preprocessor runs with the combo defines and the compatibility macros,
so every `#if` is settled before anything is rewritten; the global declarations of the
result are then taken apart. Non-opaque uniforms of both stages go into one std140 block at
set 0, binding 0 (glint computes the offsets itself), `g_TextureN` goes to binding N + 1,
attributes get locations in order, and varyings one location map by name for both stages.

Found on the corpus:

- Workshop shaders use GLSL keywords as names (`sample`); they are renamed by macro
  (`sample`, `input`, `output`, `filter`, `common`, `partition`, `active`, `buffer`,
  `shared`, `patch`, `resource`).
- The stages may disagree on a varying's width (`vec2 v_TexCoord` written, `vec4` read, and
  the other way round), which Direct3D accepts and Vulkan does not. The fragment stage reads
  the vertex type under another name and converts at the top of `main`: truncated, or padded
  with zeros.
- `#require LightingV1` provides `PerformLighting_V1`, called only under `#if LIGHTING`;
  until lights in M5 the line is dropped.
- Shaders write to their own inputs (HLSL allows it): attributes and varyings arrive under
  another name and are copied into globals of the shader's name at the top of `main`.
- An `#endif` too many (or too few) passes in Wallpaper Engine; glint drops stray ones and
  closes open blocks at the end.
- HLSL's implicit conversions: a scalar into a vector, a vector into a shorter one, bool into
  float, float into int. glint wraps the right-hand side of initialised declarations, plain
  assignments and compound assignments (`x /= v`) in a constructor of the target type (types
  from the stage's globals and each function's parameters and locals), and an `if` condition
  in `bool()`. A parenthesised comparison next to `*` becomes `float(...)`. `max`, `min`,
  `pow` and `mix` get overloads that take a scalar (or an int) on either side of a vector,
  and `%` goes through an overload set so it works on floats (HLSL's fmod).
- Over the 107 scene items installed on the author's machine, 3053 of 3079 passes compile.
  What is left is a long tail: `mix` with other mixed widths, `texture` with the wrong
  coordinate width, a comparison used where a bool is expected, a float into an int local,
  two varyings sharing a location.

Built-in uniforms glint provides: `g_ModelMatrix`, `g_ViewProjectionMatrix`,
`g_ModelViewProjectionMatrix`, `g_ModelViewProjectionMatrixInverse`, `g_EffectTextureProjectionMatrix`,
`g_Time`, `g_Daytime`, `g_Screen` (w, h, aspect), `g_TexelSize`, `g_TextureNResolution`,
`g_PointerPosition`, `g_PointerPositionLast`, `g_PointerState`, `g_ParallaxPosition`,
`g_AudioSpectrum16Left/Right`, `g_AudioSpectrum32Left/Right`, `g_AudioSpectrum64Left/Right`,
`g_Brightness`, `g_UserAlpha`, `g_Alpha`, `g_Color`, `g_Color4`, `g_LightsPosition[]`,
`g_LightsColorPremultiplied[]`, `g_LightAmbientColor`, `g_LightSkylightColor`,
`g_RenderVar0..3`, `g_TextureMipMapInfo`, `g_LayerModelMatrix`.

Shader source hash (after preprocessing, including defines) keys the SPIR-V cache.

HLSL's implicit conversions, as the translator handles them (`hlsl_conversions`): a scalar
into a vector and a wider vector into a narrower one at declarations and assignments
(`vec2 x = vec2(...)`, also with several declarators and for `for` initialisers), a float
into an int the same way, `if (x)` and `x ? a : b` on numbers through `bool()`, `%` on
floats, a float4 texture coordinate or argument where float2 is wanted (`.xy`, by the
declared types of globals, parameters and locals), a global `const` fed by a uniform
(the `const` goes), `log10` and `lerp` with mixed widths as overload sets, and varying
arrays with one location per element.


## 9. Particle json (M5)

`particles/x.json` in the item or assets: `emitter[]` (box/sphere, rate, count, origin,
distance), `initializer[]` (lifetime/size/velocity/colour/alpha random, turbulence),
`operator[]` (movement, angularmovement, alphafade, sizechange, colourchange, turbulence,
vortex, attractor, control point force, oscillate), `renderer[]` (sprite, spritetrail, rope),
`material`, `maxcount`, `starttime`, `children[]`, `animationmode`, `sequencemultiplier`,
`flags` (world space, audio response). Simulation on GPU in compute; control points come
from the object (cursor-following ones from `g_PointerPosition`).

## 10. Puppet, animation, model (M5)

- `puppet` in a model json names a `.mdl` (`models/x_puppet.mdl`): the mesh, its bones and
  their animations. The picture of a puppet is an atlas of the parts; the mesh's rest pose
  is that atlas, and an animation's keyframes put the parts together. Drawn in the rest pose
  a puppet shows its parts scattered (a face off to the side, say).
- Header, MDLV0013 to MDLV0023: `char[9] "MDLVnnnn\0"`, three u32 (`0x01800009 1 1`), the
  material path NUL-terminated, seven u32 of zero, a u32 of flags (`0x0180000f`), u32 byte
  size of the vertices, the vertices at 80 bytes (position 3 floats in picture pixels with y
  up, relative to the object's origin; normal 3; tangent 4; bone indices 4 x u32; weights 4;
  uv 2), u32 byte size of the indices, u16 triangles.
- `MDLS0001` to `MDLS0004`: u32 end offset, u32 bone count, per bone its name
  NUL-terminated (empty in most rigs, "eye", "halo" and the like in Arona's), u32 1, i32 parent
  (-1 for the root), u32 64, the 4x4 rest matrix relative to the parent (column-major), a
  NUL-terminated JSON string with the physics settings (`a`, `lamin`, `lamax`, `rax`, `ray`,
  `raz`, `s`, `tm`, `tp`). More per-bone data follows up to the end offset (further matrices
  and a bone order), which glint does not need: composing the rest matrices down the chain
  lands each bone where its vertices are.
- `MDAT0001`: u32 end offset, u16 count, per attachment u16 bone, the name NUL-terminated,
  a 4x4 matrix relative to that bone.
- `MDLA0001` to `MDLA0006`: u32 end offset, u32 animation count; per animation u32 id (what
  an object's `animationlayers[].animation` names), u32 zero, the name, the mode (`loop`,
  `mirror` or `single`), f32 fps, u32 length in frames, u32 zero, u32 track count; per track
  (one per bone, in bone order) u32 zero, u32 byte size, then length + 1 keyframes of nine
  floats: translation, rotation (radians about x, y, z) and scale, the bone's transform
  relative to its parent. Like an object's `angles`, z turns clockwise on the canvas (so it
  is negated on glint's y-up canvas); with the other sign Yosuga's hair strands bend against
  their curve and their thin triangle strips fold into one-pixel lines. After the tracks a few zero bytes (0, 0, 5, 6, 30, 31 by version)
  and a u32 count of events, each f32 time and a JSON string (`{"frame": 105, "name":
  "changeAnimation"}`). The last track in every file read so far ends exactly at the end
  offset.
- Skinning: vertex' = sum over its four bones of weight x global(pose) x inverse(global(rest))
  x vertex. A bone's global transform takes its joint through the parent's transform but
  keeps its own rotation and scale: the chain does not sum rotations. Yosuga's hair strands
  have six bones at about ten degrees each at the wave's peak; summed, the tip swept across
  the body and the thin triangle strips tore into spikes, which the Workshop preview does not
  show. This is read off the corpus, not a specification. glint does this on the CPU each frame; the posed
  vertices go into a buffer of their own.
- An object's `animationlayers`: `animation` (id), `rate`, `blend`, `visible`, `additive`.
  The first visible layer gives the pose; later ones blend in by their weight, additive ones
  as their difference from the rest pose. An object without a layer that names one of the
  mesh's animations stays in the rest pose.
- An image object with `"parent"` and `"attachment": "name"` hangs from that attachment of
  the parent puppet: its transform is the parent's, then the attachment's place in the rest
  pose (the bone chain's translations plus its own), then the object's own origin. The
  attachment does not yet follow the animated bone.
- Bone physics (spring, gravity, limits from the JSON above) are not simulated.
- `.mdl` (3D models, compiled from obj/fbx): header `MDLV0001`, meshes with vertex
  streams, skeleton (`MDLS`), animations (`MDLA`), morph (`MDMP`), attachments (`MDAT`).
  Layout to be confirmed on real files; last in the plan.

## 11. SceneScript (M5)

Any bound field may carry a script: `{"value": v, "script": "<source>", "scriptproperties":
{...}}`. The source is an ES module (the editor's template starts with `'use strict'` and
`export let __workshopId`). What it exports is called by name (the official
`lib.sceneScript.d.ts`, version 2.8, lists them): `init(value)` once, `update(value)` every
frame, both returning the field's new value (or nothing to leave it), `destroy()`,
`resizeScreen(size)`, `applyUserProperties(changed)` (once at start with every value, then
with what changed), `cursorEnter/Leave/Move/Down/Up/Click(event)`, `animationEvent`, and the
media events `mediaStatusChanged`, `mediaPlaybackChanged`, `mediaPropertiesChanged`,
`mediaThumbnailChanged`, `mediaTimelineChanged`.

What a script sees: `thisLayer` (the layer the field belongs to: `origin`, `angles` in
degrees, `scale`, `parallaxDepth`, `visible`, `alpha`, `color`, `name`, `size`, the text
fields, `getParent()`, `getChildren()`, `getAnimation(name?)`, `getEffect()`,
`getAnimationLayer()`, `getTextureAnimation()`...), `thisObject` (the layer, or the effect
the field is on; `getAnimation()` without a name is the field's own timeline), `thisScene`
(`getLayer(name|index|id)`, `enumerateLayers()`, ...), `engine` (`frametime`, `runtime`,
`timeOfDay`, `canvasSize`, `screenResolution`, `userProperties` with colours as Vec3,
`registerAudioBuffers(16|32|64)` giving `left`/`right`/`average` Float32Arrays,
`setTimeout`/`setInterval` returning a canceller, `openUserShortcut`), `input`
(`cursorWorldPosition` in canvas units y up, `cursorScreenPosition`, `cursorLeftDown`),
`shared` (an empty object all scripts share), `localStorage` (`get`/`set`/`delete`, per
screen or global), `console`, the classes `Vec2`, `Vec3`, `Vec4`, `Mat4`, the modules
`WEMath`, `WEVector`, `WEColor`, and `createScriptProperties()` whose builder
(`addSlider`, `addCheckbox`, `addCombo`, `addText`, `addColor`, `addTask`, `finish`) yields
the script's own settings: the builder's defaults under the object's `scriptproperties`,
whose entries may themselves be bound to project properties (`{"user": ..., "value": ...}`,
nested, with conditions).

Values cross as the scene stores them: vectors are "x y z" strings, so a `Vec3` goes out as
`toString()` and a number returned for a vector fills every component. A field's current
value is what the script last returned, else the timeline's or the property's.

glint (src/wallpaper/scene/script.cpp, the JavaScript side in script_prelude.cpp): one
quickjs-ng runtime and context per scene and output, every script its own module with a
line in front that binds `thisLayer`, `thisObject` and `createScriptProperties`. The
scripts run on the render thread before the frame's draws, and write into `ScriptCell`s
that `Bound::at()` and `resolve()` read through `Values::cells`, so the shared scene
description is never changed and two outputs showing one scene keep their own values.
Fields a script wrote are "live" like timelines: transforms recompute every frame, colours
and alphas patch their uniforms, a changed `visible` rebuilds the plan, a changed text is
set anew. A script that throws is logged once and left alone; a call that runs past 250 ms
is interrupted. `we-inspect <item> --scripts` runs a scene's scripts for two seconds
without a GPU and prints what they wrote. The corpus (1266 scripts in 36 items) loads and
runs clean; `createAnimationLayer` and `setParent` are still refused. Scripts on particle
instance overrides (`rate`, `size`, `colorn`...) apply to the running system without
restarting it; only `count` needs a new one.

Sound objects are layers too: `thisScene.getLayer('song').play()`, `pause()`, `stop()`
(back to the first file), `isPlaying()` and `volume` reach the player; a sound with
`startsilent` waits for its script. Two outputs showing one scene share one player, so
the output that owns the sound is the one whose scripts drive it.

Layers scripts make: `thisScene.createLayer('models/bar.json')` or a configuration object
(`name`, `origin`, `angles`, `scale`, `alignment`, `parent`...; without a model it is a
transform layer). A script from a Workshop asset names its files as the asset did and the
item keeps them under `workshop/<id>/`, so the path is tried there too (`__workshopId`).
`sortLayer(layer, index)` moves it in the draw order, `destroyLayer` takes it out. The host
keeps its own copy of the scene description for this (the shared one stays as parsed), the
new layer's textures are read and its shader variant compiled on the spot, and the plan is
rebuilt. A layer's `alignment` (center, top, bottom, left, right, topleft...) puts its
origin on the quad's centre, an edge or a corner; the visualisers set it on their bars.

## 12. Text objects (M5)

A `text` object in scene.json: `text` (a plain string, a bound property or a script value),
`font`, `pointsize`, `color`, `alpha`, `brightness`, `padding`, `spacing`, `horizontalalign`,
`verticalalign`, `anchor`, `limitwidth`/`maxwidth`, `limitrows`/`maxrows`,
`limituseellipsis`, `blockalign`, `opaquebackground`/`backgroundcolor`, the usual placement
fields (`origin`, `scale`, `angles`, `parent`, `visible`, `parallaxDepth`), effects, and the
font effects `outline`, `blur`, `dropshadow` with their sizes and colours. Defaults when a
field is missing: point size 32, padding 32 on both axes, max width 500, max rows 1, both
alignments `center`.

How Wallpaper Engine lays the text out, as the open-wallpaper-engine-mac project read it out
of the engine (its `TextLayout.swift` cites the addresses) and as the stored block sizes in
the corpus confirm:

- The face is set to `pointsize` points at 300 dpi, so the em is `pointsize * 300 / 72`
  canvas units; each glyph's advance is floored to a whole unit, `spacing.x` is added to
  every advance and `spacing.y` to the line height. A line's width is its glyphs' pixel
  boxes joined with the pen's start.
- Text wraps only with `limitwidth` (at `maxwidth`), `limitrows` keeps the first `maxrows`
  lines, ending in an ellipsis with `limituseellipsis`; `blockalign` justifies the lines the
  wrap broke. Nothing is shrunk to fit or clipped.
- scene.json's `size` plays no part. The lines sit around the origin by the alignments:
  vertically the block from the first ascender to the last baseline is centred on it, or the
  first ascender sits on it (`top`), or the last descender (`bottom`); horizontally the
  block is centred on it, or starts (`left`) or ends (`right`) there.
- The drawn block is the lines' bounds plus `padding` on each side (at most 512), centred on
  those bounds, not on the origin. The stored `size` of the corpus items matches it: the date
  of 3780525249 ("H A M S T E R", 19 pt, padding 32) is 590x159 in the file and 590x160 here.
- Glyphs are rasterised white; `color`, `alpha` and `brightness` tint them as for a picture.
  `opaquebackground` fills the whole block with `backgroundcolor` under them.
- `font` names a file in the item, the assets or another Workshop item
  (`fonts/workshop/<id>/x.otf`, shipped inside the package), or `systemfont_<name>` from a
  table of eight: arial, calibri, cambria, comicsans, consolas, sansserif (Microsoft Sans
  Serif), segoe (Segoe UI), verdana; any other name is Arial.

glint rasterises with FreeType, shaped by HarfBuzz (with a system fallback face for
characters the item's font lacks), at twice the canvas resolution, into the texture
`_text_<id>` drawn by a synthesized image object with `genericimage2`; the block's offset
from the origin goes into the model matrix. `we-inspect <item> --text` writes every text
object's block as a PAM file. Open: the font effects, rasterising at the shown density, and
text driven by scripts or media (shown as the stored value until SceneScript lands).

## 13. Media (M5)

A scene shows what is playing in two ways. Scripts get the media events (section 11):
`mediaPropertiesChanged` with `title`, `artist`, `albumTitle`, `albumArtist`, `contentType`,
`mediaPlaybackChanged` with `state` (0 stopped, 1 playing, 2 paused), `mediaThumbnailChanged`
with `hasThumbnail` and the cover's `primaryColor`, `secondaryColor`, `tertiaryColor`,
`textColor` (one of the others that reads on the primary, else black or white) and
`highContrastColor` (black or white), `mediaTimelineChanged` with `position` and `duration`
in seconds, `mediaStatusChanged` with `enabled`. And an image layer shows the cover through
a material instance:

    "image": "models/solid_instance_model_<hash>.json",
    "instance": {"combos": {...}, "textures": ["<placeholder>"],
                 "usertextures": [{"name": "$mediaThumbnail", "type": "system"}]}

The model is `{"instanced": true, "material": "materials/util/solidlayer_instance.json",
"solidlayer": true}` (genericimage2 over util/white); the instance puts its combos and
textures over the material's first pass, and a `usertextures` entry names a system texture
for that slot: `$mediaThumbnail` is the cover, shown in place of the slot's texture (the
scene's placeholder picture) while a cover is known.

glint reads the players on the session bus (MPRIS, `org.mpris.MediaPlayer2.*`, through
sdbus-c++, polled from the daemon's loop; src/media/mpris.cpp): the one playing, else the
one that played last. Metadata, PlaybackStatus and Position come from the Properties
interface, with `Seeked`; the position runs on from the last reading while playing. The
cover (`mpris:artUrl`, a file or an https URL, which FFmpeg fetches itself) is decoded on a
worker to at most 512 pixels, uploaded once, and shared by every output; the colours are the
most common saturated 4-bit bins of the picture. The client runs only while a shown scene
wants media (a thumbnail slot or a script with media handlers).

## 14. Video textures (M4)

A `.tex` may hold an mp4 (TEXB0004's video field, or bit 5 of the header flags in
TEXB0003 files; a GIF the editor took in becomes one). Nine items of the corpus have them,
often as the whole animated background at 4K. glint writes the payload to
`$XDG_CACHE_HOME/glint/video/<hash>.mp4` once, opens it with the video wallpaper's decoder
on a thread of its own (software, or VA-API with the planes copied out), waits for the
first frame on the loading worker, and each frame takes the picture due at the scene time,
uploads its three planes and converts them with the video pass into an RGBA image that the
scene samples like any texture (`SceneTexture::external`). Two outputs share the decoder
and the picture; the one further along in time drives it.

## 15. Video and web items (M4, M6)

- `type: video`: `file` is an mp4/webm; decode with FFmpeg, no special format.
- `type: web`: `file` is the html; `general.properties` reach the page through
  `window.wallpaperPropertyListener` (`applyUserProperties`, `setPaused`), audio through
  `window.wallpaperRegisterAudioListener(fn)` with the 128-float array (64 left, 64 right),
  cursor as normal DOM events. Requires the web module.

## Reading order for M2

project.json -> pkg -> scene.json general and camera -> objects of kind image -> model json
-> material json -> shader preprocessor -> tex -> effects -> user bindings. Each step has a
unit test on a file from the test corpus and a dump in `tools/we-inspect`.

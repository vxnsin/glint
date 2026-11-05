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
- `text`: `text`, `font`, `pointsize`, `anchor`, `color`, `opacity` (M5)
- `model`: a `.mdl` (M5, last)

Value sources: any field may be a plain value or `{"value": ..., "user": "propname"}` (bound
to a project property, optionally `"user": {"name": "...", "condition": "..."}`),
`{"value", "script": "..."}` (SceneScript, M5), or `{"value", "animation": ...}` (timeline,
M3). `user` bindings are what makes `props set` live: glint keeps a table binding -> uniform /
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

Built-in uniforms glint provides: `g_ModelMatrix`, `g_ViewProjectionMatrix`,
`g_ModelViewProjectionMatrix`, `g_ModelViewProjectionMatrixInverse`, `g_EffectTextureProjectionMatrix`,
`g_Time`, `g_Daytime`, `g_Screen` (w, h, aspect), `g_TexelSize`, `g_TextureNResolution`,
`g_PointerPosition`, `g_PointerPositionLast`, `g_PointerState`, `g_ParallaxPosition`,
`g_AudioSpectrum16Left/Right`, `g_AudioSpectrum32Left/Right`, `g_AudioSpectrum64Left/Right`,
`g_Brightness`, `g_UserAlpha`, `g_Alpha`, `g_Color`, `g_Color4`, `g_LightsPosition[]`,
`g_LightsColorPremultiplied[]`, `g_LightAmbientColor`, `g_LightSkylightColor`,
`g_RenderVar0..3`, `g_TextureMipMapInfo`, `g_LayerModelMatrix`.

Shader source hash (after preprocessing, including defines) keys the SPIR-V cache.

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
  their animations. The picture of a puppet is an atlas of the parts, so drawn as a flat
  quad it shows the parts scattered; the mesh puts them together.
- MDLV0023, read from the three puppets of 3426865175 (all three end their blocks where the
  next begins): `char[9] "MDLV0023"`, three u32 (`0x01800009 1 1`), the material path
  NUL-terminated, seven u32 of zero, a u32 of flags (`0x0180000f`), u32 byte size of the
  vertices, the vertices at 80 bytes (position 3 floats in picture pixels with y up, normal
  3, tangent 4, bone indices 4 x u32, weights 4, uv 2), u32 byte size of the indices, u16
  triangles. Then a block that starts `01 01 00 00 00` (not read yet), `MDLS0004` (the
  skeleton: per bone a parent, a 4x4 rest matrix and a JSON string), `MDAT0001`,
  `MDLA0006` (animations).
- Until bones arrive in M5, glint draws the mesh in its rest pose with the centre of its
  bounding box on the object's origin. On the small puppet of the corpus that is exactly
  where the flat picture sits (one picture pixel per unit); on the two large ones it puts
  the figures together as in the preview. The exact placement from the skeleton is open.
- Object `animationlayers` blend the animations; bone physics (spring, gravity, limits).
- `.mdl` (3D models, compiled from obj/fbx): header `MDLV0001`, meshes with vertex
  streams, skeleton (`MDLS`), animations (`MDLA`), morph (`MDMP`), attachments (`MDAT`).
  Layout to be confirmed on real files; last in the plan.

## 11. SceneScript (M5)

`script` value sources and `scripts/*.js` in the item: ES5-ish JavaScript with an API
(`engine`, `thisLayer`, `thisObject`, `input.cursorWorldPosition`, `shared`, `localStorage`,
`init`/`update(value)`/`destroy`, cursor events `cursorEnter/Leave/Click`). One QuickJS
context per scene on its own thread; values exchanged per frame.

## 12. Video and web items (M4, M6)

- `type: video`: `file` is an mp4/webm; decode with FFmpeg, no special format.
- `type: web`: `file` is the html; `general.properties` reach the page through
  `window.wallpaperPropertyListener` (`applyUserProperties`, `setPaused`), audio through
  `window.wallpaperRegisterAudioListener(fn)` with the 128-float array (64 left, 64 right),
  cursor as normal DOM events. Requires the web module.

## Reading order for M2

project.json -> pkg -> scene.json general and camera -> objects of kind image -> model json
-> material json -> shader preprocessor -> tex -> effects -> user bindings. Each step has a
unit test on a file from the test corpus and a dump in `tools/we-inspect`.

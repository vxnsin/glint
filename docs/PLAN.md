# Plan

Written November 2026, before the first line of engine code. Things will change; change
this file with them.

## What glint should be

One daemon per Wayland session. It opens a layer-shell surface on every output and draws
into it with Vulkan. A shell talks to it over a Unix socket: put this picture, video or
Wallpaper Engine scene on that output, pause, resume, show me the scene's settings, change
one. The daemon keeps its state and restores it on login, so nothing else has to remember
what was on which screen.

It has to be cheap when nothing happens. No timers, no polling. A frame is drawn when the
compositor asks for one and something actually changed. A still picture costs nothing
after the first frame. A covered or locked output costs nothing at all.

It has to be fast when switching. Shaders and textures are cached on disk, loading runs in
the background, and the old wallpaper stays up until the new one is ready. There is never
a bare screen in between.

And it has to run Wallpaper Engine scenes well: their effects, particles, sounds, cursor
reactions, audio-reactive parts, and the settings their authors put into `project.json`,
all changeable while the scene runs.

## Reading linux-wallpaperengine

Before deciding anything I read how the existing engine loads and draws, because that is
what we are replacing and the first goal is to be faster on Luis's machine, not to have
more features. This is the upstream `main` of November 2026.

### Loading

Everything is synchronous on the main thread. The constructor parses every `project.json`,
`setup()` creates the Wayland driver and the audio driver, and `prepareOutputs()` builds
all scene objects, textures and shaders before the first frame. The layer surface maps
with that first frame, so the screen stays empty for the whole load. A playlist switch
even parses `project.json` twice (once to check it, once to load it).

Shaders go through three compilers. `ShaderUnit` preprocesses the Wallpaper Engine dialect
into GLSL 330, `GLSLContext` feeds that to glslang, gets SPIR-V, and runs SPIRV-Cross to
turn it back into GLSL 330, which the GL driver then compiles again. Nothing is written to
disk. The next process does it all again.

Textures: `.tex` files are parsed and LZ4-decompressed on the CPU, DXT data is uploaded
compressed (good), embedded PNG and JPEG are decoded with stb_image and uploaded without
mipmaps. There is a texture cache keyed by file name for the lifetime of the process, with
no eviction.

And every switch is a new process, since there is no IPC. facet's `frame-wallpaper`
papers over that by waiting on `hyprctl layers` until the new process has drawn.

### Drawing

The loop is `while (keepRunning) render()`. The Wayland driver blocks in
`wl_display_dispatch`, then sleeps whatever is left of `1 / maximumFPS`. It does not use
`wl_surface.frame` callbacks, so it draws on its own clock whether or not the compositor
will show the frame, and the compositor's vsync and its own sleep add up.

When a fullscreen window is open it "pauses": `usleep(250)` in a loop, that is 250
microseconds, so about four thousand wakeups per second asking the fullscreen detector.
Paused costs more CPU than drawing.

Per output, `RenderContext::render` does `makeCurrent`, looks the wallpaper up in a
`std::map<std::string>` by output name, renders it, and swaps, also for outputs that show
nothing. One GL context is shared and switched per output.

`CWallpaper::render` draws the scene into a full-size FBO and then copies it to the output
with a fullscreen textured quad; scaling is done through the UVs, and the UV array is
re-uploaded with `glBufferData` for every output every frame.

`CScene::renderFrame` clears colour and depth, ticks the script engine, updates mouse and
parallax, and draws every object in order. Each image calls `getTexture()->update()`.

`CImage::render` is where the per-frame cost is. Every image, every frame: it walks its
parent chain again in `resolveTransform`, rebuilds the model, copy and MVP matrices with
`glm::ortho`, `lookAt` and two full 4x4 inverses, and re-uploads three vertex buffers with
`glBufferData(GL_DYNAMIC_DRAW)`. Then it runs its passes: the material's, one for each
effect pass, a tint pass and a colour-blend pass when needed, ping-ponging between two
full-resolution FBOs per image. Thirty image layers with a couple of effects each is
easily a hundred draws into a hundred full-screen targets.

`CPass::render`, per pass: `glUseProgram`, `glBindFramebuffer` and `glViewport`, blend,
depth and cull state set unconditionally, every uniform uploaded with its own `glUniform*`
whether it changed or not, `glActiveTexture` and `glBindTexture` for every slot, FBOs
resolved by name through `FBOProvider::find` (a string lookup), vertex attributes enabled,
pointed, drawn (six vertices), disabled again. Animated textures scan their frame list
linearly. Puppet images call `glGetAttribLocation` by name and `glGetIntegerv` per draw.

Smaller things: `localtime()` every frame for `g_Daytime`, every playlist scanned every
frame, integer scale only (no fractional scale), no output hotplug (`handleGlobalRemoved`
is a todo), and no occlusion information beyond the fullscreen detector.

### So, in glint

Frames come from `wl_surface.frame` only. A wallpaper says whether it wants another frame;
a still picture says no, a video says yes once per decoded frame, a scene with `g_Time`
says yes within the fps cap. Paused means blocked in `poll` on the Wayland fd and the
socket, zero wakeups.

One Vulkan device, one swapchain per output, one submit per output. The same scene on two
outputs renders once into an offscreen image and is composed per output. When the scene is
the whole screen, which is the normal case, it renders straight into the swapchain image;
the extra copy exists only for span groups and for `fit` and `tile`.

Transforms are computed when something changes (a property, parallax, an animation), not
per frame. One static quad for all images. Per-object data lives in a per-frame buffer
written once. Uniforms sit in uniform buffers: a per-frame block (time, pointer, audio,
screen) written once per frame, a per-pass block written at build time and on property
changes. Pipeline state is baked into `VkPipeline`; there are no per-draw state calls.

The pass list for every image is built at load into a flat list of draws with resolved
handles. Render targets are sized to the layer, not the screen, and pooled across images.
Reads of `_rt_FullFrameBuffer` are a barrier, not a copy. The frame loop touches no
strings.

Shaders are preprocessed once, compiled once with glslang to SPIR-V, and both the SPIR-V
and the `VkPipelineCache` go to `$XDG_CACHE_HOME/glint/`, keyed by a hash of the final
source. The second load of any scene compiles nothing.

Decode and upload happen on a worker through a staging ring. BC data reaches the GPU
untouched, decoded images get mipmaps generated on the GPU, textures are shared across
outputs by content hash and evicted by budget. The whole load runs on a worker while the
old wallpaper keeps drawing.

One daemon, switches over the socket, caches that survive, fractional scale, outputs that
come and go, fullscreen from foreign-toplevel, and unmapped surfaces simply get no frame
callbacks.

Every milestone is measured against linux-wallpaperengine with the same scenes on the same
machine. The numbers are at the end of this file.

## Other engines

open-wallpaper-engine (the waywallen org, grown out of the KDE plugin's scene backend;
C++, Vulkan 1.1, CEF for web, GPL-2) shows that a Vulkan scene renderer with GPU particles,
puppets, 3D models and SceneScript is doable. It is a plugin for waywallen, not a daemon.
waywallen itself (Rust and C++, MIT) is the current general Wayland wallpaper manager with
Vulkan and VA-API video and a plugin model, but no scene renderer of its own. The macOS
port's wiki has the clearest description of the scene pipeline I found: value sources,
the effect graph, the 33 blend modes, texture variants.

awww, which used to be swww (Rust, GPL-3), is a good model for the daemon and client split
and for transitions; it draws with CPU buffers and has no video. mpvpaper (C, GPL-3) does
video on layer-shell with libmpv and has sensible auto-pause rules. hyprpaper (C++, BSD-3)
shows how little a shell needs for the still-picture case.

glint is MIT, so no code comes from the GPL projects. Formats, shader quirks and behaviour
are reimplemented from documentation and from reading real files. Wallpaper Engine's own
`assets` folder (shaders, default effects and textures) stays in the Steam install and is
read at runtime, never shipped.

## Stack

C++23 with CMake and Ninja. Not Rust, and the reasons, roughly in order:

Everything glint talks to is a C or C++ library: Vulkan, wayland-client, PipeWire, FFmpeg,
glslang, later libplacebo and CEF or WPE WebKit. In C++ that is a call. In Rust each one is
a binding crate of varying quality, and this renderer is mostly about those libraries.

The three Wallpaper Engine renderers for Linux are C++. Their issues and notes carry over
one to one.

The toolchain is already there on CachyOS for Hyprland and Quickshell. A Rust toolchain is
another 1.5 GB plus the `target` folders, and disk is watched.

Hyprland, hyprpaper, hyprlock and Quickshell are C++. Same conventions, same
`wayland-scanner`.

What that gives up is the borrow checker. In exchange: RAII wrappers for every Vulkan and
Wayland handle, no raw `new`, `std::span` and `std::string_view` at the boundaries,
warnings as errors, ASan and UBSan in the debug preset, clang-tidy.

Rust was a real option. awww shows a Rust layer-shell daemon is pleasant, and `ash` and
`smithay-client-toolkit` are solid. It lost on the first and third point.

### Graphics

Vulkan 1.3 with dynamic rendering, `volk` for loading, VMA for memory. No render graph
library; a small pass list per wallpaper is enough.

One `VkSurfaceKHR` per output from the layer-shell `wl_surface`. FIFO swapchains. Frames
driven by frame callbacks, never by a timer.

Wallpaper Engine shaders are preprocessed (includes, combos, the HLSL compatibility
defines, see FORMATS.md) and compiled with glslang straight to SPIR-V. No SPIRV-Cross round
trip. glint's own shaders (the picture pass) are compiled at build time with glslangValidator
and embedded. Pipelines are created through a `VkPipelineCache` kept on disk.

Uploads in M1 run on the graphics queue: the mip chain is blitted on the GPU, which needs
graphics anyway, and one queue orders an upload before the first draw that uses it. A
separate transfer queue comes with video, where uploads never stop. Pictures get dedicated
allocations so their memory goes back when they go; the staging ring exists only while
something loads.

`.tex` payloads in BC1, BC2 and BC3 upload as they are. RGBA8, RG8 and R8 too. Embedded
PNG and JPEG go through FFmpeg, and so do still pictures and video frames, so there is one
image decoder in the tree. Video hardware decode is VA-API first (the AMD box), frames
imported as dma-buf; NVDEC through `nvidia-vaapi-driver` or Vulkan Video later. Software
decode is the fallback and comes first.

NVIDIA gets its own milestone. Explicit sync is the driver's business but needs testing,
and memory import behaves differently.

### Wayland

`wlr-layer-shell-unstable-v1` on the background layer, anchored to all edges, exclusive
zone -1, no keyboard. One surface per output. `wp-fractional-scale-v1` and `wp-viewporter`
so a 1.5x screen renders at its real pixel size. Hyprland sends the preferred scale only for
a mapped surface, so until it arrives the scale is the output's mode size over the logical
size of the surface (which covers the whole output). `wlr-foreign-toplevel-management` to see
fullscreen windows per output (Hyprland, sway, river, niri all have it), bound only while
something on screen animates: the protocol reports every title change of every window, and
that alone woke an idle M1 daemon about once a second. The surface has an empty input region
until a wallpaper wants the cursor, for the same reason. The GPU is the one the compositor
renders with, from `linux-dmabuf` feedback matched against `VK_EXT_physical_device_drm`. On Hyprland the
cursor position comes from its socket even when a window is under the pointer; elsewhere
from `wl_pointer` on the layer surface. The lock screen is not ours: the shell sends
`pause` with reason `locked`.

### Audio

PipeWire's native API, no PulseAudio shim. Playback: one stream per output that has a
sound object, mixed in glint, volume and mute per output. Capture: one stream on the
default sink's monitor for audio-reactive scenes, FFT with a small vendored routine (no
FFTW), binned to Wallpaper Engine's 64 bands per channel and smoothed the way it expects.
The capture only runs while a loaded scene asks for it.

### IPC

A Unix socket with JSON lines, request and reply plus subscriptions. The whole protocol is
in IPC.md. Not D-Bus, because Quickshell's `Socket` speaks this directly, every script can
use it with `socat`, and it works without a session bus. A D-Bus bridge can be a separate
small process later if a shell wants one.

The same binary is daemon and client: `glint` alone runs the daemon, `glint set ...` talks
to it. State goes to `$XDG_STATE_HOME/glint/state.json`.

### The CLI and the app

The CLI has two faces. Scripted, every command takes its arguments and prints text or
JSON, as in IPC.md. Interactive, when run in a terminal with arguments missing, it asks:
`glint set` lists the outputs, then the kind (pictures and videos from the wallpaper
folders, Wallpaper Engine items from the Workshop folder with title and tags, filterable
by typing), then applies. `glint props` shows the current scene's settings as a menu with
toggles, sliders and choices, and changes them live. The menus are a few hundred lines of
our own raw-terminal code (arrow keys, filter, enter), no TUI library, so the binary stays
dependency-light. `glint install` is the setup: it checks the compositor, Vulkan, PipeWire
and the Steam paths (the same checks as `glint doctor`), writes the state file, and adds
the `exec-once` line or the systemd user unit if asked.

`glint ui` starts the app. It is a separate program, `glint-app`, in `apps/` of this repo
with its own CMake target, so it can be installed and packaged on its own; the daemon does
not depend on it. Qt 6 and QML, because that is what the author knows from Quickshell and
what is already installed on a Quickshell desktop. It shows the library (folders and
Workshop items with their previews), a per-output assignment, the scene's settings panel
built from `props`, and the daemon's status. For the Workshop it does what Wallpaper
Engine itself does through the Steam client: browsing and subscribing happen in Steam
(`steam://openurl/...` to the item page, so subscribing is one click there), Steam
downloads the item into the Workshop folder, and glint-app watches that folder and shows
the new item as soon as it lands. Item details that need no login come from Steam's public
`GetPublishedFileDetails`. No embedded browser, no Steam API key, no scraping.

facet keeps its own wallpaper app in Quickshell; it talks to the same socket. glint-app is
for everyone else and for the Workshop and settings parts.

### Web wallpapers

Later, optional. CEF is what the other engines use: about 300 MB, offscreen rendering into
a shared texture, best compatibility. WPE WebKit (`wpewebkit`, `wpebackend-fdo`) is far
smaller and exports frames as dma-buf, made for exactly this. Try WPE first.

### SceneScript

Scenes can carry JavaScript. QuickJS-ng is small and is the plan, in milestone 5. Until
then scripts are ignored and the static value is used.

## Dependencies

Arch and CachyOS package names. Most are on a Hyprland desktop already.

Required: vulkan-headers, vulkan-icd-loader, volk, wayland, wayland-protocols,
wlr-protocols, glslang, nlohmann-json, lz4, ffmpeg, pipewire, cmake, ninja, a C++23
compiler (GCC 14 or Clang 18 and later). Vulkan Memory Allocator has no Arch package, only
an AUR one, so its single header is in `third_party/vma`. For the measurements in
`docs/LOG.md`: perf.

Later and optional: libva (hardware video), freetype, fontconfig and harfbuzz (text
objects), quickjs-ng (SceneScript), wpewebkit and wpebackend-fdo or CEF (web), qt6-base and
qt6-declarative (glint-app).

On the CachyOS box with a Hyprland desktop, glslang, ffmpeg, pipewire, wayland and the
compiler were already there. The new packages were about 175 MB installed, cmake (100 MB)
and vulkan-headers (40 MB) being the big ones. The build directory is 100 to 200 MB in
debug, the binary a few MB. Nothing is bundled except tiny single-file pieces,
listed in `third_party/README.md`.

## Layout

```
src/
  main.cpp            daemon or client, by arguments
  core/               log, event loop (epoll), config, state file, clock
  wayland/            registry, outputs, layer surfaces, pointer, foreign-toplevel, scale;
                      the protocol code is generated by cmake/Dependencies.cmake
  gpu/                instance, device, per-output swapchain, VMA, pipeline cache, uploads
  ipc/                socket server, protocol, client used by the CLI
  cli/                argument handling, the interactive menus, install and doctor
  audio/              PipeWire playback mixer, capture and spectrum
  input/              cursor sources (wl_pointer, hyprland socket), unified cursor state
  wallpaper/          Wallpaper interface, loader (picks a kind from a path), properties
    image/            still pictures
    video/            FFmpeg decode, hardware import, frame pacing
    scene/            Wallpaper Engine scenes
      pkg, tex        archive and texture readers
      project         project.json, properties, conditions
      scene           scene.json: general, camera, objects, value sources
      material, effect
      shader/         preprocessor, glslang, SPIR-V cache, reflection
      objects/        image, particle, text, sound, light, model
      script/         SceneScript on QuickJS, later
    web/              later, optional
apps/glint-app/       the Qt app, own target
third_party/vma/      vk_mem_alloc.h, the only vendored file
tests/                readers, preprocessor, property conditions
tools/                we-inspect: dumps a project, scene, pkg or tex
```

Every wallpaper kind implements one interface: load (on a worker, the old one keeps
drawing until ready), resize, render (returns whether it drew anything), whether it wants
another frame, pause and resume, properties (list, get, set), cursor, and an audio
spectrum feed for scenes. It is in `src/wallpaper/wallpaper.hpp`.

## Milestones

Renderer and scenes first, measured against linux-wallpaperengine on Luis's machine; video
and the long tail of scene features after. Each milestone ends in something that runs on
the CachyOS box and a note in `docs/LOG.md` with the numbers: idle CPU, GPU busy, frame
time, switch time, memory. The issues on GitHub carry the details.

M0: CMake, the module layout, a `main` that prints its version. Written on Windows and first
compiled on the CachyOS box at the start of M1.

M1, the renderer: Wayland with outputs, hotplug, layer surfaces, fractional scale,
pointer, foreign-toplevel. Vulkan with device pick, one swapchain per output, dynamic
rendering, VMA, staging, per-frame buffer, pipeline cache, timestamp queries. The frame
loop on `poll`, pause reasons that stack, the covered case proven. The image kind with
FFmpeg decode on a worker, mipmaps on the GPU, scaling modes, crossfade. IPC and CLI with
outputs, set, unset, status, pause, resume, subscribe, state file, restore on start. Done
when idle is 0 % CPU and 0 % GPU, paused has no wakeups, a picture switch takes under 100
ms, and two outputs with different scales look right. Done on 2026-11-05;
the numbers and what was left out are in `docs/LOG.md`.

M2, scenes: the readers (project.json, pkg, tex, model, material and effect JSON, assets
lookup), the shader pipeline with cache, the scene graph with image objects and transforms
computed on change, the pass list built at load (material and effect passes, fbos, bind,
previous, combos, conditions, the 33 blend modes, masks, tint and colour-blend passes),
properties over IPC driving uniforms and combos live, the per-frame block. Done when five
Workshop scenes Luis picks render like in Wallpaper Engine, load from cache under a
second, switch without a bare screen, and cost less GPU and CPU time per frame than the
old engine. Built on 2026-11-05 except for the three corpus scenes
not installed yet; what was measured and left out is in `docs/LOG.md`.

M3, interaction and audio: cursor from `wl_pointer` and Hyprland's socket,
`g_PointerPositionLast`, `g_PointerState`, mouse-delay effects, per-layer parallax; sound
objects over PipeWire with per-output volume; capture, FFT and `g_AudioSpectrum*`;
timelines and animated properties. Plus the interactive CLI menus, since they need scenes
and properties to be useful.

M4, video: FFmpeg on a worker, pacing to the video's rate under the output's cap, loop,
speed, pause, audio over the M3 path, then VA-API with dma-buf import, measured against
software decode. Video textures inside scenes use the same decoder. Replaces mpvpaper.

M5, the rest of scenes: particles as compute shaders, text objects with FreeType and
HarfBuzz, puppet warp, lights, camera paths and shake, bloom, SceneScript on QuickJS-ng,
and `.mdl` models last.

M6, the platform: the NVIDIA pass, web wallpapers as an optional module, a D-Bus bridge if
asked for, the power policy and window rules from `src/policy` wired to UPower and the
toplevels (battery fps cap, idle after n minutes, free after n minutes covered, pause and
mute rules by app_id or title), `glint inspect` for
library UIs, `glint doctor` and `glint install`.

M7, the app: glint-app with library, per-output assignment, the settings panel, status,
and the Workshop flow through Steam.

## Numbers to beat

Measured on Luis's machine with the same Workshop scenes running in linux-wallpaperengine
next to it. Idle: 0 % CPU, 0 % GPU, zero wakeups when paused. Switch: picture to picture
under 100 ms, scene from cache under 1 s. Per frame: less GPU time and less CPU time than
the old engine for every scene in the corpus. Its frame carries the full FBO copy, the
per-image buffer uploads and one full-resolution ping-pong pass per effect pass; ours does
not, so this should hold by a wide margin. GPU time comes from Vulkan timestamp queries,
CPU time from the frame loop, both shown in `glint status`.

## Decided

MIT, nothing borrowed from the GPL engines. WPE WebKit before CEF, in M6. SceneScript in
M5, not earlier. The shell and glint-app own the library, the daemon only renders and
inspects. Only AMD hardware for now, so M6 stays last.

The test corpus for M2, all rated Everyone: Roxy on a sunset street (3780525249, anime,
layers and effects, properties), Frieren - Pagoda of flowers (3426865175, 4K, audio
responsive, mouse parallax, puppet warp, the heavy one), Astronaut (1705122927, the
classic audio-reactive scene), Forest River (1139304621, sound objects and particles),
Aurora Lake (1275921440, landscape effects). Three-Body problem (3509243656, 3D, scripts,
HDR) is the stretch goal for M5.

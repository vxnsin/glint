# Log

Measurements per milestone, on Luis's machine. Numbers, how they were taken, and what was
left out.

## M1, the renderer (2026-11-05)

### Setup

CachyOS, kernel 7.3.1, Hyprland 0.57.0, Mesa 26.2.5 (RADV), FFmpeg 9.0.2. An RX 7800 XT
(Navi 32) drives both outputs; the Ryzen 7 7700X's integrated GPU is unused and glint picks
the discrete one through the compositor's linux-dmabuf main device. DP-2 is 2560x1440 at 60
Hz, HDMI-A-1 1920x1080 at 60 Hz, both at scale 1 unless noted. glint is the release preset
(GCC 16.2.1, -O3).

How the numbers were taken:

- CPU: utime plus stime of the process from `/proc/<pid>/stat`, in ticks of 10 ms.
- Wakeups: voluntary plus involuntary context switches, summed over every thread of the
  process (`/proc/<pid>/task/*/status`).
- GPU: `drm-engine-gfx` and `drm-memory-vram` from the process's DRM fdinfo.
- Switch time: `load_ms` from the `set` reply, the time from the request to the first
  presented frame that shows the new picture on every addressed output.

Test pictures: two frames taken from Workshop videos as 2560x1440 JPEG (130 and 180 KB), one
of them as a 3840x2160 PNG (3.3 MB) and as a 3840x2160 JPEG, and a 1440x2560 portrait JPEG.

### Idle

| Case | Window | CPU ticks | Context switches | GPU busy | VRAM |
|---|---|---|---|---|---|
| Picture on both outputs | 60 s | 0 | 0 | 0 ms | 96 MiB |
| Paused (`locked`) | 30 s | 0 | 0 | 0 ms | 117 MiB |
| Paused, after a `set` during the pause | 30 s | 0 | 0 | 0 ms | 117 MiB |

The 96 MiB are the six swapchain images, three per output at 4 bytes per pixel (about 69
MiB), the picture with its mip chain (20 MiB), and a few MiB of the driver's own. With no
picture set it is 76 MiB. GTT is 2 MiB and resident memory 78 MiB, most of it shared
libraries. Paused at 117 MiB holds two pictures, the old one and the one set during the
pause, until the next frame can be drawn.

A `set` while paused loads the picture and answers, but draws nothing: the frame counter
stayed at 1988 for 30 seconds. After `resume` came exactly two frames, one per output.

Two things this milestone changed to reach those zeros, both found by measuring:

- Foreign-toplevel woke the idle daemon 14 times in 10 seconds. The compositor sends an event
  for every title change of every window (a terminal with an animated title is enough), and
  glint does not need fullscreen detection for a still picture, because a covered still picture
  draws nothing anyway. glint now binds foreign-toplevel only while something on screen wants
  frames. The layer surface also has an empty input region, so pointer motion over the
  desktop does not wake the daemon.
- The first build held 172 MiB VRAM and 34 MiB GTT with the same pictures: the picture lived in a
  VMA block that stayed when the picture went, and the staging ring stayed mapped. Pictures
  now get dedicated memory and the ring is freed when nothing loads.

### Covered

With `GLINT_TRACK_TOPLEVELS=1` and a fullscreen kitty on HDMI-A-1: a `set` while the output
was covered loaded and answered after 14.6 ms, the output drew no frame for 5 seconds, and
drew one frame once kitty closed. While tracking is on, the daemon wakes for every toplevel
event: 22 to 52 context switches in 20 seconds on this desktop.

A fullscreen window counts only while it has focus. Spotify fullscreen in Hyprland's special
workspace kept DP-2 paused while the desktop was in view, because the protocol does not say
whether a workspace is visible. With focus required, the test above also showed focus moving
away from kitty once (focus follows the mouse here), which lifted the pause for a moment.
Missing a covered output is cheap: Hyprland sends no frame callbacks to a background surface
it does not draw. During a crossfade on HDMI-A-1 behind Discord, the output drew 3 frames
instead of about 18.

### Switching

| Picture | Transition | n | Min | Median | Max |
|---|---|---|---|---|---|
| 2560x1440 JPEG on both outputs | fade | 20 | 12.8 ms | 13.5 ms | 15.2 ms |
| 2560x1440 JPEG on both outputs | none | 20 | 12.1 ms | 13.5 ms | 14.5 ms |
| 3840x2160 JPEG on both outputs | none | 10 | 25.4 ms | 26.7 ms | 28.0 ms |
| 3840x2160 PNG on both outputs | none | 10 | 75.6 ms | 77.2 ms | 81.2 ms |
| Already decoded (shown on the other output) | none | 20 | 0.3 ms | 9.9 ms | 14.2 ms |

Every switch decodes on a worker, uploads, builds the mip chain on the GPU, and waits for the
next frame on both outputs; one picture is decoded once for both. The already-decoded case is
mostly the wait for the next vblank. With `fade`, the crossfade of 300 ms runs on after the
reply. The CPU time of a frame is 0.1 to 0.4 ms, the GPU time 0.05 to 0.1 ms.

### Two outputs with different scales

HDMI-A-1 switched to scale 1.5 while glint drew a picture: glint followed within one
configure, the swapchain became 1920x1080 for a logical 1280x720, and DP-2 stayed at 2560x1440 and scale 1. Scale 1.25 gave 1920x1080 for a logical 1536x864.
Back at scale 1, the window layout was unchanged. Started while HDMI-A-1 was at 1.5, glint
reported the right pixel size before its first frame (checked in the log, before drawing
worked): Hyprland sends the preferred fractional scale
only for a mapped surface, so until it arrives glint takes the mode size over the logical size.

### Compared with frame-wallpaper's image backend

frame-wallpaper's image backend is facet's shell itself: Quickshell draws the picture in
`Backdrop.qml`. Its cost is part of the `qs` process, next to everything else the shell
does, so it cannot be measured alone. Each window was 60 seconds with the same 2560x1440
picture, each engine running alone.

| Process and case | CPU | Context switches | GPU busy | VRAM |
|---|---|---|---|---|
| glint, picture on both outputs | 0 % | 0/s | 0 ms | 96 MiB |
| qs, no picture (glint and frame-wallpaper stopped) | 0.57 % | 154/s | 35 ms | 324 MiB |
| qs, image backend with the picture | 1.18 % | 199/s | 48 ms | 463 MiB |
| qs, next to glint | 1.40 % | 323/s | 188 ms | 324 MiB |

The qs rows move with what happens on the desktop (music, notifications, the bar), so the
difference in CPU and wakeups between them is mostly that, not the picture. The VRAM is
clear: the picture in the shell costs qs 139 MiB, against 96 MiB for all of glint with
both outputs.

Switching picture to picture with `frame-wallpaper set` (8 switches): the command returned
after 99 to 113 ms, and the script logged 738 to 766 ms per switch, 600 ms of which is a
fixed wait for Hyprland's layer fade before the old layer goes. When Quickshell shows the new
picture is not logged; that was not measured.

### Done when

- 0 % CPU and 0 % GPU idle: yes, 0 ticks and 0 ms in 60 seconds.
- No wakeups when paused: yes, 0 context switches in 30 seconds, also after a `set`.
- Picture-to-picture switch under 100 ms: yes, 13.5 ms median for 2560x1440, 77 ms for a 4K
  PNG.
- Two outputs with different scales correct: yes, at 1.5 and 1.25 next to 1.

### The five scenes

| Scene | Load from disk cache | GPU per frame | GPU memory | Against its preview |
|---|---|---|---|---|
| Roxy (3780525249) | 81 ms | 0.21 ms | 83 MB | as in the preview |
| Frieren (3426865175) | 130 ms | 1.32 ms | 283 MB | figures complete, no particles |
| Astronaut (2185197772) | 328 ms | 2.43 ms | 475 MB | as in the preview |
| Jane Doe (3337889865) | 102 ms | 0.96 ms | 227 MB | as in the preview |
| Yosuga no Sora (2857144643) | 31 ms | 0.98 ms | 198 MB | background right, figure in its rest pose |

All at 2560x1440 and 60 fps, loaded by a running daemon after another scene; GPU memory is
`memory_mb` of `glint status`, the scene's textures and its targets on this output.

The Astronaut first drew only its clear colour: its layers sit at z = -100, and the scene
projection's depth range was -1 to 1. It is -10000 to 10000 now, Wallpaper Engine's `farz`.

Yosuga no Sora is a rig: 18 puppets hang from attachment points on each other's skeletons.
The attachments are read (FORMATS.md, 10) and place each part on its parent, but the figure
sits on the suitcase only through its animation, which glint does not play until M5; in the
rest pose the parts point the wrong way. Its puppet meshes also settled where puppets are
placed: the mesh positions are relative to the object's origin as they are. The first M2
build centred each mesh on its bounding box, which on Frieren cut Himmel's head off and
Frieren's legs; both are whole now.

### Shaders across the whole Workshop folder

`we-inspect <item> --shaders` over all 107 scene items installed here: 3079 passes, 2996
compile (97.3 %), 83 fail in 25 items, and one item does not get through the reader.

After the dialect work of the same day (writable stage inputs, unbalanced `#endif`, HLSL's
implicit conversions, FORMATS.md 8): 3031 compile (98.4 %), 48 fail. No scene compiles
fewer passes than before any of it, and the five test scenes render the same mean colour as
before (Frieren off by one step in one channel: its water moves between the two shots).

### Left out or different from the plan

- Uploads run on the graphics queue, not a separate transfer queue. The mip chain needs
  graphics anyway, and one queue orders the upload before the first draw for free. The
  transfer queue comes with video in M4, where uploads are continuous.
- The built-in shaders are compiled at build time with glslangValidator and embedded. The
  runtime glslang path with a SPIR-V cache on disk comes with the scene shaders in M2. The
  pipeline cache is on disk (2.3 KB in `$XDG_CACHE_HOME/glint/pipelines.bin`).
- EXIF orientation of JPEGs is not applied.
- A video file is shown as its first frame; video playback is M4.
- A crossfade on an output that gets no frame callbacks (covered) waits for the next one and
  holds both pictures until then.
- The pointer is wired up but switched off (empty input region) until a wallpaper wants it
  in M3.
- Hotplug was tested with a headless output before drawing worked (surfaces come and go in
  the log); with pictures on screen only through the restart and scale paths. Not tested on
  sway, river or niri.
- In the sanitizer build, LeakSanitizer reports 128 bytes from inside the Vulkan driver at
  exit, none from glint.

## M2, scenes (2026-11-05)

### Setup

As for M1. Two corpus items are installed, Roxy on a sunset street (3780525249) and
Frieren - Pagoda of flowers (3426865175). Astronaut, Forest River and Aurora Lake are not
subscribed; Luis chose to take three of the installed scenes in their place, all rated
Everyone: Astronaut [7680x2160] [Audio responsive] (2185197772, the same artwork at 32:9,
14 layers and 100 effects), Jane Doe from Zenless Zone Zero (3337889865, 3 layers and 13
effects, nothing in it waits for a later milestone) and Yosuga no Sora (2857144643, 27
layers, 26 effects, 18 puppets, nothing waiting for a later milestone either). Each engine ran alone on
DP-2 (2560x1440) at 60 fps, muted, without its fullscreen pause; HDMI-A-1 showed glint's
background colour, or nothing for linux-wallpaperengine. Same measures as in M1 over 30
seconds, after 5 seconds of settling.

### Against linux-wallpaperengine

| Scene | Engine | CPU | Context switches | GPU busy | VRAM |
|---|---|---|---|---|---|
| Frieren | glint | 1.07 % | 64/s | 9.5 % (1.32 ms per frame) | 362 MiB |
| Frieren | linux-wallpaperengine | 4.20 % | 565/s | 62.6 % | 2086 MiB |
| Roxy | glint | 0.70 % | 64/s | 1.4 % (0.22 ms per frame) | 160 MiB |
| Roxy | linux-wallpaperengine | crashed at load | | | |

linux-wallpaperengine stops on Roxy with a JSON type error from its own parser
(`type must be number, but is string`); glint draws it.

The Frieren comparison is not like for like: linux-wallpaperengine also draws the scene's
eight particle systems and three text objects, which glint skips until M5, and its puppets
move, while glint's stand in their rest pose. The GPU time per frame comes from glint's
timestamp queries; its CPU time per frame is 0.1 to 0.2 ms. The 64 context switches a second
are the 60 frames.

### Loading and switching

A scene loaded by a fresh daemon, with the SPIR-V and pipeline caches on disk: Roxy 153 ms,
Frieren 214 ms from the request to the first frame on screen, of which reading the package,
decoding the textures (four large PNGs and a 4K DXT5 for Frieren) and reading the cached
shaders on the worker take 152 and 210 ms. The first load ever also compiles the shaders
(14 for Frieren) and took 138 ms on the worker in that run, so compiling is not what the
load waits for; the spread between runs (138 to 210 ms) is the texture decoding. linux-wallpaperengine showed its layer 608 ms after start on Frieren.

A switch keeps the old wallpaper on screen until the new scene is uploaded, so there is no
bare frame in between; a second output showing the same scene loads nothing again.

### Correctness

Checked with `glint screenshot` against the items' previews: Roxy draws as in the preview;
Frieren draws the backdrop, flowers, the colour filter over the whole frame and both figures.
The figures are puppets: their texture is an atlas of parts, and drawn flat it showed the
parts scattered. glint now draws the puppet's mesh in its rest pose, which puts each figure
together; it does not move. The other three scenes are below. Setting `xme` off over `glint props` hides Himmel at once, and
the conditions of the other settings follow.

### The five scenes

| Scene | Load from disk cache | GPU per frame | GPU memory | Against its preview |
|---|---|---|---|---|
| Roxy (3780525249) | 81 ms | 0.21 ms | 83 MB | as in the preview |
| Frieren (3426865175) | 130 ms | 1.32 ms | 283 MB | figures complete, no particles |
| Astronaut (2185197772) | 328 ms | 2.43 ms | 475 MB | as in the preview |
| Jane Doe (3337889865) | 102 ms | 0.96 ms | 227 MB | as in the preview |
| Yosuga no Sora (2857144643) | 31 ms | 0.98 ms | 198 MB | background right, figure in its rest pose |

All at 2560x1440 and 60 fps, loaded by a running daemon after another scene; GPU memory is
`memory_mb` of `glint status`, the scene's textures and its targets on this output.

The Astronaut first drew only its clear colour: its layers sit at z = -100, and the scene
projection's depth range was -1 to 1. It is -10000 to 10000 now, Wallpaper Engine's `farz`.

Yosuga no Sora is a rig: 18 puppets hang from attachment points on each other's skeletons.
The attachments are read (FORMATS.md, 10) and place each part on its parent, but the figure
sits on the suitcase only through its animation, which glint does not play until M5; in the
rest pose the parts point the wrong way. Its puppet meshes also settled where puppets are
placed: the mesh positions are relative to the object's origin as they are. The first M2
build centred each mesh on its bounding box, which on Frieren cut Himmel's head off and
Frieren's legs; both are whole now.

### Shaders across the whole Workshop folder

`we-inspect <item> --shaders` over all 107 scene items installed here: 3079 passes, 2996
compile (97.3 %), 83 fail in 25 items, and one item does not get through the reader.

After the dialect work of the same day (writable stage inputs, unbalanced `#endif`, HLSL's
implicit conversions, FORMATS.md 8): 3031 compile (98.4 %), 48 fail. No scene compiles
fewer passes than before any of it, and the five test scenes render the same mean colour as
before (Frieren off by one step in one channel: its water moves between the two shots).

### Left out or different from the plan

- Particles, text, sound, lights, puppet animation, the camera shake and bloom: M3 and M5.
  Values animated by timelines take their static value.
- Puppets stand in their rest pose; rigs posed by an animation look wrong until M5.
- Objects marked `perspective` are drawn flat with the scene's orthographic camera.
- Copybackground, effect commands (`copy`, `swap`) and the 33 colour blend modes that read
  `_rt_FullFrameBuffer` through the BLENDMODE combo are not done; nothing in the two installed
  scenes uses them. The fixed blendings (normal, translucent, additive) are.
- Layers are rendered at the output's scale, not at the canvas size, which saves memory on a
  smaller screen and changes effects that count in texels slightly.
- A property change rebuilds the plan and the uniform blocks; a combo bound to a property
  would need a recompile, which is not done (none in the corpus).
- The parallax follows the pointer with the scene's delay; its strength is glint's guess
  (FORMATS.md has no number for it) and has not been compared with Wallpaper Engine.

## M3, interaction, sound and audio (2026-11-05)

### Audio-reactive scenes

The capture is a PipeWire stream on the monitor of the default output, analysed into 64 bands
per channel (src/audio/spectrum.cpp) and handed to `g_AudioSpectrum16/32/64Left/Right`. Fed
through a null sink (`GLINT_AUDIO_TARGET`) with a 110 Hz tone and with pink noise, the
Astronaut's jellyfish glows with the sound and settles in silence; `glint status` showed a
peak of 0.048 for the tone at an eighth of full scale and 0.036 for the noise. How far
Wallpaper Engine scales the bands is not documented, and the levels have not been compared
with it on music yet.

The stream exists only while a shown, running wallpaper wants the spectrum: with Roxy or Jane
Doe on screen `status` reports no capture and glint has no PipeWire node; a paused output
stops it.

### Cursor

On Hyprland glint reads `cursorpos` from the request socket once per drawn frame while a
wallpaper wants the cursor, so it follows the pointer over windows. Moving the cursor across
DP-2 with Hyprland's Lua dispatcher (`hl.dsp.cursor.move`) moved the Astronaut's layers about
190 output pixels against it, from the brightness centroid of screenshots taken at the left
edge, the middle and the right edge. A scene that follows the cursor draws every frame.

### Sound objects

Frieren's music played into a null sink at volume 0.5 and was recorded from its monitor
(peak 0.13 a few seconds in); silent while the output was paused, playing again on resume.
Ten seconds paused with the music loaded: no context switches on any of the daemon's 13
threads (the stream is inactive and the decoder waits without a timeout). Scenes are silent
unless `volume` is set, so a login does not start music.

### Timelines

Effect constants, an object's alpha, origin, angles and scale follow their timelines (cubic
bezier keyframes; single, loop and mirror). On Floating Ducks (3377132665) the fish, ducks and
their legs move. The same scene showed two renderer faults that are fixed: a scrolling
picture's layer clamped its edge into long bars (layers now wrap like their picture), and
`_rt_imageLayerComposite_<id>_a` read as white (it is now the object drawn into a target of
its own, which brings the scene's water back).

### Left out

- `g_PointerState` (buttons) is 0; clicks need pointer input on the surface, which glint keeps
  off on Hyprland.
- Per-output volume over the `audio` op; the volume is the `set` option.
- Sound objects that start silent wait for SceneScript (M5).
- The interactive CLI menus (#49) are not built yet.

## Validation, target sharing and the fps cap (2026-11-05)

With `vulkan-validation-layers` installed, `GLINT_VALIDATION=1` and synchronization
validation on (`khronos_validation.validate_sync = true` in a `vk_layer_settings.txt`), a sweep
over a picture, the 4K video, six scenes, a property change, a crossfade and covering an
output reported four kinds of error and no hazard. All four are fixed: a scene's zero
vertex buffer was destroyed while a frame could still read it; the swapchain and its
semaphores were destroyed while the queue was still presenting (teardown and resize now wait
for the queue); a fragment stage's varying with no vertex output became an undefined
pipeline input (it is now a zero global). A second sweep after the fixes is clean.

Scene targets now share images: a target's image goes to the next target of the same size
once the frame is done with it, composites are only planned for readers that are drawn, and
`_rt_imageLayerComposite_<id>_b` is `_a` under another name. Astronaut dropped from 843 to
631 MiB on both outputs, Frieren from 452 to 404; the other scenes were unchanged, since each
of their objects has a layer of its own size. Screenshots before and after match.

The `fps` option caps the frames drawn: Hyprland's frame callback follows a commit within a
few milliseconds and says nothing about the vblank, so the cap runs on a timerfd with a grid
of the period. Counted over ten seconds on Frieren: 15 gives 150 frames, 24 gives 240, 20
gives 201, 30 gives 301, 45 gives 451. At 10 fps the daemon sits at 1.8 % CPU and 172
wakeups per second.

## M4, video (2026-11-05)

Measured on 3784694220, a 3840x2160 h264 video at 30 fps (7.5 s, so every window below spans
several loops), shown on DP-2 at 2560x1440. CPU and context switches over 15 s with
`/proc/<pid>/stat` after 8 s of warm-up, GPU busy from the DRM fdinfo:

| decode                        | CPU   | switches/s | GPU gfx | VRAM    |
|-------------------------------|-------|------------|---------|---------|
| VA-API, frames as dma-bufs    | 2.7 % | 432        | 0.6 %   | 506 MiB |
| VA-API, frames copied out     | 36 %  | 1014       | 3.7 %   | 472 MiB |
| software (`hwdec=no`)         | 56 %  | 770        | 2.4 %   | 145 MiB |

The copy is av_hwframe_transfer_data into memory, then the three planes through a staging
buffer. The dma-buf path maps each VA-API surface to DRM PRIME (on this card one buffer
object: R8 luma at offset 0, GR88 chroma after it, modifier 0x200000020801b04), imports each
surface once as two images with VK_EXT_image_drm_format_modifier, takes them over from the
foreign queue per frame and samples the chroma interleaved. The decoder's surface pool grows
by seven so the queued frames and the one on screen do not starve it, which is the extra
VRAM. Colours match the copy path in screenshots; the import cache stays at the pool's size
(the dma-buf fd count stays flat over minutes).

The Workshop folder holds 69 videos: 66 h264 (two of them full-range yuvj420p), one HEVC,
one VP9 and one empty download. HEVC, VP9 and h264 with two video streams all play through
VA-API as dma-bufs.

The same video on both outputs: one decoder, the same frame on both screens, 2.0 % CPU and
485 MiB, against 891 MiB with a decoder per output before. Covering DP-2 with a fullscreen
window paused DP-2 while HDMI-A-1 played on.

A synthetic 3 s video with the frame number in its brightness read back at 30 frames per
second across three loops. That test found a bug from the first video commit: the loop
offset was set instead of added to, so from the third pass on every frame was due at once
and the decoder ran as fast as it could (the 42 % and 203 % measured then were that).

### Left out

- A video draws on every frame of the output (or of the `fps` cap) and changes its picture
  at its own rate; it does not pace the output to the video's rate.
- Video textures inside scenes (TEXB0004 with the video flag) are not decoded yet.
- 10-bit (P010) import is written but untested; none of the videos is 10-bit.
- NVDEC and Vulkan Video decode are not done; this machine has neither need.

## M5, particles and puppets (2026-11-05)

### Particles

Of the 107 scene items installed, 80 have particle systems (389 in all, up to 41 in one
scene). A survey of their json (scratch script, not in the tree) gave the module list in
docs/FORMATS.md 9: two emitters, ten initializers, twelve operators, four renderers, with
`sprite` and `alphafade`/`movement` in nearly every system, and `genericparticle` as the
only material shader (318 additive, 70 translucent). glint simulates on the CPU, one quad
per particle in the shader's own vertex layout (`THICKFORMAT`), and uses Wallpaper Engine's
genericparticle shader unchanged, so sprite sheets, overbright and cutout come for free.

Two things it found on the way: the particle textures `particle/light/light_shafts_0` and
most halos are two-channel (RG88, luminance and alpha), which the shaders only read right
with `TEX0FORMAT` set, a define glint had never passed (it now passes `TEXnFORMAT` for every
non-RGBA8 texture, which can matter for image materials too); and reading json per particle
per frame cost 77 % CPU on a 100000-particle system, so the modules are compiled to structs
once (15 % now, 2.7 % for ordinary scenes).

Rope trails (56 systems) are drawn as sprite trails of the rope's length along the velocity,
which is right for straight streaks (meteors) and wrong for curved ones; a real ribbon along
the particle's path is still to do. The children types eventfollow, eventspawn and
eventdeath work; the audio-response fields on emitters and operators are ignored.

### Puppets

The Frieren scene 3104057965 showed a head without a face: the figure is one puppet whose
rest pose is the parts atlas, and the pose comes from the animation. The .mdl animation
block (MDLA) and the bone list (MDLS) are decoded in docs/FORMATS.md 10, checked by a script
that walks all 68 puppet files of the corpus to their block end offsets: 67 parse, one
(3577990983, with a morph block) does not. Composing the rest matrices down the bone chain
puts every bone where its weighted vertices are (checked on 23 bones), and the keyframes'
translations differ from the rest pose exactly for the parts that were scattered.

With CPU skinning each frame, Frieren has her face, hair and the flowers in her hands, Yosuga
(2857144643) sits as in the Workshop preview, and Frieren and Himmel (3426865175) stand
together; the hair and breathing move. A posed puppet costs about 8 % CPU on this machine
(the Frieren scene, 2433 vertices and 21 particle systems).

Not done: attachments following the animated bone (objects hung on a puppet stay at the
rest-pose point), bone physics, morphs (MDMP).

### Shaders, second pass

With the generated sources of failing passes dumped (`GLINT_SHADER_DUMP=<dir> we-inspect
<item> --shaders`), four classes fell: compound assignments that truncate (`vec2 /= vec4`),
`%` on floats, a comparison multiplied by a float, and `mix`/`max` with an int or a scalar on
one side. The survey over all 107 items now compiles 3053 of 3079 passes (26 left, in 20
effects), up from 3031.

### Groups, and a sweep over every scene

Objects with no picture of their own ("Transform" nodes) are parents to other objects in a
quarter of the scenes; glint had placed their children as if the group sat at the origin.
They are read as groups now, their transform and visibility reach their children
(`disablepropagation` stops the visibility), and objects hung on a puppet's attachment follow
the animated bone. 3163060610 went from a stack of shifted layers to the classroom it is.

A sweep set each of the 97 scene items rated Everyone on one output for four seconds, with
Roxy on the other: no crash, no validation error, every scene loaded (the slowest in 0.5 s
from the caches). What the warnings list: 26 shader passes that do not compile, effect
`copy`/`swap` commands (motionblur, not done), and two puppets whose .mdl is cut off at
exactly 1 MiB inside their package, drawn flat.

### The Blue Archive scenes and Yosuga's hair

Three scenes the user pointed at, each a general fault:

- Stargazing Arona (2901582518): the puppet showed no face. Its bone list names every bone
  ("eye", "halo", "ribbon"), a field the parser had taken for five fixed bytes; every other
  rig in the corpus has empty names, which is why it had passed. The haze over the scene was
  the bokeh particles at full count: their `count` is bound to the "bokeh" slider (0.2), and
  instance overrides bound to properties were not resolved. The cursor trail now follows the
  cursor: emitters sit at their control point, which the simulation had ignored. Its "Held"
  mode (only while the mouse button is down, through a script) stays open.
- Arona in the classroom (2919051696): the window showed static. It is the rain on the glass,
  up to 15600 refracting drops; the genericparticle shader reads the scene behind them from
  `g_Texture3`, whose annotated default `_rt_FullFrameBuffer` glint had never bound (nor any
  sampler default). With the framebuffer bound the drops are faint, as in Wallpaper Engine.
  The album cover stays a white placeholder: media integration needs MPRIS and text.
- Yosuga (2857144643): one-pixel lines through the hair. The strands are long thin triangle
  strips posed by their bones; with the keyframe rotation applied counter-clockwise they
  bent against their curve and some triangles folded into lines. Bone z rotations are
  clockwise like object angles; with the sign fixed the right tail hangs as in the preview.

Found on the way and fixed: alphafade defaults (particles popped out), the `rope`
renderer (one ribbon through all particles, used by cursor trails), and a puppet's layer
now holds the whole stored texture because mesh uvs run a little past the picture.

### Yosuga once more

The pieces the user saw flying about the hair are two things. The spiked strand ends in
front of the sleeve are painted that way (the texture of 右边头发2 flares into points at the
bottom) and the scene lists that bundle after the body, so it is drawn in front; that part
is as authored. The torn, stretched look came from summing bone rotations down the chain;
with joints following the parent but each bone keeping its own rotation the strands sway
gently and stay whole. The remaining uncertainty is the exact sway, which a 192-pixel
preview cannot settle.

### Text objects

Clocks, dates and song titles are text objects. glint sets them with FreeType and HarfBuzz
and draws the block like a picture (docs/FORMATS.md, 12). The layout rules come from the
open-wallpaper-engine-mac project, which read them out of the engine: the em is the point
size at 300 dpi, advances are whole units, scene.json's `size` is ignored and the block
(bounds plus padding) hangs off the origin by the alignments. The corpus confirms it: the
date block of the Roxy scene comes out 590x160 against the 590x159 the editor stored. Fonts
come from the item (Workshop fonts ship in the package), the assets or fontconfig for the
`systemfont_` names. Scripted texts show their stored value for now; the clock reads 12:34
until SceneScript runs.

### SceneScript

The clock now ticks. Scripts run on quickjs-ng, one context per scene and output, each
script its own module (docs/FORMATS.md, 11). The API surface came from the official type
declarations and from counting what the corpus actually calls: `update` and `init` on
nearly every script, `createScriptProperties` on most, `engine.canvasSize`, `frametime`,
`registerAudioBuffers`, `thisLayer.origin`/`visible`, `shared`, `getParent`,
`input.cursorWorldPosition`, `thisObject.getAnimation().play()` in click handlers, the media
events. Two things the corpus taught that the docs do not say: the object's
`scriptproperties` may be bound to project properties, nested and with conditions, and
sound objects are layers scripts look up by name to start and stop them. The 1266 scripts
of the 36 scripted items load and run; what is left open is listed in FORMATS.

Design note: scripts must not write into the scene description, which two outputs share.
Every bound field got a key at parse time, and a per-output vector of cells, reachable
through `Values`, holds what scripts wrote. The readers did not change; `Bound::at()` looks
in the cell first.

### Media

The album cover and the song title. Players announce themselves on the session bus as
MPRIS services; glint reads the playing one and hands the scene a state with versions, so
the scripts get each event once (docs/FORMATS.md, 13). The cover comes as an https URL from
Spotify, and FFmpeg's own http client fetches it, so no new dependency was needed for that;
sdbus-c++ was agreed on earlier. The thumbnail slot in a scene is a material instance with
a system texture, which the readers had skipped; it now resolves to the cover when there is
one and to the scene's placeholder otherwise.

### Layers made by scripts

The audio visualisers in the corpus draw nothing of their own: their one script creates
sixty-three copies of a bar model at start and moves them with the spectrum. The script
host now owns a copy of the scene description and adds layers to it, with their textures
and shader variants read and compiled on the spot; `alignment`, which no static layer in
the corpus uses, puts a bar's origin on its bottom edge so it grows upward. The image list
is reserved ahead, because scripts and draws point into it.

### The last shader failures

Of the corpus's shaders 28 passes had refused to compile; none is left. They were all
HLSL letting things slide that GLSL does not: a float4 texture coordinate (`texSample2D`
with the varying as a whole), a float4 passed to a function taking float2, a float4 in
arithmetic whose result is assigned to a float2, `float2 a = 0.0, b = 0.0` in one
declaration, `for (int i = someFloat; ...)`, a global `const` fed by a uniform, `x ? a :
b` with a number for x, a varying array that needs a location per element, and a shader
defining its own `log10` beside the dialect's macro. Each got its rule in the translator
(src/wallpaper/scene/shader.cpp, `hlsl_conversions`): the types of globals, parameters and
locals are known from their declarations, so an identifier wider than the slot it goes
into gets the swizzle that HLSL's truncation implies.

### Hardening from the review issues

The read-only review of fe2c279 had filed sixteen issues (#51 to #66); this batch closes
them. A request with a field of the wrong type, a worker that throws, a scene.json with a
string id, a condition with a thousand parentheses, a shader naming g_Texture999999 or a
.tex with a mip larger than its data all used to be able to end or mislead the daemon; each
now fails only the request or the file. A screenshot answers at once when none can be
taken, and is answered when the output pauses or goes away; a suboptimal present makes the
swapchain anew with the next draw and the loop gets a wakeup for it; the pointer redraws a
parallax scene at rest; events Mesa read during a frame are drawn in the same pass; the
toplevel manager stays until the compositor's "finished"; a set is not rolled back when one
of its outputs is unplugged; props requests are checked whole before anything is applied,
sliders are clamped, combos checked, and a colour given as #rrggbb survives the state file;
the state file is synced before its rename and kept per display for a nested compositor;
the socket's directory is only made private when glint made it, two daemons no longer
unlink each other, and a client that never reads is dropped at four megabytes of backlog.

### Motion blur

Four items carry a motion blur effect: an accumulation pass reads the previous frame's
result from an fbo marked `unique`, writes the new one, and a `copy` command carries it back
for the next frame. The copy is glint's own copy pass between two targets, and a unique
fbo keeps its image across frames (cleared once when it is made) instead of sharing the
pool. The effect was refused with a warning before; now it runs.

### Sound objects under scripts

The music players in the corpus keep their songs as sound objects that start silent and
are started, held and stopped by scripts (`getLayer(name).play()`). The player's tracks
now take that: a held track keeps its ring and stays silent, a stopped one is taken back
to its first file by the decoder, and the scene applies what the scripts changed once per
frame, by version.

### glint-render

Both outputs were covered by games for most of a day, and a covered output gets no frames,
so nothing could be looked at. `glint-render <item> out.png --frames 120` now draws a scene
headless: the GPU context without surface extensions, the renderer into a colour image,
the last frame read back as a PNG, 120 frames of the Roxy scene in 0.4 s. It is the tool for
regression pictures as well (issue #21). The first picture it drew showed the date and the
clock set by their scripts, in place, with their blur.

### A solid layer's effects

The classroom's sound bar is a white solid layer with an audio-bars effect and a
perspective warp. glint drew a white square: a solid layer's material shader (`flat`)
paints its colour and never looks at a texture, so after the effects had run it painted
over their result. With effects on a solid layer the last pass is `genericimage2` now,
drawing the effects' result tinted by the layer's colour; the frame around the album cover
came out purple as authored and the bars are gone until there is audio. One run of
glint-render on this scene ended in a heap corruption that did not come back in several
runs, with or without the sanitizer; noted here in case it shows again.

### Two more from the headless pictures

A terminal-styled scene showed a large "false": a text object bound to a property of the
label type, whose value is a boolean; a text now shows nothing for a boolean and the number
for a number. Its GIF window stayed empty because the GIF is a video texture (TEXB0003 with
bit 5 of the flags), which the new mip-size check had refused as too short; the flag now
counts, and the video itself still waits for M4.

### Video textures

The dark, noisy render of one scene turned out to be its background missing: a 4K mp4
inside a .tex, which glint had skipped since M2 (and, for older files, had uploaded as if
it were pixels). Nine items carry such textures. They now go through the video wallpaper's
decoder into an RGBA picture per frame (docs/FORMATS.md, 14); the GIF windows of the
terminal scene move and the 4K background shows. The decoder is opened on the loading
worker with its first frame ready, so the first draw has a picture.

### Older puppets

Four items' puppets were drawn flat: their .mdl files are older or newer versions with a
different header and vertex stride (52 bytes without normal and tangent, 84 with one float
more), which the reader had taken for the usual 80. The layouts are told apart by their
flags now (docs/FORMATS.md, 10), with a search over plausible strides for flags not seen
before. Lucy and her cat, and Gojo, are puppets again.

### The contact sheets

A headless render of every safe-rated scene, laid out as contact sheets next to the
previews, showed six scenes clearly wrong. Each was a general fault:

- Hu Tao (2470680861) was one brown surface: a 4 x 4 compose layer that follows the cursor
  has no effects, and its shader fills the target with the patch behind it. A passthrough
  layer without effects is now not drawn at all.
- Ellen (3290874632) showed the picture three times, squeezed: a compose layer with effects
  had copied the whole frame into its layer instead of the patch behind it. The material
  pass now fills the layer and `genericimage2` draws the result back.
- Ai Hoshino (2983739644) was white: a solid layer of size `0 0` had fallen back to the
  canvas size.
- Lucy (3521337568) fell apart: bone keyframe rotations were negated against the rest
  pose. A check over all 66 puppets of the safe corpus found the keyframe angle equal to the
  rest angle in every rig where it can be told (Lucy, Gojo, Martinez, XME, Jane); Yosuga's
  bones rest unturned and its hair looks the same either way. Her black wedge was the
  atmosphere over the Earth, an object with `colorBlendMode` 7, which glint had read and never
  used; it is now the shaders' `BLENDMODE` combo.
- Reze (3577990983) was not a fault: an opening animation fades a black layer out over
  three seconds, longer than the render ran.

The daemon also stopped drawing on one output for good after a pause right at its start: a
frame callback asked for before the pause never came, and no frame is drawn while one is
pending. A resume now gives up on a pending callback and draws.


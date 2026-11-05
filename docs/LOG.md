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
Frieren - Pagoda of flowers (3426865175); Astronaut, Forest River and Aurora Lake are not
subscribed yet, so the five-scene check of the milestone is open. Each engine ran alone on
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
together; it does not move. Setting `xme` off over `glint props` hides Himmel at once, and
the conditions of the other settings follow.

### Left out or different from the plan

- Particles, text, sound, lights, puppet animation, the camera shake and bloom: M3 and M5.
  Values animated by timelines take their static value.
- The puppet's placement uses the centre of its mesh's bounding box; the exact transform
  from the skeleton is open (FORMATS.md, 10).
- Copybackground, effect commands (`copy`, `swap`) and the 33 colour blend modes that read
  `_rt_FullFrameBuffer` through the BLENDMODE combo are not done; nothing in the two installed
  scenes uses them. The fixed blendings (normal, translucent, additive) are.
- Layers are rendered at the output's scale, not at the canvas size, which saves memory on a
  smaller screen and changes effects that count in texels slightly.
- A property change rebuilds the plan and the uniform blocks; a combo bound to a property
  would need a recompile, which is not done (none in the corpus).
- The parallax follows the pointer with the scene's delay; its strength is glint's guess
  (FORMATS.md has no number for it) and has not been compared with Wallpaper Engine.

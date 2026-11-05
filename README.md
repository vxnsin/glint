<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="brand/wordmark-dark.svg">
    <img src="brand/wordmark-light.svg" alt="glint" width="300">
  </picture>
</p>

<p align="center">
  A wallpaper engine for Wayland. Vulkan, one daemon, every output.<br>
  Built for <a href="https://github.com/facet-shell">facet</a>, usable from any shell.
</p>

**Status: milestone M5 in progress.** The daemon puts still pictures and Wallpaper Engine
scenes (image layers, effects, particles, puppets, text, live settings) on every output with
Vulkan and is driven over the socket; SceneScript, media, sound and video come next. Open work is tracked in the [issues](https://github.com/vxnsin/glint/issues).

## What it does

glint runs as one process per session, puts a layer-shell surface on every output and draws
into it with Vulkan. A shell or a script talks to it over a Unix socket.

- **Still pictures and videos.** Pictures decode once and then cost nothing. Videos decode in
  hardware where the driver allows it.
- **Wallpaper Engine backgrounds.** Scenes from the Steam Workshop with their effects,
  particles, sounds and settings. Web wallpapers are planned as an optional module.
- **The wallpaper's own settings, live.** Every option a scene author put into
  `project.json` is listed over the socket and can be changed while the scene runs. A
  shell can build a settings panel from that list without knowing the format.
- **Interaction and audio.** Cursor position goes into the scene (parallax, mouse effects,
  particle control points). Sounds play through PipeWire. Audio-reactive scenes get a
  spectrum of what you are listening to.
- **Zero cost when nothing happens.** Frames are drawn only when the compositor asks and
  something changed. A fullscreen game or a locked session drops an output to zero frames
  and zero wakeups.
- **Fast switching.** Shaders and textures are cached on disk, loading runs in the
  background, and the old wallpaper stays on screen until the new one is ready.
- **Per output.** Different wallpapers per screen, fractional scale, outputs that come and
  go.

## Why not the engines that exist

[linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) made Workshop
scenes run on Linux and taught us the format. But it starts a new process for every
switch and compiles everything again, has no way to change a setting in a running
instance, renders on its own clock instead of the compositor's, and busy waits while
paused. The plan has a [close reading](docs/PLAN.md#reading-linux-wallpaperengine) of its
render path and what glint does differently.

## Build

Linux, a Wayland compositor with `wlr-layer-shell` (Hyprland, sway, river, niri), Vulkan
1.3. Packages are listed in [docs/PLAN.md](docs/PLAN.md#dependencies).

```
cmake --preset release
cmake --build --preset release
```

## Use

```
glint                                   # the daemon; Hyprland: exec-once = glint
glint set DP-1 ~/Pictures/lake.jpg
glint set '*' ~/Pictures/lake.jpg scaling=fit color=#203040
glint status                            # per output: state, frames, CPU and GPU time
glint pause '*' locked                  # from a lockscreen; resume with the same reason
glint subscribe                         # events as JSON lines
```

Wallpaper Engine scenes from the Workshop and their settings:

```
glint set '*' 3426865175                # a Workshop id, or the item's folder
glint props DP-1                        # the scene's own settings
glint props DP-1 bloom off
glint screenshot DP-1 shot.png          # what glint drew, without the windows on top
```

The daemon remembers what was on which output in `$XDG_STATE_HOME/glint/state.json` and puts
it back on the next start.

The socket protocol is in [docs/IPC.md](docs/IPC.md). It is plain JSON lines, so a shell
can use it directly; Quickshell's `Socket` type does, and `socat` works for scripts.

## Docs

- [docs/PLAN.md](docs/PLAN.md): the stack and why, modules, milestones, what is measured.
- [docs/IPC.md](docs/IPC.md): the socket protocol and how facet moves onto it.
- [docs/FORMATS.md](docs/FORMATS.md): what of Wallpaper Engine's formats glint reads.
- [docs/LOG.md](docs/LOG.md): the measurements per milestone.

## License

MIT. glint is not affiliated with Wallpaper Engine or Valve and ships none of their
assets; scenes need Wallpaper Engine installed through Steam.

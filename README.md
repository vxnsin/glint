<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="brand/wordmark-dark.svg">
    <img src="brand/wordmark-light.svg" alt="glint" width="300">
  </picture>
</p>

<p align="center">
  A Wayland wallpaper engine that draws with Vulkan.<br>
  Built for <a href="https://github.com/facet-shell">facet</a>, usable from any shell.
</p>

<p align="center">
  <a href="https://github.com/vxnsin/glint/actions/workflows/ci.yml"><img src="https://github.com/vxnsin/glint/actions/workflows/ci.yml/badge.svg" alt="ci"></a>
</p>

glint is one daemon per session. It puts a layer-shell surface on every output, draws
into it with Vulkan and takes its orders over a Unix socket.

It shows pictures, videos and Wallpaper Engine scenes from the Steam Workshop: effects,
particles, puppets, text, sound and SceneScript. The options a scene author put into
`project.json` can be changed while the scene runs.

Version 0.1.0 is early. It has been tested on one machine, Hyprland with AMD graphics,
and some scenes still look wrong. [docs/STATUS.md](docs/STATUS.md) lists what works and
what is missing, and the [issues](https://github.com/vxnsin/glint/issues) track the rest.
NVIDIA has not been tried.

## Install

You need Linux, a Wayland compositor with `wlr-layer-shell` (Hyprland, sway, river, niri)
and Vulkan 1.3. The packages are listed in [docs/PLAN.md](docs/PLAN.md#dependencies),
with their Arch names.

On Arch, unpack the archive from the [latest
release](https://github.com/vxnsin/glint/releases/latest) and run `bin/glint`. On any
other system, build the source:

```
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix ~/.local
```

The install puts `glint` in `~/.local/bin` and adds a Glint entry to the application
launcher, which opens the terminal UI.

## Use

```
glint                                   # terminal UI
glint --daemon                          # the daemon; Hyprland: exec-once = glint --daemon
glint set DP-1 ~/Pictures/lake.jpg
glint set '*' 3426865175                # a Workshop id or an item folder
glint set '*' lake.jpg scaling=fit color=#203040
glint choose roxy                       # pick a wallpaper and an output in a menu
glint list hikari                       # search installed Workshop titles, ids, tags
glint props DP-1                        # the scene's own settings
glint props DP-1 bloom off
glint configure DP-1                    # the same, in a terminal menu
glint status                            # per output: state, frames, CPU and GPU time
glint pause '*' locked                  # from a lockscreen; resume with the same reason
glint screenshot DP-1 shot.png
glint subscribe                         # events as JSON lines
```

The daemon remembers what was on which output and restores it on the next start.

The [terminal UI](docs/TUI.md) has previews, search, display selection and the settings
of the current wallpaper. Workshop items open in Steam, or download by id with SteamCMD
if you have it installed.

The build also makes `glint-render`, which draws a scene into a PNG without a compositor,
and `we-inspect`, which dumps what glint reads from a Workshop item.

The socket protocol is in [docs/IPC.md](docs/IPC.md). It is plain JSON lines, so a shell
can talk to it directly. Quickshell's `Socket` does, and `socat` works for scripts.

## Why not linux-wallpaperengine

[linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) made Workshop
scenes run on Linux and taught us the format. It starts a new process for every switch,
cannot change a setting in a running instance, renders on its own clock instead of the
compositor's, and busy waits while paused. glint is a rewrite built around those points.
The [plan](docs/PLAN.md#reading-linux-wallpaperengine) has the details.

## Docs

- [docs/PLAN.md](docs/PLAN.md): the stack, modules, milestones and dependencies.
- [docs/STATUS.md](docs/STATUS.md): what works and what does not.
- [docs/IPC.md](docs/IPC.md): the socket protocol.
- [docs/FORMATS.md](docs/FORMATS.md): which Wallpaper Engine formats glint reads.
- [docs/TUI.md](docs/TUI.md): the terminal UI.
- [docs/LOG.md](docs/LOG.md): measurements per milestone.

## License

MIT. glint is not affiliated with Wallpaper Engine or Valve and ships none of their
assets. Scenes need Wallpaper Engine installed through Steam.

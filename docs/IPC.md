# glint IPC

Version 1. Implemented: `outputs`, `set`, `unset`, `status`, `pause`, `resume`, `props`,
`cursor`, `list`, `inspect`, `subscribe`, `screenshot` and `version`. The operations
`audio` and `settings` are not implemented yet; their descriptions below are plans.

The CLI also has offline `list [search]` and `inspect <Workshop id|folder>` commands. They
read installed projects directly, support `--json`, and use `GLINT_WORKSHOP` or Steam's
default Workshop folder. The offline inspect output includes project metadata, properties,
entry existence, scene counts, skipped object kinds and parser warnings. The socket's
`list` and `inspect` operations return the same library data without launching a CLI.

## Transport

- Unix stream socket at `$XDG_RUNTIME_DIR/glint/$WAYLAND_DISPLAY.sock` (one daemon per
  Wayland session). `GLINT_SOCKET` overrides the path.
- Newline-delimited JSON, UTF-8. One object per line in both directions.
- Request: `{"id": 7, "op": "set", ...}`. Reply: `{"id": 7, "ok": true, "result": ...}` or
  `{"id": 7, "ok": false, "error": "no such output"}`. `id` is anything the client picks; it is
  echoed. Requests without `id` get no reply.
- Events: `{"event": "state", ...}` to every client that sent `subscribe`. Events carry no
  `id`.
- A client may keep the connection open and interleave requests and events; replies may
  arrive in any order (a `set` of a big scene answers when the scene is ready, a `status` in
  between answers at once).

The CLI is the same thing in argv form: `glint set DP-1 lake.jpg` sends
`{"op":"set","output":"DP-1","path":"lake.jpg"}` and prints the result as JSON (`--json`)
or as text.

## Common fields

- `output`: a connector name as the compositor reports it (`DP-1`, `HDMI-A-1`), or `"*"` for
  all current and future outputs. Per-output entries win over `*`. Setting `*` replaces the
  per-output entries, so every output shows it; a later per-output `set` wins again.
- `path`: absolute path. A file is a picture or video by content; a directory is a Wallpaper
  Engine project (contains `project.json`); a Workshop id is accepted when the Workshop
  folder is known (`workshop` setting or Steam's default path).

## Operations

### outputs

```
-> {"op": "outputs"}
<- {"ok": true, "result": [
     {"name": "DP-1", "make": "...", "model": "...", "width": 3440, "height": 1440,
      "scale": 1.0, "refresh": 144.0, "x": 0, "y": 0,
      "wallpaper": {"path": "...", "kind": "scene", "state": "running"}}]}
```

`width` and `height` are buffer pixels after fractional scale; `description` is the
compositor's text for the output. `state` is `running`, `paused` (with `reason`: `covered`,
`locked`, `request`, `idle`), `loading`, `error` (with `message`), `none`.

### set / unset

```
-> {"op": "set", "output": "DP-1", "path": "/.../431960/123456",
    "options": {"scaling": "fill", "fps": 60, "transition": "fade", "duration": 300}}
<- {"ok": true, "result": {"kind": "scene", "title": "...", "load_ms": 840}}

-> {"op": "unset", "output": "DP-1"}        // back to the background colour
```

`options` per kind:

- all: `scaling` (`fill`, `fit`, `center`, `tile`, `stretch`), `color` (`"#rrggbb"`,
  behind `fit`), `fps` (cap), `transition`, `duration`
- video: `volume` (0 to 1), `mute`, `loop`, `speed`, `hwdec` (`auto`, `vaapi`, `no`)
- scene: `volume`, `mute`, `cursor` (`on`, `off`), `parallax` (`on`, `off`), `audio`
  (`on`, `off`; the capture for reactive scenes), `properties` (initial values, same as
  `props set`)

Built for videos: `volume` and `mute` (the file's sound track, silent by default), `speed`,
`hwdec`; videos always loop. Outputs showing the same file with the same `hwdec`, `speed`
and volume share one decoder and one sound stream and stay on the same frame; each keeps its
own `scaling` and `color`. With VA-API the decoded frames are imported into Vulkan as
dma-bufs without a copy; when the driver cannot export or import them glint copies them out
instead, as it does with `GLINT_VIDEO_COPY=1`.

Built so far for scenes: `volume` (0 to 1, default 0: a scene is silent unless asked, so a
login does not start music) and `mute` play the scene's sound objects through one PipeWire
stream; `properties`. Two outputs showing the same scene play its sound once. A paused
output's sound pauses with it and resumes where it was. Sound objects marked `startsilent`
wait for a script (M5).

The reply comes when every addressed output has presented its first frame with the new
wallpaper (an output that is paused counts once the new wallpaper is loaded); `load_ms` is
the time from the request to that point. The crossfade, if any, runs on after the reply. The
old wallpaper keeps drawing until the new one is ready, and the first wallpaper on an output
appears without a fade. A second `set` for the same output while one is loading cancels the
first (the reply of the cancelled one is `{"ok": false, "error": "superseded"}`).

A path that does not exist or cannot be read is refused before anything changes. A picture
that fails to decode answers with the decoder's error, the output keeps what it showed, and
the assignment goes back to what it was. Successful `set` and `unset` are written to
`$XDG_STATE_HOME/glint/state.json` and restored when the daemon starts.

A folder with a `project.json` of type `scene` is a Wallpaper Engine scene; the CLI also takes
a bare Workshop id and looks in Steam's Workshop folder. Every file is decoded as a still
picture with FFmpeg (JPEG, PNG, WebP, and the first frame of anything else FFmpeg reads). Options in M1: `scaling`, `color`, `transition` (`fade`
or `none`), `duration` (milliseconds, default 300), `fps` (a cap on the frames drawn per
second, 0 for the output's own rate; a timer holds the next frame back when the
compositor's frame callback comes early, so the rate holds exactly, e.g. 24 on a 60 Hz
output alternates two and three vblanks).

`unset` on an output with its own entry falls back to `*` if there is one, else to the
background colour (black).

### status

```
-> {"op": "status", "output": "DP-1"}
<- {"ok": true, "result": {"path": "...", "kind": "image", "state": "running",
     "fps": 0, "frames": 3, "frame_ms": 0.4, "gpu_ms": 0.1, "load_ms": 18.5,
     "memory_mb": 20.3, "paused_reason": null, "paused_reasons": [],
     "width": 2560, "height": 1440, "swapchain_images": 3,
     "options": {"scaling": "fill", "color": "#000000", "transition": "fade", "duration": 300},
     "picture": {"width": 2560, "height": 1440}, "wakeups": 82}}
```

`audio` is the capture for audio-reactive scenes: `capture` says whether it runs (only
while a shown, running wallpaper wants the spectrum), `peak` the strongest band now.
`fps` counts the frames presented in the last second, so a still picture reads 0. `frame_ms`
is the CPU time of the last frame, `gpu_ms` its GPU time from timestamp queries, `load_ms`
the time of the last `set` on this output. `wakeups` counts the daemon's returns from
`poll` since it started. Without `output` (or with `*`): an object keyed by output.

### props list / get / set / reset

The background's own settings. For a scene these are `project.json` `general.properties`
(types `bool`, `slider`, `combo`, `color`, `text`, `file`, `directory`, `textinput`, `scenetexture`),
plus the ones every scene has (`volume`, `fps`, `cursor`, `parallax`, `audio`). For a
video: `volume`, `mute`, `loop`, `speed`. For a picture: `scaling`, `color`.

```
-> {"op": "props", "output": "DP-1"}
<- {"ok": true, "result": [
     {"name": "bloom", "type": "bool", "text": "Bloom", "value": true, "order": 100},
     {"name": "speed", "type": "slider", "text": "Speed", "value": 1.0,
      "min": 0, "max": 3, "step": 0.1, "order": 101},
     {"name": "scheme", "type": "combo", "text": "Color scheme", "value": "warm",
      "options": [{"label": "Warm", "value": "warm"}, {"label": "Cold", "value": "cold"}]},
     {"name": "tint", "type": "color", "text": "Tint", "value": "#ff8800"},
     {"name": "snow", "type": "bool", "text": "Snow", "value": false,
      "condition": "season.value == 'winter'", "visible": false}]}

-> {"op": "props", "output": "DP-1", "set": {"bloom": false, "speed": 1.5}}
<- {"ok": true, "result": {"changed": ["bloom", "speed"], "recompiled": ["bloom"]}}

-> {"op": "props", "output": "DP-1", "reset": true}      // back to the project's defaults
```

In glint: for a scene the list is `project.json`'s properties, labels and HTML headings
(type `text`, `group`) included so a panel can show them. Pictures have no properties yet.
`props` applies to the wallpaper on that output; the reply to a set lists `recompiled` as
empty, since every bound value in the corpus is a uniform or a visibility.

Rules:

- Values are typed as in the listing. `color` is `#rrggbb` in both directions (Wallpaper
  Engine stores `"r g b"` floats; glint converts).
- `condition` is the project's expression; `visible` is glint's evaluation of it, so a shell
  can hide the entry without parsing the expression.
- Set values are persisted per project path and restored on every later `set` of that path,
  on any output, until `reset`.
- A change that flips a shader combo recompiles from the cache; the old pipeline draws until
  the new one is ready.
- The `props` event (below) fires for every change, including the ones a script makes.

### screenshot

```
-> {"op": "screenshot", "output": "DP-1", "path": "/tmp/shot.png"}
<- {"ok": true, "result": {"path": "/tmp/shot.png", "width": 2560, "height": 1440}}
```

Reads back the next frame glint presents on the output, after its own drawing and before the
compositor's, and writes it as PNG. A still output draws once more for it; a paused one
answers with an error.

### pause / resume

```
-> {"op": "pause", "output": "*", "reason": "locked"}
-> {"op": "resume", "output": "*", "reason": "locked"}
```

`output` defaults to `*`, `reason` to `request`. Reasons stack: an output paused for
`locked` and `covered` resumes only when both are lifted. Reasons given for `*` also apply to
outputs that appear later; `resume` for `*` lifts the reason on every output. glint sets
`covered` and `idle` itself and refuses them from clients; shells use `locked` and `request`.

`covered` comes from foreign-toplevel: a fullscreen window with focus on the output. The
protocol does not say whether a window's workspace is visible, and on Hyprland a fullscreen
window on a hidden or special workspace still reports fullscreen on its output, so without
focus it does not count. A covered output that glint misses costs little: the compositor
sends no frame callbacks to a surface it does not draw. glint follows
toplevels only while an output shows something that wants frames (videos and scenes, from
M2 on), because the protocol reports every title change of every window and would wake the
daemon for nothing while it shows still pictures. A covered still picture costs nothing
anyway: it presents no frames. `GLINT_TRACK_TOPLEVELS=1` turns the tracking on always.

### audio

```
-> {"op": "audio", "output": "DP-1", "volume": 0.3, "mute": false}
-> {"op": "audio", "capture": false}        // global: never open a capture stream
```

### cursor

```
-> {"op": "cursor", "output": "DP-1", "x": 1200.5, "y": 300.0, "buttons": 1}
```

Only needed on compositors where glint cannot see the pointer itself. On Hyprland glint asks
the request socket for `cursorpos` once per drawn frame while a wallpaper wants the cursor,
so the cursor counts over windows too; elsewhere it uses `wl_pointer` on the layer surface,
which only sees the pointer over the bare desktop. A scene wants the cursor when its camera
parallax is on or a shader reads `g_PointerPosition`, and then draws every frame. The
`source` field is planned, not built.

### settings

Daemon-wide values, persisted:

```
-> {"op": "settings", "set": {"workshop": "/path/to/431960", "assets": "/path/to/assets",
    "fps": 60, "idle_after": 300, "battery_fps": 30, "battery_pause": false,
    "free_after": 600, "fullscreen_pause": true,
    "rules": [{"app_id": "mpv", "action": "ignore"},
              {"app_id": "discord", "action": "mute"},
              {"app_id": "steam_app_*", "fullscreen": true, "action": "stop"},
              {"title": "*YouTube*", "maximized": true, "action": "pause"}]}}
-> {"op": "settings"}                        // read them all
```

The power values: `idle_after` pauses every output with reason `idle` after that many
seconds without input and resumes with the next input; `battery_fps` caps the frame rate
and `battery_pause` pauses while UPower reports `OnBattery`; `free_after` makes an output
that has been covered for that many seconds drop its wallpaper's memory (decoders,
textures, render targets) and rebuild it from the caches when uncovered. 0 switches each
one off.

`rules` decide what the windows on an output do to it, on top of the built-in rule that a
fullscreen window with focus pauses its output (`fullscreen_pause` turns that off). A rule
matches on `app_id` (the X11 class for Xwayland windows) and `title`, both globs with `*`
and `?` matched without regard to case, and on the states `fullscreen`, `maximized` and
`activated` when given. The first matching rule decides for a window: `pause` stops the
frames, `mute` the sounds, `stop` pauses and frees the memory as `free_after` does, and
`ignore` exempts the window from every rule, the built-in one included. An output gets
what all of its windows ask for together; minimized windows never count. The logic is in
`src/policy`, the toplevel feed and UPower in the daemon (M6).

### list / inspect

Reads installed Workshop projects without loading them as wallpapers. Both operations run
on a worker; clients may send other requests while waiting for the reply. Search matches
title, ID and tags, ignoring ASCII case. `GLINT_WORKSHOP` selects the library root.

```
-> {"id": 1, "op": "list", "search": "roxy"}
<- {"id": 1, "ok": true, "result": [
     {"id": "123456", "title": "Roxy", "type": "scene", "path": "/.../431960/123456",
      "preview": "preview.gif", "tags": ["Anime"], "rating": "Everyone", "property_count": 3}]}

-> {"id": 2, "op": "inspect", "path": "123456"}
<- {"id": 2, "ok": true, "result": {
     "id": "123456", "title": "Roxy", "type": "scene", "path": "/.../431960/123456",
     "preview": "preview.gif", "entry_exists": true, "properties": [...],
     "scene": {"width": 3840, "height": 2160, "images": 8, "skipped_objects": {}},
     "warnings": []}}
```

`inspect` accepts an absolute folder or a Workshop ID. `preview` is relative to the item's
`path`; it can be empty or missing on disk. The results describe installed Workshop items,
not arbitrary local image/video folders. `scene` is present only for scene projects.
Compatibility cannot yet be reduced to a `supported` flag; `warnings` and
`scene.skipped_objects` expose known parsing limitations without asserting visual fidelity.

### subscribe

```
-> {"op": "subscribe", "events": ["state", "props", "outputs"]}
```

Without `events`: all of them. The reply lists what the client is subscribed to.

Events:

- `{"event": "state", "output": "DP-1", "state": "paused", "reason": "covered"}`, sent when
  the state of an output changes; `reasons` lists all of them when there is more than one
- `{"event": "props", "output": "DP-1", "changed": {"bloom": false}}`
- `{"event": "outputs", "added": ["DP-2"], "removed": []}`
- `{"event": "error", "output": "DP-1", "message": "..."}`

### version

`{"op": "version"}` -> `{"version": "0.1.0", "protocol": 1, "vulkan": "1.3", "device": "...", "operations": [...]}`

`operations` lists the operations this daemon implements. Older protocol-1 daemons may omit
it, so clients should handle both a missing list and an error for an unsupported operation.

## Running the daemon

`glint` without a command is the daemon. A second one on the same Wayland display refuses to
start. `GLINT_LOG=debug` logs every frame; `GLINT_VALIDATION=1` loads the Vulkan validation
layer if it is installed.

## Facet integration

Facet's wallpaper service uses Quickshell's `Socket` client when the backend marker
`$XDG_STATE_HOME/frameshell/wallpaper-backend` contains `glint`. It subscribes to
state, property, output and error events and reconnects after daemon restarts.
`integrations/facet/GlintClient.qml` is the reusable transport component.

Bloom's `wall` launcher mode and both styles' Super+W wallpaper views send `set`
and `props` requests directly. Common settings first request current status and
update each selected output separately, preserving its path and other options.
Authored property changes target only displays showing the selected project.
The background surface yields to Glint on assigned outputs; locking sends
`pause` with `locked` and unlocking sends the matching `resume`. Fullscreen
pause remains Glint's responsibility.

`frame-wallpaper list-local` supplies local image/video library entries alongside
Glint's installed Workshop list. Other legacy helper commands remain available
for configurations that have not selected Glint. A disconnected Glint selection
does not fall back to a legacy renderer. `frame-glint-start` handles autostart,
existing-daemon reuse and first-run migration without replacing saved Glint state.
`facet wallpaper` opens the separate terminal interface; explicit subcommands
forward to the Glint CLI. Online discovery opens Steam Workshop.

The initial native integration is committed in Facet core, CLI and Bloom/Tui
styles and locally installed. Publication is pending repository write access.
Offscreen tests cover the native views with a fake asynchronous transport; the
real desktop socket and compositor still require user validation. Other shells
use the same protocol without any dependency on Facet.

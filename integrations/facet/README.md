# Facet integration

`GlintClient.qml` is an optional Quickshell component. It connects to the glint socket,
subscribes to state, property, output and error events, and reconnects if the daemon goes
away. Requests return IDs; the `response` and `requestFailed` signals carry that ID and the
operation name, so replies can arrive in any order.

```qml
GlintClient {
    id: glint
    onConnectedChanged: {
        if (connected) {
            outputs();
            list("");
        }
    }
    onResponse: (requestId, operation, result) => {
        if (operation === "list")
            console.log(`Found ${result.length} Workshop items`);
    }
    onRequestFailed: (requestId, operation, message) => console.warn(message)
    onEventReceived: (event, data) => console.log(event)
}
```

Methods include `list(search)`, `inspect(path)`, `outputs()`, `setWallpaper(output, path,
options)`, `settings(output)`, `setSettings(output, values)`, `resetSettings(output)`,
`pause(output, reason)` and `resume(output, reason)`. `list` returns installed Workshop
items, not Facet's local image/video folders. The list response's `preview` may be absolute or relative to
the item `path`. A `set` socket request needs an absolute path; use the `path` from a list
or inspect response.

The component respects `GLINT_SOCKET`, `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`. If
`XDG_RUNTIME_DIR` is missing, set `GLINT_SOCKET` explicitly. Requests made while disconnected
fail immediately; the caller can react to `connected` before retrying.

`Smoke.qml` checks reply and event dispatch without connecting to a daemon. For example:

```sh
env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= \
  QT_QUICK_BACKEND=software qs -p integrations/facet/Smoke.qml
```

Facet's core and Bloom/Tui styles now include a native integration built on this client.
`facet install quickshell hypr wallpaper` installs Glint and selects it as the backend;
Hyprland starts `frame-glint-start`, which reuses an existing daemon or restores saved
assignments. Bloom's `wall` launcher mode and both styles' Super+W views use the socket
directly, including per-display common settings and authored project properties. The
lockscreen pauses and resumes Glint with the `locked` reason.

The legacy helper supplies local image/video library entries; it does not start a
renderer when Glint is selected. Offline Glint reports a connection error and offers a
start button. The separate `glint tui` remains available for terminal users.

The native views and migration have asynchronous fake-transport and offscreen tests in
Facet core (`tests/glint`). Real compositor behavior still requires a desktop check.
The initial integration is committed and locally installed; remote publication is
pending repository write access. Online Workshop discovery currently opens Steam,
rather than embedding its catalog or download interface. See `docs/IPC.md`.

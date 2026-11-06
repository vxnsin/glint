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
items, not Facet's local image/video folders. The list response's `preview` is relative to
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

The component is not installed into Facet yet. Its existing wallpaper service, Backdrop,
launcher and lockscreen still use their current backend. See `docs/IPC.md` for the protocol
and remaining integration work.

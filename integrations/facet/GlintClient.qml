import QtQuick
import Quickshell
import Quickshell.Io

Scope {
    id: root

    property bool enabled: true
    readonly property bool connected: socket.connected
    readonly property string socketPath: {
        const override = Quickshell.env("GLINT_SOCKET");
        if (override)
            return override;
        const runtime = Quickshell.env("XDG_RUNTIME_DIR");
        if (!runtime)
            return "";
        const display = (Quickshell.env("WAYLAND_DISPLAY") || "wayland-0").split("/").pop();
        return `${runtime}/glint/${display}.sock`;
    }

    property int nextId: 1
    property var pending: ({})

    signal response(int requestId, string operation, var result)
    signal requestFailed(int requestId, string operation, string message)
    signal eventReceived(string event, var data)

    function send(operation, fields) {
        if (!socket.connected) {
            requestFailed(-1, operation, "Glint is not connected");
            return -1;
        }
        const id = nextId++;
        const request = Object.assign({}, fields || {}, { id: id, op: operation });
        pending[id] = operation;
        socket.write(JSON.stringify(request) + "\n");
        socket.flush();
        return id;
    }

    function outputs() { return send("outputs"); }
    function list(search) { return send("list", { search: search || "" }); }
    function inspect(path) { return send("inspect", { path: path }); }
    function setWallpaper(output, path, options) {
        return send("set", { output: output, path: path, options: options || {} });
    }
    function settings(output) { return send("props", { output: output }); }
    function setSettings(output, values) { return send("props", { output: output, set: values }); }
    function resetSettings(output) { return send("props", { output: output, reset: true }); }
    function pause(output, reason) {
        return send("pause", { output: output || "*", reason: reason || "request" });
    }
    function resume(output, reason) {
        return send("resume", { output: output || "*", reason: reason || "request" });
    }

    function receive(line) {
        let message;
        try {
            message = JSON.parse(line);
        } catch (error) {
            console.warn(`glint: invalid JSON: ${error}`);
            return;
        }
        if (message === null || typeof message !== "object" || Array.isArray(message))
            return;
        if (message.event) {
            eventReceived(message.event, message);
            return;
        }
        if (message.id === undefined || !(message.id in pending))
            return;
        const operation = pending[message.id];
        delete pending[message.id];
        if (message.ok)
            response(message.id, operation, message.result);
        else
            requestFailed(message.id, operation, message.error || "Request failed");
    }

    function connectSocket() {
        if (enabled && socketPath && !socket.connected)
            socket.connected = true;
    }

    Component.onCompleted: connectSocket()
    onEnabledChanged: {
        if (enabled)
            connectSocket();
        else
            socket.connected = false;
    }

    Socket {
        id: socket
        path: root.socketPath

        onConnectionStateChanged: {
            if (connected) {
                root.send("subscribe", { events: ["state", "props", "outputs", "error"] });
            } else {
                const old = root.pending;
                root.pending = {};
                for (const id in old)
                    root.requestFailed(Number(id), old[id], "Glint disconnected");
            }
        }

        parser: SplitParser {
            splitMarker: "\n"
            onRead: data => root.receive(data)
        }
    }

    Timer {
        interval: 2000
        repeat: true
        running: root.enabled && !socket.connected && root.socketPath !== ""
        onTriggered: root.connectSocket()
    }
}

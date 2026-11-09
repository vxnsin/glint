import QtQuick
import Quickshell
import "." as Facet

ShellRoot {
    Facet.GlintClient {
        id: client
        enabled: false

        property int receivedReplies: 0
        property int receivedErrors: 0
        property int receivedEvents: 0

        onResponse: (id, operation, result) => {
            if (id === 2 && operation === "outputs" && result[0] === "DP-1")
                receivedReplies++;
        }
        onRequestFailed: (id, operation, message) => {
            if (id === 1 && operation === "list" && message === "not found")
                receivedErrors++;
        }
        onEventReceived: (event, data) => {
            if (event === "state" && data.output === "DP-1")
                receivedEvents++;
        }
    }

    Timer {
        interval: 50
        running: true
        onTriggered: {
            client.pending = ({ 1: "list", 2: "outputs" });
            client.receive('{"id":2,"ok":true,"result":["DP-1"]}');
            client.receive('{"event":"state","output":"DP-1"}');
            client.receive('{"id":1,"ok":false,"error":"not found"}');
            client.receive('{"id":99,"ok":true,"result":null}');
            client.receive('null');
            client.receive('[]');
            if (client.receivedReplies !== 1 || client.receivedErrors !== 1 ||
                    client.receivedEvents !== 1 || Object.keys(client.pending).length !== 0) {
                console.error("Glint reply dispatch failed");
                Qt.exit(1);
            } else {
                console.log("Glint reply dispatch passed");
                Qt.quit();
            }
        }
    }
}

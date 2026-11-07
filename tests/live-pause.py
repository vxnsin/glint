#!/usr/bin/env python3
"""Opt-in regression against a running video wallpaper daemon: --live."""

import argparse
import json
import os
from pathlib import Path
import socket
import time


def request(op, **fields):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    display = Path(os.environ.get("WAYLAND_DISPLAY", "wayland-0")).name
    path = os.environ.get("GLINT_SOCKET", str(runtime / "glint" / (display + ".sock")))
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(15)
        connection.connect(path)
        connection.sendall((json.dumps({"id": 1, "op": op, **fields}) + "\n").encode())
        with connection.makefile("r") as stream:
            reply = json.loads(stream.readline())
    assert reply["ok"], reply
    return reply.get("result")


def frozen(label):
    first = request("status")
    time.sleep(2)
    last = request("status")
    for name, before in first.items():
        after = last[name]
        assert after["state"] == "paused", (name, after)
        assert before["frames"] == after["frames"], (name, before, after)
        assert after["video"]["running_instances"] == 0, (name, after)
        assert before["video"]["position"] == after["video"]["position"], (name, before, after)
        assert not after["current_wants_frames"] and not after["incoming_wants_frames"]
    print(label + ": frozen clock, decoder and frame counters", flush=True)
    return last


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true", required=True)
    parser.parse_args()
    state_root = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state")))
    display = Path(os.environ.get("WAYLAND_DISPLAY", "wayland-0")).name
    state_name = "state.json" if display in ("wayland-0", "wayland-1") else f"state-{display}.json"
    state_path = state_root / "glint" / state_name
    saved = json.loads(state_path.read_text())
    status = request("status")
    assert status and all(s.get("kind") == "video" for s in status.values()), "Use video wallpapers"
    assert all("request" not in s["paused_reasons"] for s in status.values()), "Already manually paused"
    assert all(s["options"].get("speed", 1) == 1 for s in status.values()), "Use normal playback speed"
    paused = False
    try:
        request("pause", output="*", reason="request")
        paused = True
        frozen("Existing wallpapers")
        # Reapply the original assignments while covered, including their project paths/options.
        assignments = []
        if saved.get("all"):
            assignments.append(("*", saved["all"]))
        assignments.extend(saved.get("outputs", {}).items())
        for output, assignment in assignments:
            request("set", output=output, **assignment)
        before = frozen("Wallpaper loaded during pause")
        request("resume", output="*", reason="request")
        paused = False
        started = time.monotonic()
        time.sleep(1)
        after = request("status")
        elapsed = time.monotonic() - started
        running = [name for name, s in after.items() if s["state"] == "running" and s["fps"] > 0]
        assert running, after
        for name in running:
            advance = after[name]["video"]["position"] - before[name]["video"]["position"]
            assert abs(advance - elapsed) < 0.2, (name, advance, elapsed)
            lag = after[name]["video"]["position"] - after[name]["video"]["decoded_position"]
            assert abs(lag) < 0.2, (name, lag)
        print("Resume: normal playback speed without a decode backlog", flush=True)
        assert json.loads(state_path.read_text()) == saved, "Saved assignments/options changed"
    finally:
        if paused:
            request("resume", output="*", reason="request")


if __name__ == "__main__":
    main()

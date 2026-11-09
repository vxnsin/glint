#!/usr/bin/env python3
"""Opt-in global settings regression against a running daemon: --live."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("live_pause", Path(__file__).with_name("live-pause.py"))
pause = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pause)
request = pause.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live", action="store_true", required=True)
    parser.add_argument("--preview", type=Path, required=True)
    args = parser.parse_args()
    preview = args.preview.resolve(strict=True)
    state_root = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state")))
    display = Path(os.environ.get("WAYLAND_DISPLAY", "wayland-0")).name
    name = "state.json" if display in ("wayland-0", "wayland-1") else f"state-{display}.json"
    state_path = state_root / "glint" / name
    before = json.loads(state_path.read_text())
    defaults = request("settings")
    statuses = request("status")
    assert statuses
    target = sorted(statuses)[-1]
    assert "request" not in statuses[target]["paused_reasons"], "Already manually paused"
    changed = 72 if defaults["fps"] != 72 else 120
    held = False
    try:
        request("settings", set={"fps": changed})
        current = request("status")
        for output, state in statuses.items():
            assert current[output]["path"] == state["path"], "Global FPS replaced a wallpaper"
            assert current[output]["options"]["fps"] == changed, (output, current)
        for invalid in ({"fps": -1}, {"fps": 120, "properties": {"clock": False}}):
            try:
                request("settings", set=invalid)
            except AssertionError:
                pass
            else:
                raise AssertionError("Invalid settings accepted")
            assert request("settings")["fps"] == changed, "Rejected update changed defaults"
        request("pause", output=target, reason="request")
        held = True
        request("set", output=target, path=str(preview))
        assert request("status")[target]["options"]["fps"] == changed, "New wallpaper lost defaults"
        saved = json.loads(state_path.read_text())
        assert saved["settings"]["fps"] == changed, "Defaults not persisted"
        assert saved.get("properties") == before.get("properties"), "Authored settings changed"
        print("Global updates, validation, new wallpaper inheritance and persistence passed")
    finally:
        request("settings", set=defaults)
        assignments = []
        if before.get("all"):
            assignments.append(("*", before["all"]))
        assignments.extend(before.get("outputs", {}).items())
        for output, assignment in assignments:
            request("set", output=output, path=assignment["path"],
                    options={**assignment.get("options", {}), **defaults})
        if target not in before.get("outputs", {}) and not before.get("all"):
            request("unset", output=target)
        if held:
            request("resume", output=target, reason="request")
    print("Original wallpapers restored with global defaults")


if __name__ == "__main__":
    main()

# Current status

This is the current implementation checklist. PLAN.md describes the intended product and
historical milestones; it is not a list of features already implemented.

## Working

- Wayland wallpaper daemon, Vulkan renderer, still pictures and videos, per-output
  assignments, state restore, screenshots, frame caps and stacking pause reasons.
- Wallpaper Engine scene readers, materials, effect chains, particle simulation on the
  CPU, puppet animation, timelines, SceneScript, text rasterization, audio capture and MPRIS.
- Embedded video textures, persistent effect targets and effect copy commands.
- Worker-side graphics pipeline preparation, incremental scene uploads and a crossfade
  clock that starts with the first submitted frame.
- Named scene packages derived from the project's entry point, including gifscene.pkg.
- Live text string and style changes from scripts: font, size, padding, alignment,
  wrapping, row limits and background. Identical assignments do not rerasterize text.
- Named puppet animation-layer lookup and real clip metadata, with frame seeking,
  pause/resume and continuous rate changes.
- Offline `glint list [search]` and `glint inspect <id|folder>`; JSON output and
  `GLINT_WORKSHOP` override. Explicit set commands also accept bare Workshop IDs.
- `glint configure <output>` provides a terminal menu for the visible settings of the
  wallpaper currently on that output.
- Local per-wallpaper script-property overrides can adjust authored script settings that
  the wallpaper does not expose as project properties; they are read when a scene loads.
- The socket exposes installed Workshop listing and project inspection on workers, with
  an optional Quickshell client component for requests and events.

## Remaining

- Interactive wallpaper selection, `doctor` and `install`.
- Daemon-wide settings IPC, UPower/idle integration and configurable window rules.
  The pure policy logic and the
  built-in fullscreen pause exist, but the full planned policy integration does not.
- Independent per-output audio control; sharing scene sound currently prevents duplicates.
- Perspective/3D model rendering, lights, camera paths/shake and scene-wide bloom.
- Bone physics and quaternion pose blending. The bone hierarchy's rotation behavior still
  needs stronger reference validation across rigs.
- Native text outline/blur/shadow settings. Authored image effects on text are separate.
- Further SceneScript APIs, including reparenting, creating/destroying animation layers,
  animation-ended callbacks and texture/video animation control.
- Editing script-property overrides in the settings menu; currently those overrides are
  configured as local JSON files.
- Effect swap commands and the remaining particle modules; particles currently run on the
  CPU rather than the planned compute path.
- Optional web wallpapers, NVIDIA validation, glint-app and wiring the Quickshell client
  into Facet's wallpaper app, launcher and lockscreen.

## Verification

The user has checked Roxy, the flower-tower Frieren, Yosuga, several Hikari/Nozomi scenes,
Hoshino and two Arona scenes. After the general rotation change, the user confirmed that
Arona (2919051696) looks correct, Yosuga's hair now animates as expected, and Frieren and
Nozomi Hikari 131121999_p0 also look right. More scenes still need rechecking.
The corrected Yosuga strand is continuous in the user's GPU
capture. The user reported no stutter on the Hoshino switch after incremental loading;
this is one successful switch test, not a guarantee for every scene.

Automated suites cover readers, scheduling, transitions, policy, audio spectrum,
SceneScript and library inspection. A CPU script sweep covers the 96 installed scenes
rated Everyone. Passing these checks does not verify their final Vulkan output. The current
agent environment does not expose GPU devices; visual verification of new renderer changes
requires running the build in the desktop session. Measurements and limitations are in LOG.md.

# Terminal UI

Run `glint` in a terminal, or `glint tui`. `glint menu` is an alias. The daemon runs
separately with `glint --daemon`. Existing unattended starts without a terminal still run
the daemon. Opening the TUI while the daemon is running connects to it.

The TUI uses ncurses with wide-character support (`ncurses` on Arch), a UTF-8 terminal,
and at least 76 columns and 22 rows. Previews use FFmpeg's command-line executable
to decode one image frame. Ghostty and Kitty use the [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/)
for higher-resolution RGB previews; other terminals use half-block characters and the
terminal's 256-color palette. Automatic image-protocol detection is disabled inside
terminal multiplexers. `GLINT_TUI_IMAGES=symbols` forces the character preview;
`GLINT_TUI_IMAGES=kitty` forces the image protocol in a compatible direct terminal.
`NO_COLOR` disables colors
and previews. The library remains usable when the daemon is offline; applying wallpapers
and editing their settings needs a running daemon.

## Controls

| Key | Action |
| --- | --- |
| Up / Down, j / k | Select an item |
| Page Up / Page Down | Move through a long list |
| Enter | Apply a wallpaper or edit the selected setting |
| Tab / Shift+Tab | Switch between Library, Displays, Settings and Workshop |
| 1–4 | Open a page directly |
| / | Search installed titles, IDs and tags |
| o | Choose the display for subsequent actions |
| e | Open settings for the current wallpaper |
| f | Apply an image, video, project folder or Workshop ID by path |
| p / u | Pause / resume the selected display, or all displays |
| r | Refresh installed wallpapers and display state |
| Esc / q | Close a picker, or exit the TUI |

Text prompts accept Unicode, Backspace, Enter to save and Esc to cancel. The TUI keeps
responding while the daemon loads a wallpaper. Exiting cancels the client's pending wait;
an already accepted wallpaper change may still finish in the daemon.

## Settings

The Settings page follows the selected display target. Choose **All displays** with `o`
or from the Displays page to edit general settings across all active wallpapers.
Different values are shown as **Mixed**. Each display keeps its own wallpaper path and
other options; changing FPS on all displays does not copy one display's wallpaper to the
others. Displays without an assigned wallpaper are skipped. Failed changes report the
affected display rather than claiming that everything was saved.

Wallpaper-specific project properties can be edited together when all active displays
show the same project. When they show different projects, select an individual display
with `o` to edit its project properties.

- FPS: display refresh rate, presets, or a custom cap from 0 to 1000.
- Scaling: fill, fit, center, tile or stretch.
- Crossfade on/off and transition duration.
- Sound volume, mute and background color.
- Video playback speed and hardware decoding mode.
- The project's visible settings, including effect switches, sliders, colors and text.

There is no invented quality preset: the renderer's actual performance controls are FPS
and the effects exposed by each project. There is no general resolution/quality setting
yet. General display options reapply the current wallpaper with its other options
preserved. The old frame stays visible while the new assignment loads. Project properties
use live `props` requests. The daemon persists both kinds of changes.

## Workshop

The Library includes installed Wallpaper Engine items and registered SteamCMD downloads.
Online search and item pages open in the Steam client. Steam subscriptions continue to
download through Steam. The Workshop page refreshes the installed library every five
seconds while open; `r` refreshes it on demand.

For a direct download, select **Download with SteamCMD**, enter an item ID or Workshop
link and your Steam account name. The optional `steamcmd` executable must be installed.
Glint temporarily gives the terminal to SteamCMD, which prompts for its own password and
Steam Guard code. No password is passed by Glint or stored in its configuration. Use an
account entitled to Wallpaper Engine; Steam determines which files it may download.

After SteamCMD finishes, Glint checks its usual download locations and registers a
complete scene/video project. If SteamCMD uses a different location, choose **Import
downloaded folder** and enter the folder printed by SteamCMD. A failed download is not
treated as successful merely because SteamCMD exits with status zero.

The same operations are available without the TUI:

```sh
glint download 2919051696 your_steam_account
glint import /path/to/steamapps/workshop/content/431960/2919051696
glint list arona
```

Registered downloads are links under `$XDG_DATA_HOME/glint/workshop/431960` (normally
`~/.local/share/glint/workshop/431960`). The downloaded files stay in their original
folders, which must remain available. Existing library entries are never overwritten by
an import. Steam's installed item wins when both libraries contain the same ID.

SteamCMD authentication and real downloads need a desktop/account check. Automated tests
cover the handoff, command arguments, importing completed downloads and the return to the
TUI with a simulated SteamCMD, without using an account or network.

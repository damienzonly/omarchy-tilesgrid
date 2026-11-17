# tilesgrid

A static, paginated 3×3 workspace manager for [Omarchy](https://omarchy.org/) on
Hyprland. Nine workspaces per page, three pages per monitor, live window previews in
the overview, and a bar indicator that replaces Omarchy's stock workspace widget.

`tilesgrid` is a fork of [hyprtasking](https://github.com/raybbian/hyprtasking) with
its dynamic workspace↔tile cache removed. Every workspace ID is fixed by the monitor's
base and the page/tile formula, so windows can never drift into a tile you cannot
reach. There is no `hyprpm`, no upstream plugin, and no second workspace manager.

## What you get

- **`SUPER + Tab`** — grid overview with live previews of the nine tiles and a page
  strip at the bottom.
- **Left click a tile** — focus that workspace and close the overview.
- **Right-drag a window** onto another tile — move it there. Drop it on a page tab to
  move it into the first empty tile of that page. An invalid drop cancels; the window
  stays where it was.
- **Click a page tab / scroll over the strip** — preview another page. Page tabs never
  change the active page, so closing the overview returns to the real active tile.
- **`SUPER + 1..9`** — focus tile N of the active tile's page on the monitor under the
  cursor. `SUPER + SHIFT + N` moves the focused window there and follows;
  `SUPER + SHIFT + ALT + N` moves it silently.
- **Recovered pages** — numbered workspaces outside the map (for example after an
  update that renumbers something) appear on `R1`, `R2`, … tabs. They are reachable
  but never moved automatically. Drag one onto a normal page tab to adopt its window.
- **Monitor unplug** — orphaned windows are moved one per empty tile on the focused
  monitor's current page; overflow stays on its migrated workspace and appears on
  Recovered. This is the only automatic window move.
- **Bar widget** — nine tile buttons plus a page indicator (`1/3` or `R1`), scoped to
  its own monitor, replacing `omarchy.workspaces` and any other `*.workspaces` widget.

Regular **named** workspaces (e.g. `workspace = name:code`) are not part of the tile
map; special/scratchpad workspaces are intentionally excluded from it.

## Requirements

- Omarchy 4.5+ with its Hyprland/Lua configuration.
- Hyprland headers matching the running compositor (`hyprland` package).
- Build tools: `meson`, `ninja`, `g++` (C++23), `pkg-config`, `python3`.

The native plugin is compiled against your installed Hyprland headers. A Hyprland
update changes the plugin ABI, so the plugin must be rebuilt after every Hyprland
update. The bundled post-update hook does this automatically and asks you to restart
Hyprland.

## Install

```bash
omarchy plugin add https://github.com/damienzonly/omarchy-tilesgrid --enable
```

On the next shell start the bar widget runs the package's installer once. The
installer:

1. builds `native/` against your Hyprland headers and installs
   `~/.local/lib/hyprland/libtilesgrid.so`;
2. installs the workspace-manager shadow at
   `~/.config/default/hypr/monitor-workspaces.lua`, which disables Omarchy's
   reconciler (the component that renumbers workspaces in blocks of ten);
3. installs `~/.local/state/omarchy/toggles/hypr/tilesgrid.lua`, which loads the
   plugin, pins 3 × 9 workspaces per monitor, writes the shared map, and owns the
   workspace keybindings;
4. replaces `omarchy.workspaces` / `*.workspaces` entries in
   `~/.config/omarchy/shell.json` with `tilesgrid`;
5. installs the post-update rebuild hook and reloads Hyprland.

If something does not appear immediately, run the installer directly or restart:

```bash
~/.config/omarchy/plugins/tilesgrid/install.sh
omarchy restart shell
```

A first install that happens while Hyprland is already running may require a Hyprland
restart to load the native plugin; the installer tells you when that is the case.

### Coming from hyprtasking

If the upstream hyprtasking plugin is still enabled through `hyprpm`, the installer
runs `hyprpm disable hyprtasking` (both plugins hook the same compositor functions, so
only one can be loaded). If that fails, run it yourself and restart Hyprland; the
installer shows a notification with the command. Once tilesgrid loads, old page-1
workspaces keep their IDs: the installer seeds each monitor's base from Omarchy's
existing monitor block state when available.

### Manual install from a checkout

```bash
git clone https://github.com/damienzonly/omarchy-tilesgrid
cd omarchy-tilesgrid
./install.sh
# or, to install the widget itself through Omarchy:
omarchy plugin add . --enable
```

## Workspace map

Each monitor gets a stable base assigned in this order:

1. your own `monitor_workspace_offset(name)` function, if the old Omarchy config
   defined one;
2. otherwise the lowest free base in `0, 10, 20, …`.

The assignment is stored in `~/.local/state/tilesgrid/monitor-bases` and mirrored to
`~/.local/state/tilesgrid/map.conf`, which the plugin and the bar widget both read.

```
workspace_id = monitor_base + page * 100 + tile
  page = 0, 1, 2   (shown as 1, 2, 3)
  tile = 1..9
```

Page 1 keeps the traditional per-monitor IDs (`1–9`, `11–19`, `21–29`), so existing
windows stay where they are. A monitor's base never changes while its description is
unchanged; a reconnect keeps its page IDs.

## Updating

```bash
omarchy plugin update tilesgrid
omarchy restart shell
```

Then restart Hyprland if the installer says the native plugin was rebuilt. From a
checkout, `git pull && ./install.sh` does the same.

## Uninstall

```bash
~/.config/omarchy/plugins/tilesgrid/uninstall.sh
omarchy plugin remove tilesgrid
```

The uninstaller restores the backed-up workspace shadow, post-update hook, and
`shell.json` layout, unloads and deletes the native plugin, and restores the stock
workspace widget. Restart Hyprland afterwards. Your monitors' page-1 workspaces are
preserved; page 2/3 workspaces (`1xx`, `2xx`) remain as ordinary numbered workspaces
and can be deleted normally.

## Configuration

The installer writes `~/.local/state/omarchy/toggles/hypr/tilesgrid.lua`. It is
regenerated on update, so put local tweaks in a separate config file loaded after it.
Plugin options live under `plugin:tilesgrid:` and are read from
`hl.config({ plugin = { tilesgrid = { … } } })`:

| Option | Default | Meaning |
|---|---|---|
| `layout` | `"grid"` | Layout name |
| `drag_button` | right mouse | Button that starts a window drag |
| `select_button` | left mouse | Button that selects a tile |
| `bg_color` | `0xFF1E1E2E` | Overview background |
| `page_strip` | enabled | Page tab strip at the bottom |

The native plugin exposes `hl.plugin.tilesgrid.focus(n)`,
`hl.plugin.tilesgrid.move_to_tile(n, follow)`, and
`hl.plugin.tilesgrid.focus_id(id, monitor)` for your own bindings.

## GPU rendering

The overview is rendered by Hyprland's OpenGL renderer in the compositor's render
pass — the tiles are live textures, not screenshots, and nothing is composed on the
CPU. Only the small page-tab glyphs are rasterized once and cached as textures.

## Security note

This package contains a native Hyprland plugin, which runs with the compositor's full
privileges. It is built locally from the sources in `native/`, and the installer
refuses to run as root. Read `native/src/` before installing if that matters to you.

## Upstream and license

The fork is based on hyprtasking by raybbian, pinned to the Hyprland 0.56.x API, and
retains its BSD-3-Clause license and copyright. See [LICENSE](LICENSE). The
tilesgrid-specific changes (static mapping, pages, Recovered pages, monitor recovery,
Omarchy integration) are covered by the same license.

## Troubleshooting

- **Plugin did not load** — check `hyprctl plugin list` and
  `~/.local/state/tilesgrid/install.log`; usually the headers do not match the running
  Hyprland, so reinstall/rebuild and restart.
- **Workspaces renumbered after an update** — verify
  `~/.config/default/hypr/monitor-workspaces.lua` exists; it must shadow Omarchy's
  module. Then run `hyprctl reload`.
- **Bar still shows the old indicator** — `omarchy restart shell`, then
  `omarchy plugin validate ~/.config/omarchy/plugins/tilesgrid`.
- **A window is on a `R` page** — that workspace is outside the fixed map. Drag it
  onto a normal page tab, or focus it and use `SUPER + SHIFT + N` to move it into tile
  N of the active page.

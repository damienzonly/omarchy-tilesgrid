#!/usr/bin/env bash
set -euo pipefail

# tilesgrid uninstaller: removes the native plugin and the Hyprland/Omarchy
# integration, restores the stock workspace shadow and bar widget when backed
# up, and leaves the shell plugin folder for `omarchy plugin remove tilesgrid`.

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
STATE_DIR="$STATE_HOME/tilesgrid"
INSTALL_DIR="$HOME/.local/lib/hyprland"
SHADOW_FILE="$HOME/.config/default/hypr/monitor-workspaces.lua"
TOGGLE_FILE="$STATE_HOME/omarchy/toggles/hypr/tilesgrid.lua"
HOOK_FILE="$HOME/.config/omarchy/hooks/post-update.d/tilesgrid-rebuild.hook"
SHELL_JSON="$HOME/.config/omarchy/shell.json"
BACKUP_DIR="$STATE_DIR/backup"

notify() {
  command -v omarchy-notification-send >/dev/null 2>&1 || return 0
  omarchy-notification-send -u "$1" -g "󰐃" "$2" "$3" 2>/dev/null || true
}

mkdir -p "$STATE_DIR"
if command -v flock >/dev/null 2>&1; then
  exec 9>"$STATE_DIR/install.lock"
  flock -w 180 9 || {
    echo '[tilesgrid] another install/uninstall is still running' >&2
    exit 1
  }
fi

# Restore a backup only when it is not a copy of this package's own output
# (older installers could back up a file they had just installed).
restore_or_remove() {
  local backup="$1" target="$2" package="$3"
  if [[ -f "$backup" ]] && ! cmp -s "$backup" "$package"; then
    cp -a "$backup" "$target"
  else
    rm -f "$target"
  fi
}

restore_or_remove "$BACKUP_DIR/monitor-workspaces.lua" "$SHADOW_FILE" \
  "$PLUGIN_DIR/config/monitor-workspaces.lua"
restore_or_remove "$BACKUP_DIR/toggle.lua" "$TOGGLE_FILE" \
  "$PLUGIN_DIR/config/tilesgrid-toggle.lua"
restore_or_remove "$BACKUP_DIR/tilesgrid-rebuild.hook" "$HOOK_FILE" \
  "$PLUGIN_DIR/config/tilesgrid-rebuild.hook"

if [[ -f "$SHELL_JSON" ]]; then
  python3 - "$SHELL_JSON" "$BACKUP_DIR/shell.json" <<'PY'
import json, os, shutil, sys, tempfile

path, backup = sys.argv[1], sys.argv[2]
if os.path.exists(backup):
    shutil.copy2(backup, path)
    raise SystemExit(0)

with open(path) as f:
    config = json.load(f)
layout = config.setdefault("bar", {}).setdefault("layout", {})
placed = False
for section, items in list(layout.items()):
    if not isinstance(items, list):
        continue
    out = []
    for item in items:
        if isinstance(item, dict) and str(item.get("id", "")) == "tilesgrid":
            if not placed:
                out.append({"id": "omarchy.workspaces"})
                placed = True
            continue
        out.append(item)
    layout[section] = out

if not placed:
    left = layout.setdefault("left", [])
    left.insert(0, {"id": "omarchy.workspaces"})

fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".shell.json.")
with os.fdopen(fd, "w") as f:
    json.dump(config, f, indent=2)
    f.write("\n")
os.replace(tmp, path)
PY
fi

if [[ "${TILESGRID_SKIP_RELOAD:-0}" != "1" ]] && command -v hyprctl >/dev/null 2>&1; then
  hyprctl plugin unload "$INSTALL_DIR/libtilesgrid.so" 2>/dev/null || true
  hyprctl reload >/dev/null 2>&1 || true
fi

rm -f "$INSTALL_DIR/libtilesgrid.so"
rm -rf "$CACHE_HOME/tilesgrid"

if [[ "${TILESGRID_SKIP_RELOAD:-0}" != "1" ]] && command -v omarchy-shell >/dev/null 2>&1; then
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
  omarchy-shell shell reloadConfig >/dev/null 2>&1 \
    || omarchy-shell -q shell rescanPlugins >/dev/null 2>&1 || true
fi

notify low "tilesgrid removed" "The stock workspace manager is restored. Restart Hyprland to finish."
printf '[tilesgrid] uninstalled; run: omarchy plugin remove tilesgrid\n'

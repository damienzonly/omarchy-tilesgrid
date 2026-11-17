#!/usr/bin/env bash
set -euo pipefail

# tilesgrid installer: builds the native Hyprland plugin, installs the
# workspace-manager shadow/toggle, replaces the Omarchy bar workspace widget,
# and installs the post-update rebuild hook.
#
# Usage:
#   install.sh                 full install/update
#   install.sh --ensure        no-op if everything is current
#   install.sh --rebuild-only  rebuild/copy the .so only (update hook)

MODE="${1:-}"
PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$PLUGIN_DIR/native"

STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
STATE_DIR="$STATE_HOME/tilesgrid"
BUILD_DIR="$CACHE_HOME/tilesgrid/build"
INSTALL_DIR="$HOME/.local/lib/hyprland"
SHADOW_FILE="$HOME/.config/default/hypr/monitor-workspaces.lua"
TOGGLE_FILE="$STATE_HOME/omarchy/toggles/hypr/tilesgrid.lua"
HOOK_FILE="$HOME/.config/omarchy/hooks/post-update.d/tilesgrid-rebuild.hook"
SHELL_JSON="$HOME/.config/omarchy/shell.json"
BACKUP_DIR="$STATE_DIR/backup"
LOG_FILE="$STATE_DIR/install.log"

log() {
  printf '[tilesgrid] %s\n' "$*" | tee -a "$LOG_FILE" >&2
}

notify() {
  local urgency="$1" title="$2" body="$3"
  command -v omarchy-notification-send >/dev/null 2>&1 || return 0
  omarchy-notification-send -u "$urgency" -g "󰐃" "$title" "$body" 2>/dev/null || true
}

fail() {
  log "ERROR: $*"
  notify critical "tilesgrid install failed" "$*"
  exit 1
}

# hyprpm keeps the enabled/disabled state (and built plugins) in
# /var/cache/hyprpm/$USER. It must be writable by the user; a root-owned cache
# (from running hyprpm via sudo once) makes every disable/enable fail with
# "Failed to write plugin state".
HYPRPM_STATE_DIR="/var/cache/hyprpm/${USER}"
[[ -d "$HYPRPM_STATE_DIR" ]] || HYPRPM_STATE_DIR="$HOME/.cache/hyprpm"

hyprtasking_enabled() {
  command -v hyprpm >/dev/null 2>&1 || return 1
  hyprpm list 2>/dev/null | awk '
    /Plugin hyprtasking/ { found = 1; next }
    found && /enabled:/    { print; exit }
  ' | grep -qi 'true'
}

# hyprtasking and tilesgrid hook the same render/input functions. Hyprland
# refuses to hook a function twice ("function is already hooked"), so it is not
# enough for hyprpm to *record* hyprtasking as disabled: the running compositor
# also has to unload it before tilesgrid can initialise.
hyprtasking_loaded() {
  command -v hyprctl >/dev/null 2>&1 || return 1
  hyprctl plugin list 2>/dev/null | grep -qi 'Hyprtasking'
}

hyprtasking_so() {
  local candidate
  for candidate in \
    "$HYPRPM_STATE_DIR/hyprtasking/hyprtasking.so" \
    "$HOME/.cache/hyprpm/hyprtasking/hyprtasking.so" \
    "$HOME/.cache/hyprpm/${USER}/hyprtasking/hyprtasking.so" \
    "$HOME/.local/share/hyprpm/hyprtasking/hyprtasking.so"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

[[ $EUID -ne 0 ]] || fail "run install.sh as your user, not root"

mkdir -p "$STATE_DIR" "$BACKUP_DIR"
touch "$LOG_FILE"

# A bar widget on every monitor can trigger --ensure at the same shell start.
# Serialize installers so they do not race on the build directory or shell.json.
if command -v flock >/dev/null 2>&1; then
  exec 9>"$STATE_DIR/install.lock"
  if [[ "$MODE" == "--ensure" ]]; then
    # A widget re-created by the reload can run this while the installer that
    # triggered the reload still holds the lock. It will finish the job.
    flock -n 9 || exit 0
  else
    flock -w 180 9 || {
      log "another tilesgrid install is still running; giving up"
      exit 1
    }
  fi
fi

shell_layout_ok() {
  [[ -f "$SHELL_JSON" ]] || return 0
  python3 - "$SHELL_JSON" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as f:
        cfg = json.load(f)
except Exception:
    sys.exit(1)
items = []
for value in (cfg.get("bar", {}).get("layout", {}) or {}).values():
    if isinstance(value, list):
        items.extend(value)
ids = [str(item.get("id", "")) for item in items if isinstance(item, dict)]
def workspace_like(ident):
    return ident == "omarchy.workspaces" or ident.endswith(".workspaces")
if "tilesgrid" not in ids:
    sys.exit(1)
if any(workspace_like(ident) for ident in ids):
    sys.exit(1)
PY
}

up_to_date() {
  [[ -f "$INSTALL_DIR/libtilesgrid.so" ]] || return 1
  [[ -f "$SHADOW_FILE" ]] || return 1
  [[ -f "$TOGGLE_FILE" ]] || return 1
  [[ -f "$HOOK_FILE" ]] || return 1
  cmp -s "$SHADOW_FILE" "$PLUGIN_DIR/config/monitor-workspaces.lua" || return 1
  cmp -s "$TOGGLE_FILE" "$PLUGIN_DIR/config/tilesgrid-toggle.lua" || return 1
  cmp -s "$HOOK_FILE" "$PLUGIN_DIR/config/tilesgrid-rebuild.hook" || return 1
  if find "$NATIVE_DIR/src" "$NATIVE_DIR/meson.build" -type f \
      -newer "$INSTALL_DIR/libtilesgrid.so" -print -quit | grep -q .; then
    return 1
  fi
  shell_layout_ok
}

if [[ "$MODE" == "--ensure" ]] && up_to_date; then
  exit 0
fi

for tool in meson ninja g++ pkg-config python3 install; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing dependency: $tool"
done

# --- Native plugin ---------------------------------------------------------

build_needed=0
[[ -f "$INSTALL_DIR/libtilesgrid.so" ]] || build_needed=1
if [[ -f "$INSTALL_DIR/libtilesgrid.so" ]] && find "$NATIVE_DIR/src" "$NATIVE_DIR/meson.build" \
    -type f -newer "$INSTALL_DIR/libtilesgrid.so" -print -quit | grep -q .; then
  build_needed=1
fi
if [[ "$MODE" == "--rebuild-only" ]]; then
  build_needed=1
fi

if (( build_needed )); then
  log "building native plugin"
  if [[ -d "$BUILD_DIR" ]]; then
    meson setup --reconfigure "$BUILD_DIR" "$NATIVE_DIR" --buildtype=release >>"$LOG_FILE" 2>&1 \
      || fail "meson reconfigure failed; see $LOG_FILE"
  else
    meson setup "$BUILD_DIR" "$NATIVE_DIR" --buildtype=release >>"$LOG_FILE" 2>&1 \
      || fail "meson setup failed; see $LOG_FILE"
  fi
  meson compile -C "$BUILD_DIR" >>"$LOG_FILE" 2>&1 \
    || fail "native build failed; see $LOG_FILE (Hyprland headers may not match)"

  mkdir -p "$INSTALL_DIR"
  tmp="$INSTALL_DIR/.libtilesgrid.so.tmp.$$"
  cp "$BUILD_DIR/libtilesgrid.so" "$tmp"
  mv -f "$tmp" "$INSTALL_DIR/libtilesgrid.so"
  log "installed $INSTALL_DIR/libtilesgrid.so"
fi

if [[ "$MODE" == "--rebuild-only" ]]; then
  notify low "tilesgrid rebuilt" "Restart Hyprland to load the new plugin"
  exit 0
fi

# --- Hyprland/Omarchy config ----------------------------------------------

# Back up only files that are about to be replaced for the first time. Files that
# already match this package are ours from an earlier install, so re-installing
# must not turn our own output into the "restore" target.
backup_if_replaced() {
  local source="$1" target="$2" package="$3"
  [[ -e "$source" ]] || return 0
  [[ -e "$target" ]] && return 0
  if [[ -n "$package" ]] && cmp -s "$source" "$package"; then
    return 0
  fi
  cp -a "$source" "$target"
}

backup_if_replaced "$SHADOW_FILE" "$BACKUP_DIR/monitor-workspaces.lua" \
  "$PLUGIN_DIR/config/monitor-workspaces.lua"
backup_if_replaced "$TOGGLE_FILE" "$BACKUP_DIR/toggle.lua" \
  "$PLUGIN_DIR/config/tilesgrid-toggle.lua"
backup_if_replaced "$HOOK_FILE" "$BACKUP_DIR/tilesgrid-rebuild.hook" \
  "$PLUGIN_DIR/config/tilesgrid-rebuild.hook"

if [[ -f "$SHELL_JSON" && ! -e "$BACKUP_DIR/shell.json" ]]; then
  python3 - "$SHELL_JSON" "$BACKUP_DIR/shell.json" <<'PY' >>"$LOG_FILE" 2>&1 || true
import json, shutil, sys

source, target = sys.argv[1], sys.argv[2]
try:
    with open(source) as f:
        config = json.load(f)
except Exception:
    shutil.copy2(source, target)
    raise SystemExit(0)

items = []
for value in (config.get("bar", {}).get("layout", {}) or {}).values():
    if isinstance(value, list):
        items.extend(value)
ids = [str(item.get("id", "")) for item in items if isinstance(item, dict)]
workspace_like = any(
    ident == "omarchy.workspaces" or ident.endswith(".workspaces")
    for ident in ids
)
# A layout that already contains only tilesgrid is this package's own output.
if workspace_like or "tilesgrid" not in ids:
    shutil.copy2(source, target)
PY
fi

install -Dm644 "$PLUGIN_DIR/config/monitor-workspaces.lua" "$SHADOW_FILE"
install -Dm644 "$PLUGIN_DIR/config/tilesgrid-toggle.lua" "$TOGGLE_FILE"
install -Dm755 "$PLUGIN_DIR/config/tilesgrid-rebuild.hook" "$HOOK_FILE"

if [[ -f "$SHELL_JSON" ]]; then
  python3 - "$SHELL_JSON" <<'PY' >>"$LOG_FILE" 2>&1
import json, os, sys, tempfile

path = sys.argv[1]
with open(path) as f:
    config = json.load(f)

layout = config.setdefault("bar", {}).setdefault("layout", {})

def workspace_like(entry):
    if not isinstance(entry, dict):
        return False
    ident = str(entry.get("id", ""))
    return ident == "omarchy.workspaces" or ident.endswith(".workspaces")

placed = False
for section, items in list(layout.items()):
    if not isinstance(items, list):
        continue
    out = []
    for item in items:
        if isinstance(item, dict) and str(item.get("id", "")) == "tilesgrid":
            if not placed:
                out.append(item)
                placed = True
            continue
        if workspace_like(item):
            if not placed:
                out.append({"id": "tilesgrid"})
                placed = True
            continue
        out.append(item)
    layout[section] = out

if not placed:
    left = layout.setdefault("left", [])
    index = 0
    for i, item in enumerate(left):
        if isinstance(item, dict) and item.get("id") == "omarchy.menu":
            index = i + 1
            break
    left.insert(index, {"id": "tilesgrid"})

fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".shell.json.")
with os.fdopen(fd, "w") as f:
    json.dump(config, f, indent=2)
    f.write("\n")
os.replace(tmp, path)
PY
fi

# Upstream hyprtasking hooks the same render/input functions. Disable it while
# it is actually enabled (hyprpm list still mentions the repository once it is
# disabled), and report the real error instead of swallowing it.
if hyprtasking_enabled; then
  log "disabling upstream hyprtasking"
  if ! hyprpm_error="$(hyprpm disable hyprtasking 2>&1)"; then
    log "hyprpm disable failed: ${hyprpm_error:-unknown error}"
    if [[ -d "$HYPRPM_STATE_DIR" && ! -w "$HYPRPM_STATE_DIR" ]]; then
      notify critical "hyprtasking is still enabled" \
        "hyprpm cannot write $HYPRPM_STATE_DIR (owner: $(stat -c '%U' "$HYPRPM_STATE_DIR" 2>/dev/null || echo unknown)). Run: sudo chown -R $USER $HYPRPM_STATE_DIR, then re-run this installer"
    else
      notify critical "hyprtasking is still enabled" \
        "hyprpm disable failed: ${hyprpm_error:-unknown error}. Run 'hyprpm disable hyprtasking' before restarting Hyprland"
    fi
  fi
fi

if [[ "${TILESGRID_SKIP_RELOAD:-0}" != "1" ]] && command -v hyprctl >/dev/null 2>&1; then
  plugin_loaded=0
  if hyprctl plugin list 2>/dev/null | grep -q 'tilesgrid'; then
    plugin_loaded=1
  fi

  # hyprpm disable only records the state; the running compositor keeps
  # hyprtasking loaded (and holding the hooks) until Hyprland restarts.
  # Unloading a hooking plugin at runtime is unsafe and no longer needed: the
  # toggle loads tilesgrid itself from the hyprland.start event, so a restart
  # is the safe way to switch.
  if (( ! plugin_loaded )) && hyprtasking_loaded; then
    log "hyprtasking is still loaded in this session; restart Hyprland to switch to tilesgrid"
    notify low "tilesgrid" "Restart Hyprland to switch from hyprtasking to tilesgrid"
  fi

  hyprctl reload >/dev/null 2>&1 || true
  sleep 1

  if ! hyprctl plugin list 2>/dev/null | grep -q 'tilesgrid'; then
    if hyprtasking_loaded; then
      notify critical "tilesgrid did not load" \
        "hyprtasking still holds the render/input hooks. Restart Hyprland to let tilesgrid load"
    else
      notify critical "tilesgrid did not load" \
        "Run: hyprctl plugin load $INSTALL_DIR/libtilesgrid.so and check hyprctl configerrors"
    fi
  elif (( plugin_loaded )); then
    notify normal "tilesgrid updated" "Restart Hyprland to load the new native build"
  else
    notify low "tilesgrid installed" "Workspace manager and bar indicator are active"
  fi

  errors="$(hyprctl configerrors 2>/dev/null || true)"
  if [[ -n "$errors" ]]; then
    log "config errors: $errors"
    notify critical "tilesgrid config errors" "$errors"
  fi
fi

if [[ "${TILESGRID_SKIP_RELOAD:-0}" != "1" ]] && command -v omarchy-shell >/dev/null 2>&1; then
  omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
  # Reload the shell's config from disk so the rewritten bar layout wins over
  # the copy the running shell still holds in memory.
  omarchy-shell shell reloadConfig >/dev/null 2>&1 \
    || omarchy-shell -q shell rescanPlugins >/dev/null 2>&1 || true
fi

log "done"

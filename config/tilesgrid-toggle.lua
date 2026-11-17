-- tilesgrid: installs the native plugin, pins the fixed 3x3 pages, and takes
-- over Omarchy's workspace keybindings. Installed by install.sh into
-- ~/.local/state/omarchy/toggles/hypr/tilesgrid.lua, which Omarchy's toggles
-- loader runs after the user's own Hyprland config.

local HOME = os.getenv("HOME") or ""
local STATE_HOME = os.getenv("XDG_STATE_HOME") or (HOME .. "/.local/state")
local STATE_DIR = STATE_HOME .. "/tilesgrid"
local BASES_FILE = STATE_DIR .. "/monitor-bases"
local MAP_FILE = STATE_DIR .. "/map.conf"

local PAGES = 3
local STRIDE = 100
local TILES = 9

local function ensure_state_dir()
  os.execute("mkdir -p " .. STATE_DIR)
end

local function read_bases()
  local bases = {}
  local file = io.open(BASES_FILE, "r")
  if not file then
    return bases
  end

  for line in file:lines() do
    local identity, base = line:match("^(.-)\t(%d+)$")
    if identity ~= nil and identity ~= "" then
      bases[identity] = tonumber(base)
    end
  end
  file:close()
  return bases
end

local function write_bases(bases)
  ensure_state_dir()
  local lines = {}
  for identity, base in pairs(bases) do
    lines[#lines + 1] = identity .. "\t" .. base
  end
  table.sort(lines)

  local tmp = BASES_FILE .. ".tmp"
  local file = io.open(tmp, "w")
  if not file then
    return
  end
  file:write(table.concat(lines, "\n") .. "\n")
  file:close()
  os.rename(tmp, BASES_FILE)
end

local duplicate_descriptions = {}

local function raw_identity_of(monitor)
  if monitor == nil then
    return ""
  end
  local description = monitor.description or ""
  if description ~= "" then
    return description
  end
  return monitor.name or ""
end

local function is_internal(monitor)
  local name = monitor.name or ""
  return name:match("^eDP") or name:match("^LVDS") or name:match("^DSI")
end

local function connected_monitors()
  local monitors = {}
  for _, monitor in ipairs(hl.get_monitors() or {}) do
    if monitor.name ~= "FALLBACK" then
      monitors[#monitors + 1] = monitor
    end
  end

  -- The laptop panel claims the first block, matching Omarchy's stock
  -- assignment so a migration does not shuffle its workspaces.
  table.sort(monitors, function(left, right)
    local left_internal = is_internal(left) and 1 or 0
    local right_internal = is_internal(right) and 1 or 0
    if left_internal ~= right_internal then
      return left_internal > right_internal
    end
    local left_identity = raw_identity_of(left)
    local right_identity = raw_identity_of(right)
    if left_identity == right_identity then
      return (left.name or "") < (right.name or "")
    end
    return left_identity < right_identity
  end)

  -- Two identical displays share a description. Key those by connector name so
  -- they cannot share a workspace base.
  local counts = {}
  for _, monitor in ipairs(monitors) do
    local description = monitor.description or ""
    if description ~= "" then
      counts[description] = (counts[description] or 0) + 1
    end
  end
  duplicate_descriptions = {}
  for description, count in pairs(counts) do
    if count > 1 then
      duplicate_descriptions[description] = true
    end
  end

  return monitors
end

-- Must be called after connected_monitors(): it refreshes the duplicate set.
local function identity_of(monitor)
  if monitor == nil then
    return ""
  end
  local description = monitor.description or ""
  if description ~= "" and not duplicate_descriptions[description] then
    return description
  end
  return monitor.name or ""
end

local bases = read_bases()

-- Omarchy's stock workspace manager stores each monitor's block (0, 1, ...) in
-- ~/.local/state/omarchy/monitor-workspaces; block N owns workspaces N*10+1
-- through N*10+10. Seeding from it keeps the page-1 workspace IDs a user already
-- has when tilesgrid takes over.
local function read_legacy_blocks()
  local result = { by_connector = {}, by_description = {}, by_name = {} }
  local file = io.open(STATE_HOME .. "/omarchy/monitor-workspaces", "r")
  if not file then
    return result
  end

  for line in file:lines() do
    local block, connector, identity = line:match("^(%d+)\t([^\t]*)\t(.+)$")
    if block ~= nil then
      local base = tonumber(block) * 10
      if base >= 0 and base < STRIDE then
        if connector ~= "" then
          result.by_connector[connector] = base
        end
        local description = identity:gsub(" #%d+$", "")
        if description ~= "" then
          result.by_description[description] = base
        end
        result.by_name[identity] = base
      end
    end
  end
  file:close()
  return result
end

local legacy_blocks = read_legacy_blocks()

local function legacy_base_for(monitor)
  local base = legacy_blocks.by_connector[monitor.name or ""]
  if base ~= nil then
    return base
  end

  local description = monitor.description or ""
  if description ~= "" then
    base = legacy_blocks.by_description[description]
    if base ~= nil then
      return base
    end
  end

  return legacy_blocks.by_name[monitor.name or ""]
end

local function base_used_by_other(identity, base)
  for other_identity, other_base in pairs(bases) do
    if other_identity ~= identity and other_base == base then
      return true
    end
  end
  return false
end

-- Assign stable bases to connected monitors. Existing tilesgrid bases win,
-- then a user's own monitor_workspace_offset(name), then a migrated stock
-- Omarchy block. Otherwise assign the lowest free 0/10/20... base.
for _, monitor in ipairs(connected_monitors()) do
  local identity = identity_of(monitor)
  local base = bases[identity]

  if base == nil and _G.monitor_workspace_offset ~= nil then
    local candidate = _G.monitor_workspace_offset(monitor.name)
    if type(candidate) == "number" and candidate >= 0 and candidate < STRIDE
      and not base_used_by_other(identity, candidate) then
      base = candidate
    end
  end

  if base == nil then
    local legacy = legacy_base_for(monitor)
    if legacy ~= nil and not base_used_by_other(identity, legacy) then
      base = legacy
    end
  end

  if base == nil or base_used_by_other(identity, base) then
    local candidate = 0
    while base_used_by_other(identity, candidate) do
      candidate = candidate + 10
    end
    base = candidate
  end

  bases[identity] = base
end

write_bases(bases)

local function base_for(monitor)
  return bases[identity_of(monitor)] or 0
end

-- Used by the monitor-workspaces shadow and the Lua fallbacks.
function monitor_page_workspace(name, page, tile)
  for _, monitor in ipairs(connected_monitors()) do
    if monitor.name == name then
      return base_for(monitor) + page * STRIDE + tile
    end
  end
  return page * STRIDE + tile
end

local function pin_workspaces()
  for _, monitor in ipairs(connected_monitors()) do
    local base = base_for(monitor)
    for page = 0, PAGES - 1 do
      for tile = 1, TILES do
        local rule = {
          workspace = tostring(base + page * STRIDE + tile),
          monitor = monitor.name,
        }
        if page == 0 and tile == 1 then
          rule.default = true
        end
        hl.workspace_rule(rule)
      end
    end
  end
end

local function write_map()
  ensure_state_dir()
  local lines = {
    "pages=" .. PAGES,
    "stride=" .. STRIDE,
    "fallback=30",
  }
  local rows = {}

  for identity, base in pairs(bases) do
    if identity ~= "" then
      rows[#rows + 1] = { identity = identity, base = base }
    end
  end
  table.sort(rows, function(left, right)
    if left.base ~= right.base then
      return left.base < right.base
    end
    return left.identity < right.identity
  end)
  for _, row in ipairs(rows) do
    lines[#lines + 1] = "monitor=" .. row.identity .. "|" .. row.base
  end

  local tmp = MAP_FILE .. ".tmp"
  local file = io.open(tmp, "w")
  if not file then
    return
  end
  file:write(table.concat(lines, "\n") .. "\n")
  file:close()
  os.rename(tmp, MAP_FILE)

  if hl.plugin and hl.plugin.tilesgrid and hl.plugin.tilesgrid.reloadmap then
    hl.plugin.tilesgrid.reloadmap()
  end
end

-- Load the native plugin only after Hyprland has started. It must NOT be
-- loaded while the config is being parsed: PLUGIN_INIT registers this plugin's
-- config values, which schedules a config reload, and Hyprland re-enters
-- pluginInit from the still-pending plugin-load request
-- (reload -> loadPlugin -> pluginInit -> registerPluginValue -> reload ...).
-- The compositor spins in that loop and never modesets, so the session looks
-- frozen/black. hl.plugin.load() is also config-phase only (it silently does
-- nothing from an event callback), so load via hyprctl once startup is done.
local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function load_native_plugin()
  if hl.plugin and hl.plugin.tilesgrid then
    return
  end
  local plugin_path = HOME .. "/.local/lib/hyprland/libtilesgrid.so"
  local plugin_file = io.open(plugin_path, "r")
  if not plugin_file then
    return
  end
  plugin_file:close()
  hl.exec_cmd("hyprctl plugin load " .. shell_quote(plugin_path))
end

-- hyprctl needs permission to load plugins without an interactive prompt.
hl.permission("/usr/(bin|local/bin)/hyprctl", "plugin", "allow")

-- Loudly report a failed load instead of silently leaving keys bound to a nil
-- plugin (the bindings below fall back to stock workspace switching).
local function warn_missing_plugin()
  if hl.plugin and hl.plugin.tilesgrid then
    return
  end
  hl.exec_cmd(
    "command -v omarchy-notification-send >/dev/null 2>&1 && "
      .. "omarchy-notification-send -u critical -g '󰐃' 'tilesgrid did not load' "
      .. "'Workspace keys fall back to stock. Check: hyprctl plugin list'"
  )
end

if not (hl.plugin and hl.plugin.tilesgrid) then
  hl.on("hyprland.start", function()
    load_native_plugin()
    hl.timer(warn_missing_plugin, { timeout = 3000, type = "oneshot" })
  end)
end

if hl.plugin.tilesgrid then
  hl.config({
    plugin = {
      tilesgrid = {
        layout = "grid",
        drag_button = 0x111,   -- right mouse button
        select_button = 0x110, -- left mouse button
        -- Full-resolution live previews by default. Set mode = "cached" to
        -- trade sharpness for speed on slower GPUs (tune preview:scale then).
        preview = {
          mode = "live",
        },
      },
    },
  })
end

pin_workspaces()
write_map()

hl.on("hyprland.start", function()
  pin_workspaces()
  write_map()
end)

local function repin_later()
  hl.timer(function()
    pin_workspaces()
    write_map()
  end, { timeout = 500, type = "oneshot" })
end
hl.on("monitor.added", repin_later)
hl.on("monitor.removed", repin_later)

-- Bindings -----------------------------------------------------------------

local function tilesgrid()
  return hl.plugin.tilesgrid
end

-- Keep workspace keys usable when the native plugin is not loaded (ABI
-- mismatch, hyprtasking still holding the hooks, or an install that happened
-- after this session started). Falls back to tile 1..9 of page 1 via the map.
local function fallback_tile(n, move, follow)
  if _G.monitor_page_workspace == nil then
    return
  end
  local monitor = hl.get_monitor_at_cursor()
  if monitor == nil or monitor.name == nil then
    return
  end
  local id = _G.monitor_page_workspace(monitor.name, 0, n)
  if id == nil then
    return
  end
  if move then
    hl.dispatch(hl.dsp.window.move({ workspace = tostring(id), follow = follow }))
  else
    hl.dispatch(hl.dsp.focus({ workspace = tostring(id) }))
  end
end

hl.unbind("SUPER + TAB")
o.bind("SUPER + TAB", "Workspace overview", function()
  local plugin = tilesgrid()
  if plugin then
    plugin.toggle("cursor")
  end
end)

hl.bind("escape", function()
  local plugin = tilesgrid()
  if plugin and plugin.is_active() then
    plugin.toggle("all")
  end
end, { non_consuming = true })

for n = 1, 9 do
  local key = "code:" .. tostring(n + 9)
  hl.unbind("SUPER + " .. key)
  hl.unbind("SUPER + SHIFT + " .. key)
  hl.unbind("SUPER + SHIFT + ALT + " .. key)

  o.bind("SUPER + " .. key, "Switch to tile " .. n, function()
    local plugin = tilesgrid()
    if plugin then
      plugin.focus(n)
    else
      fallback_tile(n, false, false)
    end
  end)
  o.bind("SUPER + SHIFT + " .. key, "Move window to tile " .. n .. " and follow", function()
    local plugin = tilesgrid()
    if plugin then
      plugin.move_to_tile(n, true)
    else
      fallback_tile(n, true, true)
    end
  end)
  o.bind("SUPER + SHIFT + ALT + " .. key, "Move window silently to tile " .. n, function()
    local plugin = tilesgrid()
    if plugin then
      plugin.move_to_tile(n, false)
    else
      fallback_tile(n, true, false)
    end
  end)
end

-- Remove stock bindings that can focus or create workspaces outside the fixed
-- page map, and whole-workspace monitor moves that would break it.
for _, keys in ipairs({
  "SUPER + code:19",
  "SUPER + SHIFT + code:19",
  "SUPER + SHIFT + ALT + code:19",
  "SUPER + SHIFT + TAB",
  "SUPER + CTRL + TAB",
  "SUPER + mouse_down",
  "SUPER + mouse_up",
  "SUPER + SHIFT + ALT + LEFT",
  "SUPER + SHIFT + ALT + RIGHT",
  "SUPER + SHIFT + ALT + UP",
  "SUPER + SHIFT + ALT + DOWN",
}) do
  hl.unbind(keys)
end

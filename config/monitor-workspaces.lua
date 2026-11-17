-- tilesgrid shadow of Omarchy's default.hypr.monitor-workspaces.
--
-- Omarchy's module numbers workspaces in blocks of ten and reconciles them on
-- every config reload and monitor event. tilesgrid uses fixed 3x3 pages instead,
-- so this shim keeps the `o.workspaces` API alive for Omarchy's default
-- bindings without registering any reconciler or event handlers.

local M = { slots = 9 }

local function action(fn, expression)
  o.bind_expressions[fn] = expression
  return fn
end

local function plugin()
  return hl.plugin.tilesgrid
end

local function monitor_name(monitor_name_arg)
  if monitor_name_arg ~= nil and monitor_name_arg ~= "" then
    return monitor_name_arg
  end
  local monitor = hl.get_monitor_at_cursor()
  return monitor and monitor.name or nil
end

local function fallback_workspace(slot, monitor_name_arg)
  if _G.monitor_page_workspace == nil then
    return nil
  end

  local name = monitor_name(monitor_name_arg)
  if name == nil then
    return nil
  end

  -- The plugin owns the current-page state; this fallback only exists for the
  -- first config pass before hl.plugin.tilesgrid is registered.
  return _G.monitor_page_workspace(name, 0, slot)
end

function M.focus(slot, monitor_name_arg)
  return action(function()
    local tilesgrid = plugin()
    if tilesgrid and tilesgrid.focus then
      tilesgrid.focus(slot, monitor_name_arg)
      return
    end

    local id = fallback_workspace(slot, monitor_name_arg)
    if id ~= nil then
      hl.dispatch(hl.dsp.focus({ workspace = tostring(id) }))
    end
  end, "o.workspaces.focus(" .. slot
    .. (monitor_name_arg and (", " .. string.format("%q", monitor_name_arg)) or "") .. ")")
end

function M.move_window(slot, follow)
  return action(function()
    local tilesgrid = plugin()
    if tilesgrid and tilesgrid.move_to_tile then
      tilesgrid.move_to_tile(slot, follow ~= false)
      return
    end

    local id = fallback_workspace(slot, nil)
    if id ~= nil then
      hl.dispatch(hl.dsp.window.move({ workspace = tostring(id), follow = follow ~= false }))
    end
  end, "o.workspaces.move_window(" .. slot .. (follow == false and ", false" or "") .. ")")
end

-- Whole-workspace movement would move a fixed page workspace off its monitor,
-- breaking the static map. These bindings are unbound in hypr/bindings.lua.
function M.move_to_monitor(direction)
  return action(function() end,
    "o.workspaces.move_to_monitor(" .. string.format("%q", direction) .. ")")
end

function M.move_window_to_monitor(direction)
  return action(function()
    local target = hl.get_monitor(direction)
    if target then
      hl.dispatch(hl.dsp.window.move({ monitor = target.name }))
    end
  end, "o.workspaces.move_window_to_monitor(" .. string.format("%q", direction) .. ")")
end

function M.workspace_id(monitor, slot)
  local name = type(monitor) == "table" and monitor.name or monitor
  if name == nil then
    return slot
  end
  if _G.monitor_page_workspace ~= nil then
    return _G.monitor_page_workspace(name, 0, slot)
  end
  return slot
end

function M.reconcile()
  -- Intentionally empty: tilesgrid owns reconciliation.
end

o.workspaces = M
return M

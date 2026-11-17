#include "manager.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspacePlacementController.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include "config/shared/workspace/WorkspaceRuleManager.hpp"
#include "layout/grid.hpp"
#include "map.hpp"
#include "overview.hpp"

HTManager::HTManager() {
    swipe_state = HT_SWIPE_NONE;
    swipe_amt = 0.0;
}

PHTVIEW HTManager::get_view_from_monitor(PHLMONITOR monitor) {
    if (monitor == nullptr)
        return nullptr;
    for (PHTVIEW view : views) {
        if (view == nullptr)
            continue;
        if (view->get_monitor() != monitor)
            continue;
        return view;
    }
    return nullptr;
}

PHTVIEW HTManager::get_view_from_cursor() {
    return get_view_from_monitor(
        State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run());
}

PHTVIEW HTManager::get_view_from_id(VIEWID view_id) {
    for (PHTVIEW view : views) {
        if (view == nullptr)
            continue;
        if (view->monitor_id != view_id)
            continue;
        return view;
    }
    return nullptr;
}

PHLWINDOW HTManager::get_window_from_cursor(bool return_focused) {
    const PHLMONITOR cursor_monitor =
        State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run();
    if (cursor_monitor == nullptr)
        return nullptr;

    if (return_focused)
        return cursor_monitor->m_activeWorkspace == nullptr
            ? nullptr
            : cursor_monitor->m_activeWorkspace->getLastFocusedWindow();

    const PHTVIEW cursor_view = get_view_from_monitor(cursor_monitor);
    if (cursor_view == nullptr)
        return nullptr;

    const Vector2D mouse_coords = g_pInputManager->getMouseCoordsInternal();

    if (cursor_view->active && cursor_view->layout->page_strip_at(mouse_coords).has_value())
        return nullptr;

    if (!cursor_view->active || !cursor_view->layout->should_manage_mouse()) {
        return Desktop::viewState()->hitTest().windowAt(
            mouse_coords,
            Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING
        );
    }

    const WORKSPACEID ws_id = cursor_view->layout->get_ws_id_from_global(mouse_coords);
    const PHLWORKSPACE hovered_workspace = State::workspaceState()->query().id(ws_id).run();
    if (hovered_workspace == nullptr)
        return nullptr;

    if (hovered_workspace->monitorID() != cursor_monitor->m_id)
        return nullptr;

    const Vector2D ws_coords = cursor_view->layout->global_to_local_ws_unscaled(mouse_coords, ws_id)
        + cursor_monitor->m_position;

    const PHLWORKSPACEREF o_workspace = cursor_monitor->m_activeWorkspace;
    cursor_monitor->changeWorkspace(hovered_workspace, true);

    const PHLWINDOW hovered_window = Desktop::viewState()->hitTest().windowAt(
        ws_coords,
        Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING
    );

    if (o_workspace != nullptr)
        cursor_monitor->changeWorkspace(o_workspace.lock(), true);

    return hovered_window;
}

void HTManager::show_all_views() {
    for (PHTVIEW view : views) {
        if (view == nullptr)
            continue;
        view->show();
    }
}

void HTManager::hide_all_views() {
    for (PHTVIEW view : views) {
        if (view == nullptr)
            continue;
        view->hide(false);
    }
}

void HTManager::show_cursor_view() {
    const PHTVIEW view = get_view_from_cursor();
    if (view != nullptr)
        view->show();
}

void HTManager::reset() {
    swipe_state = HT_SWIPE_NONE;
    swipe_amt = 0.0;
    jump_pressed_keys.clear();
    views.clear();
}

void HTManager::snapshot_monitor(PHLMONITOR monitor) {
    if (monitor == nullptr || g_pCompositor->m_isShuttingDown)
        return;
    if (monitor->m_name == "FALLBACK" || monitor->m_activeMonitorRule.m_disabled)
        return;

    for (const auto& existing : recovery_queue) {
        if (existing.monitor_id == monitor->m_id)
            return;
    }

    RecoverySnapshot snapshot;
    snapshot.monitor_id = monitor->m_id;
    snapshot.name = monitor->m_name;
    snapshot.description = monitor->m_description;

    if (const PHLWINDOW focused = Desktop::focusState()->window();
        focused != nullptr && focused->m_monitor.lock() == monitor)
        snapshot.focused = focused;

    std::unordered_set<const void*> seen;
    for (const auto& window : Desktop::viewState()->windows()) {
        if (window == nullptr || window->m_workspace == nullptr)
            continue;
        if (window->m_workspace->monitorID() != monitor->m_id)
            continue;
        if (window->m_workspace->m_isSpecialWorkspace)
            continue;
        if (!seen.insert(window.get()).second)
            continue;
        snapshot.windows.push_back({window, window->m_workspace->m_id});
    }

    recovery_queue.push_back(std::move(snapshot));
}

void HTManager::recover_monitor(MONITORID monitor_id) {
    if (recovering || g_pCompositor->m_isShuttingDown)
        return;

    const auto found = std::find_if(
        recovery_queue.begin(),
        recovery_queue.end(),
        [monitor_id](const RecoverySnapshot& snapshot) {
            return snapshot.monitor_id == monitor_id;
        }
    );
    if (found == recovery_queue.end())
        return;

    RecoverySnapshot snapshot = std::move(*found);
    recovery_queue.erase(found);

    struct RecoveringReset {
        bool& flag;
        ~RecoveringReset() { flag = false; }
    } reset_recovering {recovering};
    recovering = true;

    PHLMONITOR adoption = Desktop::focusState()->monitor();
    if (adoption == nullptr || adoption->m_id == monitor_id || adoption->m_name == "FALLBACK") {
        adoption = nullptr;
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (monitor != nullptr && monitor->m_id != monitor_id && monitor->m_name != "FALLBACK") {
                adoption = monitor;
                break;
            }
        }
    }

    if (adoption == nullptr)
        return;

    const PHTVIEW view = get_view_from_monitor(adoption);
    if (view == nullptr || view->layout == nullptr || view->layout->layout_name() != "grid")
        return;

    auto* grid = static_cast<HTLayoutGrid*>(view->layout.get());
    const PHLWORKSPACE active = adoption->m_activeWorkspace;
    int page = active == nullptr ? 0 : grid->page_of_workspace(active->m_id);
    if (page < 0)
        page = 0;

    std::vector<WORKSPACEID> empty_tiles;
    for (int tile = 1; tile <= TilesGrid::TILES_PER_PAGE; tile++) {
        const auto id = grid->tile_workspace_at(page, tile);
        if (!id.has_value())
            continue;

        const PHLWORKSPACE existing = State::workspaceState()->query().id(*id).run();
        if (existing != nullptr
            && (existing->monitorID() != adoption->m_id || existing->getWindowCount() > 0))
            continue;

        empty_tiles.push_back(*id);
    }

    size_t next_empty = 0;
    std::unordered_set<const void*> moved;

    for (const auto& entry : snapshot.windows) {
        if (next_empty >= empty_tiles.size())
            break;

        const PHLWINDOW window = entry.window.lock();
        if (window == nullptr || window->m_pinned || window->m_workspace == nullptr)
            continue;
        if (window->m_workspace->m_id != entry.origin)
            continue;
        if (window->m_workspace->m_isSpecialWorkspace)
            continue;

        while (next_empty < empty_tiles.size()) {
            const WORKSPACEID target_id = empty_tiles[next_empty++];
            PHLWORKSPACE target = State::workspaceState()->query().id(target_id).run();
            if (target == nullptr)
                target = State::workspaceState()->create(target_id, adoption->m_id);
            if (target == nullptr || target->monitorID() != adoption->m_id)
                continue;

            Desktop::globalWindowController()->moveWindowToWorkspace(window, target);
            moved.insert(window.get());
            break;
        }
    }

    if (const PHLWINDOW focused = snapshot.focused.lock();
        focused != nullptr && moved.contains(focused.get()) && focused->m_workspace != nullptr) {
        adoption->changeWorkspace(focused->m_workspace);
        Desktop::focusState()->fullWindowFocus(focused, Desktop::FOCUS_REASON_CLICK);
    }

    recovering = false;
    refresh_static_maps();
}

void HTManager::refresh_static_maps() {
    if (recovering || views.empty())
        return;

    const auto& map = TilesGridMap::current();

    std::vector<PHTVIEW> ordered;
    ordered.reserve(views.size());
    for (const PHTVIEW& view : views) {
        if (view != nullptr && view->layout != nullptr && view->layout->layout_name() == "grid")
            ordered.push_back(view);
    }
    std::sort(ordered.begin(), ordered.end(), [](const PHTVIEW& a, const PHTVIEW& b) {
        return a->get_monitor() != nullptr && b->get_monitor() != nullptr
            ? a->get_monitor()->m_name < b->get_monitor()->m_name
            : a->monitor_id < b->monitor_id;
    });

    std::unordered_set<int64_t> used;
    for (const PHTVIEW& view : ordered) {
        const PHLMONITOR monitor = view->get_monitor();
        if (monitor == nullptr)
            continue;

        const auto known = map.base_for(monitor->m_description, monitor->m_name);
        if (known.has_value() && !used.contains(*known)) {
            used.insert(*known);
            auto* grid = static_cast<HTLayoutGrid*>(view->layout.get());
            grid->set_monitor_base(*known);
            continue;
        }

        int64_t fallback = 0;
        while (used.contains(fallback))
            fallback += TilesGrid::MONITOR_BASE_STEP;
        used.insert(fallback);
        static_cast<HTLayoutGrid*>(view->layout.get())->set_monitor_base(fallback);
    }

    for (const PHTVIEW& view : ordered) {
        auto* grid = static_cast<HTLayoutGrid*>(view->layout.get());
        grid->refresh_recovered();
        if (!view->active)
            grid->init_position();
    }
}

void HTManager::remove_view_for_monitor_id(MONITORID mid) {
    std::erase_if(views, [mid](const PHTVIEW& v) {
        return v == nullptr || v->monitor_id == mid;
    });
}

bool HTManager::has_active_view() {
    for (const auto& view : views) {
        if (view == nullptr)
            continue;
        if (view->active)
            return true;
    }
    return false;
}

bool HTManager::cursor_view_active() {
    const PHTVIEW view = get_view_from_cursor();
    if (view == nullptr)
        return false;
    return view->active;
}

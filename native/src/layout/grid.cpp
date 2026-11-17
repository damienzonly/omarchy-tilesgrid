#include "grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include "../config.hpp"
#include "../globals.hpp"
#include "../map.hpp"
#include "../mapping.hpp"
#include "../overview.hpp"
#include "../render.hpp"
#include "../types.hpp"
#include "src/layout/target/Target.hpp"

using Hyprutils::Utils::CScopeGuard;

HTLayoutGrid::HTLayoutGrid(VIEWID new_view_id) : HTLayoutBase(new_view_id) {
    auto &anim_tree = Config::animationTree();
    Animation::mgr()->createAnimation(
        {0, 0},
        offset,
        anim_tree->getAnimationPropertyConfig("workspaces"),
        AVARDAMAGE_NONE
    );
    Animation::mgr()->createAnimation(
        1.f,
        scale,
        anim_tree->getAnimationPropertyConfig("workspaces"),
        AVARDAMAGE_NONE
    );

    init_position();
}

int HTLayoutGrid::pages() const {
    return TilesGridMap::current().pages;
}

int64_t HTLayoutGrid::stride() const {
    return TilesGridMap::current().stride;
}

int HTLayoutGrid::normal_page_count() const {
    return std::max(1, pages());
}

int HTLayoutGrid::visible_page_count() const {
    return normal_page_count()
        + TilesGrid::recovered_subpage_count(recovered_ids.size());
}

bool HTLayoutGrid::is_recovered_view() const {
    return page >= normal_page_count();
}

int HTLayoutGrid::recovered_subpage_index() const {
    return std::max(0, page - normal_page_count());
}

void HTLayoutGrid::set_monitor_base(int64_t base) {
    if (map_base == base)
        return;
    map_base = base;
    invalidate_cached_tiles();
}

std::optional<WORKSPACEID> HTLayoutGrid::tile_workspace_at(int viewed_page, int tile) const {
    if (tile < 1 || tile > TilesGrid::TILES_PER_PAGE)
        return std::nullopt;
    if (viewed_page < 0)
        return std::nullopt;

    if (viewed_page < normal_page_count())
        return TilesGrid::tile_workspace(map_base, viewed_page, tile, stride());

    const int sub = viewed_page - normal_page_count();
    const size_t index = static_cast<size_t>(sub) * TilesGrid::TILES_PER_PAGE
        + static_cast<size_t>(tile - 1);
    if (index >= recovered_ids.size())
        return std::nullopt;
    return recovered_ids[index];
}

std::optional<WORKSPACEID> HTLayoutGrid::viewed_tile_workspace(int tile) const {
    return tile_workspace_at(page, tile);
}

std::optional<TilesGrid::PageTile> HTLayoutGrid::page_tile_of(WORKSPACEID id) const {
    return TilesGrid::page_tile_of_workspace(id, map_base, normal_page_count(), stride());
}

int HTLayoutGrid::page_of_workspace(WORKSPACEID id) const {
    if (const auto mapped = page_tile_of(id); mapped.has_value())
        return mapped->page;

    for (size_t i = 0; i < recovered_ids.size(); i++) {
        if (recovered_ids[i] != id)
            continue;
        return normal_page_count()
            + static_cast<int>(i / TilesGrid::TILES_PER_PAGE);
    }

    return -1;
}

void HTLayoutGrid::refresh_recovered() {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    std::vector<WORKSPACEID> next;
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w == nullptr || w->m_id <= 0 || w->m_isSpecialWorkspace)
            continue;
        if (w->monitorID() != view_id)
            continue;
        if (TilesGrid::in_map(w->m_id, map_base, normal_page_count(), stride()))
            continue;
        if (w->getWindowCount() <= 0)
            continue;
        next.push_back(w->m_id);
    }

    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());

    if (next == recovered_ids)
        return;

    recovered_ids = std::move(next);
    invalidate_cached_tiles();

    if (is_recovered_view() && page >= visible_page_count()) {
        page = std::max(0, visible_page_count() - 1);
        if (visible_page_count() == normal_page_count())
            page = 0;
    }

    g_pHyprRenderer->damageMonitor(monitor);
    monitor->scheduleFrame();
}

void HTLayoutGrid::set_viewed_page(int viewed_page) {
    const int total = visible_page_count();
    if (total <= 0) {
        page = 0;
        return;
    }

    page = std::clamp(viewed_page, 0, total - 1);
    invalidate_cached_tiles();

    if (const PHLMONITOR monitor = get_monitor()) {
        build_overview_layout(HT_VIEW_ANIMATING);
        g_pHyprRenderer->damageMonitor(monitor);
        monitor->scheduleFrame();
    }
}

void HTLayoutGrid::change_page(int delta) {
    const int total = visible_page_count();
    if (total <= 1 || delta == 0)
        return;

    int next = (page + delta) % total;
    if (next < 0)
        next += total;
    set_viewed_page(next);
}

int HTLayoutGrid::page_strip_height() {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return 0;

    const int configured = static_cast<int>(HTConfig::value<Config::INTEGER>("page_strip:height"));
    if (configured <= 0)
        return 0;
    return std::max(1, static_cast<int>(std::round(configured * monitor->m_scale)));
}

CBox HTLayoutGrid::page_strip_box() {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return {};

    const int h = page_strip_height();
    if (h <= 0)
        return {};
    return CBox {
        Vector2D(0.0, monitor->m_transformedSize.y - h),
        Vector2D(monitor->m_transformedSize.x, static_cast<double>(h))
    };
}

std::string HTLayoutGrid::page_label(int viewed_page) const {
    if (viewed_page < normal_page_count())
        return std::to_string(viewed_page + 1);
    return "R" + std::to_string(viewed_page - normal_page_count() + 1);
}

std::vector<HTLayoutGrid::PageTab> HTLayoutGrid::page_tabs() {
    std::vector<PageTab> tabs;
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return tabs;

    const int h = page_strip_height();
    const int total = visible_page_count();
    if (h <= 0 || total <= 0)
        return tabs;

    const float gap = std::max(4.f, h * 0.18f);
    const float tab_h = std::max(1.f, h - gap * 2.f);
    const float tab_w = std::max(tab_h, 28.f * static_cast<float>(monitor->m_scale));
    const float total_w = total * tab_w + (total - 1) * gap;
    const float start_x = (monitor->m_transformedSize.x - total_w) / 2.f;
    const float y = monitor->m_transformedSize.y - h + gap;

    tabs.reserve(total);
    for (int i = 0; i < total; i++) {
        tabs.push_back(PageTab {
            i,
            CBox {
                Vector2D(start_x + i * (tab_w + gap), y),
                Vector2D(tab_w, tab_h)
            }
        });
    }
    return tabs;
}

std::optional<int> HTLayoutGrid::tile_index_at(Vector2D pos) {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr || !monitor->logicalBox().containsPoint(pos))
        return std::nullopt;

    const Vector2D local = (pos - monitor->m_position) * monitor->m_scale;
    for (size_t i = 0; i < tile_boxes.size(); i++) {
        if (tile_boxes[i].containsPoint(local))
            return static_cast<int>(i);
    }
    return std::nullopt;
}

std::optional<int> HTLayoutGrid::page_strip_at(Vector2D pos) {
    const PHTVIEW view = ht_manager == nullptr ? nullptr : ht_manager->get_view_from_id(view_id);
    if (view == nullptr || !view->active || view->closing)
        return std::nullopt;

    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr || !monitor->logicalBox().containsPoint(pos))
        return std::nullopt;

    const Vector2D local = (pos - monitor->m_position) * monitor->m_scale;
    for (const auto& tab : page_tabs()) {
        if (tab.box.containsPoint(local))
            return tab.page;
    }
    return std::nullopt;
}

void HTLayoutGrid::render_page_strip() {
    const PHTVIEW view = ht_manager == nullptr ? nullptr : ht_manager->get_view_from_id(view_id);
    if (view == nullptr || !view->active || view->closing)
        return;

    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    const CBox strip = page_strip_box();
    if (strip.empty())
        return;

    CRectPassElement::SRectData strip_bg;
    strip_bg.box = strip;
    strip_bg.color = CHyprColor {HTConfig::value<Config::INTEGER>("page_strip:bg_color")};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(strip_bg));

    const auto active_value = HTConfig::value<Config::INTEGER>("page_strip:active_color");
    const auto inactive_value = HTConfig::value<Config::INTEGER>("page_strip:inactive_color");
    const auto label_value = HTConfig::value<Config::INTEGER>("page_strip:label_color");
    const CHyprColor active_color {active_value};
    const CHyprColor inactive_color {inactive_value};
    const CHyprColor label_color {label_value};

    const int font_size = std::max(
        1,
        static_cast<int>(HTConfig::value<Config::INTEGER>("page_strip:label_size") * monitor->m_scale)
    );

    static std::unordered_map<std::string, SP<Render::ITexture>> texture_cache;

    int hovered_page = -1;
    if (g_layoutManager->dragController()->target() != nullptr) {
        if (const auto hit = page_strip_at(g_pInputManager->getMouseCoordsInternal()); hit.has_value())
            hovered_page = *hit;
    }

    for (const auto& tab : page_tabs()) {
        const bool active = tab.page == page;
        const bool hovered = tab.page == hovered_page;
        CRectPassElement::SRectData data;
        data.box = tab.box;
        data.color = (active || hovered) ? active_color : inactive_color;
        data.round = std::round(std::min(tab.box.w, tab.box.h) * 0.25);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));

        const std::string label = page_label(tab.page);
        const std::string key = label + ":" + std::to_string(font_size) + ":"
            + std::to_string(static_cast<int64_t>(label_value));
        auto& texture = texture_cache[key];
        if (texture == nullptr) {
            texture = g_pHyprRenderer->renderText(
                label,
                label_color,
                font_size,
                false,
                "",
                0,
                700
            );
        }
        if (texture == nullptr)
            continue;

        CTexPassElement::SRenderData text;
        text.tex = texture;
        text.box = CBox {
            tab.box.pos() + (tab.box.size() - texture->m_size) / 2.f,
            texture->m_size
        };
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(text)));
    }
}

bool HTLayoutGrid::on_mouse_axis(double delta) {
    const PHTVIEW view = ht_manager == nullptr ? nullptr : ht_manager->get_view_from_id(view_id);
    if (view == nullptr || !view->active || view->closing)
        return false;

    if (g_layoutManager->dragController()->target() != nullptr)
        return false;

    const Vector2D mouse = g_pInputManager->getMouseCoordsInternal();
    if (!page_strip_at(mouse).has_value())
        return false;

    change_page(delta > 0 ? 1 : -1);
    return true;
}

CBox HTLayoutGrid::closed_box_for_page_tile(int viewed_page, int tile) const {
    if (viewed_page < 0 || tile < 1 || tile > TilesGrid::TILES_PER_PAGE)
        return {};
    return const_cast<HTLayoutGrid*>(this)->calculate_ws_box(
        TilesGrid::tile_x(tile),
        TilesGrid::tile_y(tile),
        HT_VIEW_CLOSED
    );
}

void HTLayoutGrid::reanchor_to(WORKSPACEID id) {
    build_overview_layout(HT_VIEW_CLOSED);

    if (const auto box = find_ws_box(id); box.has_value() && !box->empty()) {
        offset->setValueAndWarp(-box->pos());
        scale->setValueAndWarp(1.f);
        return;
    }

    const int target_page = page_of_workspace(id);
    if (target_page >= 0 && target_page != page) {
        page = target_page;
        build_overview_layout(HT_VIEW_CLOSED);
        if (const auto box = find_ws_box(id); box.has_value() && !box->empty()) {
            offset->setValueAndWarp(-box->pos());
            scale->setValueAndWarp(1.f);
            return;
        }
    }

    offset->setValueAndWarp(Vector2D {0, 0});
    scale->setValueAndWarp(1.f);
}

std::string HTLayoutGrid::layout_name() {
    return "grid";
}

WORKSPACEID HTLayoutGrid::get_ws_id_in_direction(int x, int y, std::string& direction) {
    const int LOOP = HTConfig::value<Config::INTEGER>("grid:loop");
    constexpr int ROWS = TilesGrid::ROWS;
    constexpr int COLS = TilesGrid::COLS;

    if (direction == "up") {
        y--;
    } else if (direction == "down") {
        y++;
    } else if (direction == "right") {
        x++;
    } else if (direction == "left") {
        x--;
    } else {
        return WORKSPACE_INVALID;
    }

    if (LOOP) {
        x = (x + COLS) % COLS;
        y = (y + ROWS) % ROWS;
    }
    return get_ws_id_from_xy(x, y);
}

void HTLayoutGrid::on_move_swipe(Vector2D delta) {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    const float MOVE_DISTANCE = HTConfig::value<Config::FLOAT>("gestures:move_distance");
    constexpr int ROWS = TilesGrid::ROWS;
    constexpr int COLS = TilesGrid::COLS;
    const CBox min_ws = calculate_ws_box(0, 0, HT_VIEW_CLOSED);
    const CBox max_ws = calculate_ws_box(COLS - 1, ROWS - 1, HT_VIEW_CLOSED);

    Vector2D new_offset = offset->value() + delta / MOVE_DISTANCE * max_ws.w;
    new_offset = new_offset.clamp(Vector2D {-max_ws.x, -max_ws.y}, Vector2D {-min_ws.x, -min_ws.y});

    offset->resetAllCallbacks();
    offset->setValueAndWarp(new_offset);
}

WORKSPACEID HTLayoutGrid::on_move_swipe_end() {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return WORKSPACE_INVALID;

    build_overview_layout(HT_VIEW_CLOSED);
    WORKSPACEID closest = WORKSPACE_INVALID;
    double closest_dist = 1e9;
    for (const auto& [ws_id, box] : overview_layout) {
        const float dist_sq = offset->value().distanceSq(Vector2D {-box.box.x, -box.box.y});
        if (dist_sq < closest_dist) {
            closest_dist = dist_sq;
            closest = ws_id;
        }
    }
    return closest;
}

void HTLayoutGrid::close_open_lerp(float perc) {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    double open_scale =
        calculate_ws_box(0, 0, HT_VIEW_OPENED).w / monitor->m_transformedSize.x; // 1 / ROWS
    Vector2D open_pos = {0, 0};

    const PHLWORKSPACE active = monitor->m_activeWorkspace;
    if (active == nullptr)
        return;

    const int active_page = page_of_workspace(active->m_id);
    if (active_page >= 0)
        page = active_page;

    build_overview_layout(HT_VIEW_CLOSED);
    double close_scale = 1.;
    Vector2D close_pos = {0, 0};
    if (const auto box = find_ws_box(active->m_id); box.has_value() && !box->empty())
        close_pos = -box->pos();

    double new_scale = std::lerp(close_scale, open_scale, perc);
    Vector2D new_pos = Vector2D {
        std::lerp(close_pos.x, open_pos.x, perc),
        std::lerp(close_pos.y, open_pos.y, perc)
    };

    scale->resetAllCallbacks();
    offset->resetAllCallbacks();
    scale->setValueAndWarp(new_scale);
    offset->setValueAndWarp(new_pos);
}

void HTLayoutGrid::on_show(CallbackFun on_complete) {
    CScopeGuard x([this, &on_complete] {
        if (on_complete != nullptr)
            offset->setCallbackOnEnd(on_complete);
    });

    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    // Snapshots from a previous open are stale.
    invalidate_cached_tiles();

    *scale = calculate_ws_box(0, 0, HT_VIEW_OPENED).w / monitor->m_transformedSize.x; // 1 / ROWS
    // Offset for the whole grid of workspaces
    *offset = {0, 0};
}

void HTLayoutGrid::on_hide(CallbackFun on_complete) {
    CScopeGuard x([this, &on_complete] {
        if (on_complete != nullptr)
            offset->setCallbackOnEnd(on_complete);
    });

    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    const PHLWORKSPACE active = monitor->m_activeWorkspace;
    if (active == nullptr)
        return;

    const int active_page = page_of_workspace(active->m_id);
    if (active_page >= 0)
        page = active_page;

    build_overview_layout(HT_VIEW_CLOSED);
    *scale = 1.;
    if (const auto box = find_ws_box(active->m_id); box.has_value() && !box->empty())
        *offset = -box->pos();
    else
        *offset = {0, 0};
}

void HTLayoutGrid::on_move(WORKSPACEID old_id, WORKSPACEID new_id, CallbackFun on_complete) {
    CScopeGuard x([this, &on_complete] {
        if (on_complete != nullptr)
            offset->setCallbackOnEnd(on_complete);
    });

    const PHTVIEW par_view = ht_manager->get_view_from_id(view_id);
    if (par_view == nullptr || par_view->active)
        return;

    const int target_page = page_of_workspace(new_id);
    if (target_page >= 0)
        page = target_page;

    // prevent the thing from animating
    if (const auto old_ws = State::workspaceState()->query().id(old_id).run(); old_ws != nullptr)
        old_ws->m_renderOffset->warp();
    if (const auto new_ws = State::workspaceState()->query().id(new_id).run(); new_ws != nullptr)
        new_ws->m_renderOffset->warp();

    build_overview_layout(HT_VIEW_CLOSED);
    *scale = 1.;
    if (const auto box = find_ws_box(new_id); box.has_value() && !box->empty())
        *offset = -box->pos();
    else
        *offset = {0, 0};
}

bool HTLayoutGrid::should_render_window(PHLWINDOW window) {
    if (window == nullptr)
        return false;

    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return false;

    const PHLWORKSPACE workspace = window->m_workspace;
    if (workspace == nullptr || workspace->m_monitor.lock() != monitor)
        return false;

    // Only windows on the currently laid-out page are part of the overview.
    if (!overview_layout.contains(workspace->m_id))
        return false;

    const SP<Layout::ITarget> target = g_layoutManager->dragController()->target();
    if (target != nullptr && window == target->window())
        return false;

    const CBox window_box = get_global_window_box(window, workspace->m_id);
    if (window_box.empty() || window_box.intersection(monitor->logicalBox()).empty())
        return false;

    return HTLayoutBase::should_render_window(window);
}

float HTLayoutGrid::drag_window_scale() {
    return scale->value();
}

void HTLayoutGrid::init_position() {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    const PHLWORKSPACE active = monitor->m_activeWorkspace;
    if (active == nullptr)
        return;

    const int active_page = page_of_workspace(active->m_id);
    page = active_page >= 0 ? active_page : 0;
    reanchor_to(active->m_id);
}

CBox HTLayoutGrid::calculate_ws_box(int x, int y, HTViewStage stage) {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return {};

    // Monitor may not have its final size yet during connect/reconnect
    if (monitor->m_transformedSize.x < 1 || monitor->m_transformedSize.y < 1)
        return {};

    constexpr int ROWS = TilesGrid::ROWS;
    constexpr int COLS = TilesGrid::COLS;
    const int GAPS_USE_ASPECT_RATIO = HTConfig::value<Config::INTEGER>("grid:gaps_use_aspect_ratio");
    const float GAP_SIZE = HTConfig::value<Config::FLOAT>("gap_size") * monitor->m_scale;
    const Vector2D gaps = {
        GAP_SIZE,
        GAPS_USE_ASPECT_RATIO
            ? GAP_SIZE * monitor->m_transformedSize.y / monitor->m_transformedSize.x
            : GAP_SIZE
    };

    if (GAP_SIZE > std::min(monitor->m_transformedSize.x, monitor->m_transformedSize.y)
        || GAP_SIZE < 0)
        return {};

    const double strip_h = page_strip_height();
    const double available_h = monitor->m_transformedSize.y - strip_h;
    if (available_h <= 0 || available_h < gaps.y * (ROWS + 1))
        return {};

    double render_x = (monitor->m_transformedSize.x - gaps.x * (COLS + 1)) / COLS;
    double render_y = (available_h - gaps.y * (ROWS + 1)) / ROWS;
    const double mon_aspect = monitor->m_transformedSize.x / monitor->m_transformedSize.y;
    Vector2D start_offset {};

    // make correct aspect ratio
    if (render_y * mon_aspect > render_x) {
        start_offset.y = (render_y - render_x / mon_aspect) * ROWS / 2.f;
        render_y = render_x / mon_aspect;
    } else if (render_x / mon_aspect > render_y) {
        start_offset.x = (render_x - render_y * mon_aspect) * COLS / 2.f;
        render_x = render_y * mon_aspect;
    }

    float use_scale = scale->value();
    Vector2D use_offset = offset->value();
    if (stage == HT_VIEW_CLOSED) {
        use_scale = 1;
        use_offset = Vector2D {0, 0};
    } else if (stage == HT_VIEW_OPENED) {
        use_scale = render_x / monitor->m_transformedSize.x;
        use_offset = Vector2D {0, 0};
    }

    const Vector2D ws_sz = monitor->m_transformedSize * use_scale;
    return CBox {Vector2D {x, y} * (ws_sz + gaps) + gaps + use_offset + start_offset, ws_sz};
};

void HTLayoutGrid::build_overview_layout(HTViewStage stage) {
    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr)
        return;

    const PHLMONITOR last_monitor = Desktop::focusState()->monitor();
    Desktop::focusState()->rawMonitorFocus(monitor);

    overview_layout.clear();
    tile_ids.fill(std::nullopt);
    tile_boxes.fill(CBox {});

    for (int y = 0; y < TilesGrid::ROWS; y++) {
        for (int x = 0; x < TilesGrid::COLS; x++) {
            const int index = y * TilesGrid::COLS + x;
            const int tile = index + 1;

            CBox ws_box = calculate_ws_box(x, y, stage);
            ws_box.round();
            tile_boxes[index] = ws_box;

            std::optional<WORKSPACEID> ws_id;
            if (page < normal_page_count()) {
                ws_id = TilesGrid::tile_workspace(map_base, page, tile, stride());
            } else {
                const int sub = page - normal_page_count();
                const size_t recovered_index = static_cast<size_t>(sub) * TilesGrid::TILES_PER_PAGE
                    + static_cast<size_t>(tile - 1);
                if (recovered_index < recovered_ids.size())
                    ws_id = recovered_ids[recovered_index];
            }

            if (!ws_id.has_value())
                continue;

            tile_ids[index] = *ws_id;
            overview_layout[*ws_id] = HTWorkspace {x, y, ws_box};
        }
    }

    if (last_monitor != nullptr)
        Desktop::focusState()->rawMonitorFocus(last_monitor);
}

bool HTLayoutGrid::previews_cached() const {
    return HTConfig::value<Config::STRING>("preview:mode") != "live";
}

void HTLayoutGrid::invalidate_cached_tiles() {
    for (auto& tile : cached_tiles)
        tile.dirty = true;
    cache_warm = false;
}

// Render one workspace into an offscreen tile-sized framebuffer. This runs from
// render.pre, outside the monitor's main render pass, so it can bind a
// temporary framebuffer. The workspace is made visible/rendered without
// changing the monitor's active workspace.
bool HTLayoutGrid::render_tile_cache(
    int index,
    WORKSPACEID slot_id,
    PHLMONITOR monitor,
    PHLWORKSPACE workspace,
    const CBox& box
) {
    if (monitor == nullptr || box.empty() || index < 0
        || index >= static_cast<int>(cached_tiles.size()))
        return false;

    auto& tile = cached_tiles[index];
    const float preview_scale = std::clamp(
        static_cast<float>(HTConfig::value<Config::FLOAT>("preview:scale")),
        0.25f,
        1.f
    );
    const Vector2D size = (box.size() * monitor->m_scale * preview_scale).round();
    if (size.x < 1 || size.y < 1)
        return false;

    if (tile.fb == nullptr)
        tile.fb = g_pHyprRenderer->createFB("tilesgrid-tile");
    if (tile.fb == nullptr)
        return false;

    if (!tile.fb->isAllocated() || tile.fb->m_size != size) {
        tile.fb->release();
        if (!tile.fb->alloc(
                static_cast<int>(size.x),
                static_cast<int>(size.y),
                DRM_FORMAT_ABGR8888
            ))
            return false;
        tile.fb->setImageDescription(monitor->workBufferImageDescription());
    }

    CRegion damage {0, 0, static_cast<int>(size.x), static_cast<int>(size.y)};
    if (!g_pHyprRenderer->beginFullFakeRender(monitor, damage, tile.fb))
        return false;

    const bool saved_snapshot = g_pHyprRenderer->m_bRenderingSnapshot;
    const bool saved_blur_dirty = monitor->m_blurFBDirty;
    const bool saved_blur_should = monitor->m_blurFBShouldRender;
    g_pHyprRenderer->m_bRenderingSnapshot = true;
    monitor->m_blurFBDirty = false;
    monitor->m_blurFBShouldRender = false;

    g_pHyprRenderer->draw(CClearPassElement::SClearData {CHyprColor {0, 0, 0, 1}});
    g_pHyprRenderer->startRenderPass();

    // Hyprexpo-style preview state: render the workspace at alpha 1 with no
    // slide offset, then restore exactly what the desktop was using.
    const PHLWORKSPACE saved_active = monitor->m_activeWorkspace;
    const bool saved_active_visible =
        saved_active != nullptr && saved_active->m_visible;
    const bool saved_visible = workspace != nullptr && workspace->m_visible;
    const bool saved_force = workspace != nullptr && workspace->m_forceRendering;
    const float saved_alpha_value = workspace != nullptr ? workspace->m_alpha->value() : 0.f;
    const float saved_alpha_goal = workspace != nullptr ? workspace->m_alpha->goal() : 0.f;
    const Vector2D saved_offset_value =
        workspace != nullptr ? workspace->m_renderOffset->value() : Vector2D {};
    const Vector2D saved_offset_goal =
        workspace != nullptr ? workspace->m_renderOffset->goal() : Vector2D {};

    // Hyprland's shouldRenderWindow() treats a visible workspace's windows as
    // renderable regardless of which workspace renderWorkspace() was asked
    // for. Hide the real active workspace while a different tile is rendered,
    // or its windows would leak into every tile.
    if (saved_active != nullptr && saved_active != workspace)
        saved_active->m_visible = false;
    monitor->m_activeWorkspace = workspace;

    if (workspace != nullptr) {
        workspace->m_visible = true;
        workspace->m_forceRendering = true;
        workspace->m_alpha->setValueAndWarp(1.f);
        *workspace->m_alpha = 1.f;
        workspace->m_renderOffset->setValueAndWarp(Vector2D {});
        *workspace->m_renderOffset = Vector2D {};
    }

    CBox render_box = {Vector2D {}, box.size()};
    if (monitor->m_transform % 2 == 1)
        std::swap(render_box.w, render_box.h);

    g_cache_render = true;
    ((render_workspace_t)(render_workspace_hook->m_original))(
        g_pHyprRenderer.get(), monitor, workspace, Time::steadyNow(), render_box
    );
    g_cache_render = false;

    if (workspace != nullptr) {
        workspace->m_visible = saved_visible;
        workspace->m_forceRendering = saved_force;
        workspace->m_alpha->setValueAndWarp(saved_alpha_value);
        *workspace->m_alpha = saved_alpha_goal;
        workspace->m_renderOffset->setValueAndWarp(saved_offset_value);
        *workspace->m_renderOffset = saved_offset_goal;
    }

    if (saved_active != nullptr && saved_active != workspace)
        saved_active->m_visible = saved_active_visible;
    monitor->m_activeWorkspace = saved_active;

    g_pHyprRenderer->endRender();
    g_pHyprRenderer->m_bRenderingSnapshot = saved_snapshot;
    monitor->m_blurFBDirty = saved_blur_dirty;
    monitor->m_blurFBShouldRender = saved_blur_should;

    tile.id = slot_id;
    tile.size = size;
    tile.windows = workspace != nullptr ? workspace->getWindowCount() : 0;
    tile.dirty = false;
    return true;
}

void HTLayoutGrid::render_cached_previews() {
    for (int i = 0; i < static_cast<int>(cached_tiles.size()); i++) {
        auto& tile = cached_tiles[i];
        if (tile.fb == nullptr || !tile_ids[i].has_value())
            continue;
        if (tile.id != *tile_ids[i] || tile_boxes[i].empty())
            continue;

        const auto texture = tile.fb->getTexture();
        if (texture == nullptr)
            continue;

        CTexPassElement::SRenderData data;
        data.tex = texture;
        data.box = tile_boxes[i];
        data.a = 1.f;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(data)));
    }
}

void HTLayoutGrid::refresh_cached_previews() {
    if (!previews_cached())
        return;

    const PHTVIEW view = ht_manager == nullptr ? nullptr : ht_manager->get_view_from_id(view_id);
    if (view == nullptr || !view->active || view->closing || view->navigating)
        return;

    const PHLMONITOR monitor = get_monitor();
    if (monitor == nullptr || monitor->m_transform % 2 == 1)
        return;

    build_overview_layout(HT_VIEW_ANIMATING);

    WORKSPACEID drag_workspace = WORKSPACE_INVALID;
    if (const auto target = g_layoutManager->dragController()->target(); target != nullptr) {
        if (const auto dragged = target->window(); dragged != nullptr && dragged->m_workspace)
            drag_workspace = dragged->m_workspace->m_id;
    }

    const float preview_scale = std::clamp(
        static_cast<float>(HTConfig::value<Config::FLOAT>("preview:scale")),
        0.25f,
        1.f
    );

    std::vector<int> required;
    std::vector<int> windowed;
    bool ids_changed = false;

    for (int i = 0; i < static_cast<int>(cached_tiles.size()); i++) {
        auto& tile = cached_tiles[i];
        const CBox& box = tile_boxes[i];
        if (box.empty() || !tile_ids[i].has_value()) {
            tile.dirty = true;
            continue;
        }

        const WORKSPACEID id = *tile_ids[i];
        PHLWORKSPACE workspace = State::workspaceState()->query().id(id).run();
        // A fixed workspace that moved to another monitor is not this tile's
        // content; render the empty background for it, as the live path does.
        if (workspace != nullptr && workspace->monitorID() != view_id)
            workspace = nullptr;
        const int windows = workspace != nullptr ? workspace->getWindowCount() : 0;
        const Vector2D size =
            (box.size() * monitor->m_scale * preview_scale).round();
        const bool size_changed =
            tile.fb == nullptr || !tile.fb->isAllocated() || tile.size != size;

        if (tile.dirty || tile.id != id || tile.windows != windows || size_changed
            || (drag_workspace != WORKSPACE_INVALID && id == drag_workspace)) {
            if (tile.id != id || size_changed)
                ids_changed = true;
            required.push_back(i);
        } else if (windows > 0) {
            windowed.push_back(i);
        }
    }

    const auto render_index = [&](int index) {
        const WORKSPACEID id = *tile_ids[index];
        PHLWORKSPACE workspace = State::workspaceState()->query().id(id).run();
        if (workspace != nullptr && workspace->monitorID() != view_id)
            workspace = nullptr;
        return render_tile_cache(index, id, monitor, workspace, tile_boxes[index]);
    };

    // First frame after opening or changing pages: warm all tiles at once so
    // the overview never shows blank or stale tiles.
    if (!cache_warm) {
        for (int i = 0; i < static_cast<int>(cached_tiles.size()); i++) {
            if (tile_boxes[i].empty() || !tile_ids[i].has_value())
                continue;
            render_index(i);
        }
        cache_warm = true;
        next_refresh = 0;
        return;
    }

    if (ids_changed) {
        for (const int index : required)
            render_index(index);
        return;
    }

    const int configured = static_cast<int>(
        HTConfig::value<Config::INTEGER>("preview:refresh_tiles")
    );
    int budget = std::clamp(configured, 0, TilesGrid::TILES_PER_PAGE);
    if (budget <= 0) {
        for (const int index : required)
            render_index(index);
        return;
    }

    std::array<bool, TilesGrid::TILES_PER_PAGE> rendered {};
    for (const int index : required) {
        if (budget <= 0)
            break;
        if (render_index(index))
            rendered[index] = true;
        budget--;
    }

    // Keep the active tile the most current, but never starve the rotating
    // previews when the budget is a single tile.
    const bool allow_active =
        budget >= 2 || windowed.empty() || refresh_tick % 2 == 0;
    if (allow_active && budget > 0 && monitor->m_activeWorkspace != nullptr) {
        for (int i = 0; i < static_cast<int>(cached_tiles.size()) && budget > 0; i++) {
            if (rendered[i] || !tile_ids[i].has_value() || cached_tiles[i].windows <= 0
                || *tile_ids[i] != monitor->m_activeWorkspace->m_id)
                continue;
            if (render_index(i))
                rendered[i] = true;
            budget--;
            break;
        }
    }

    if (budget > 0 && !windowed.empty()) {
        const size_t start = next_refresh % windowed.size();
        for (size_t n = 0; n < windowed.size() && budget > 0; n++) {
            const int index = windowed[(start + n) % windowed.size()];
            if (rendered[index])
                continue;
            if (render_index(index))
                rendered[index] = true;
            budget--;
        }
        next_refresh = (next_refresh + 1) % windowed.size();
    }

    refresh_tick++;
}

// Render each visible workspace directly into its grid tile via a scaled
// renderWorkspace (renderModif). The renderTexture hook keeps the per-surface
// scissor in sync with that renderModif, so window contents aren't culled near
// tile edges.
void HTLayoutGrid::render() {
    HTLayoutBase::render();
    CScopeGuard x([this] { post_render(); });

    const PHTVIEW par_view = ht_manager->get_view_from_id(view_id);
    if (par_view == nullptr)
        return;
    const PHLMONITOR monitor = par_view->get_monitor();
    if (monitor == nullptr)
        return;

    static auto PACTIVECOL = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");
    static auto PINACTIVECOL = CConfigValue<Config::IComplexConfigValue>("general:col.inactive_border");

    auto* const ACTIVECOL = (Config::CGradientValueData*)(PACTIVECOL.ptr());
    auto* const INACTIVECOL = (Config::CGradientValueData*)(PINACTIVECOL.ptr());

    const float BORDERSIZE = HTConfig::value<Config::FLOAT>("border_size");
    const auto time = Time::steadyNow();

    CBox monitor_box = {{0, 0}, monitor->m_transformedSize};

    CRectPassElement::SRectData bg;
    bg.color = CHyprColor {HTConfig::value<Config::INTEGER>("bg_color")}.stripA();
    bg.box = monitor_box;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));

    build_overview_layout(HT_VIEW_ANIMATING);

    // In cached mode the overview is a composite of tile-sized textures, so
    // only a couple of workspaces are rendered to offscreen buffers per frame
    // (see refresh_cached_previews). Fall back to direct rendering while the
    // open/close animation is running.
    // Rotated outputs keep the live path: offscreen tiles would need the
    // monitor transform baked into each texture.
    const bool use_cache = previews_cached() && monitor->m_transform % 2 == 0
        && par_view->active && !par_view->closing && !par_view->navigating;

    // Hyprland only fully renders the active workspace, so render_workspace_at_box
    // swaps each tile's workspace in as it renders; capture the real active one to
    // restore at the end and to pick out the active-border color.
    const PHLWORKSPACE start_workspace = monitor->m_activeWorkspace;
    if (start_workspace == nullptr)
        return;
    if (!use_cache) {
        Animation::Workspace::startAnimation(
            start_workspace, Animation::Workspace::ANIMATION_TYPE_OUT, false, true
        );
        start_workspace->m_visible = false;
    }

    CBox global_mon_box = {monitor->m_position, monitor->m_transformedSize};
    const auto tile_visible = [&](const CBox& box) {
        if (box.width < 0.01 || box.height < 0.01)
            return false;
        CBox global_box = {box.pos() + monitor->m_position, box.size()};
        return !global_box.expand(BORDERSIZE).intersection(global_mon_box).empty();
    };

    const auto draw_borders = [&] {
        for (size_t i = 0; i < tile_boxes.size(); i++) {
            const CBox& box = tile_boxes[i];
            if (!tile_visible(box))
                continue;
            const bool active =
                tile_ids[i].has_value() && *tile_ids[i] == start_workspace->m_id;
            CBorderPassElement::SBorderData bdata;
            bdata.box = box;
            bdata.grad1 = active ? *ACTIVECOL : *INACTIVECOL;
            bdata.borderSize = BORDERSIZE;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(bdata));
        }
    };

    if (use_cache) {
        render_cached_previews();
        // Borders on top, since the cached textures are clipped to their tiles.
        draw_borders();
    } else {
        // Borders first, so window contents render on top of them. Workspace contents
        // now render unclipped (a window moving to a new workspace on release can
        // extend past its tile), and should not be covered by the tile borders.
        draw_borders();

        // workspace may be nullptr for empty/never-visited slots: render_workspace_at_box
        // still draws their background + wallpaper layers so the tile isn't blank. Render
        // the active workspace last so its windows (e.g. one just dropped) stay on top of
        // the neighbouring tiles.
        for (const auto& [ws_id, ws_layout] : overview_layout) {
            if (!tile_visible(ws_layout.box) || ws_id == start_workspace->m_id)
                continue;
            PHLWORKSPACE workspace = State::workspaceState()->query().id(ws_id).run();
            if (workspace != nullptr && workspace->monitorID() != view_id)
                continue;
            render_workspace_at_box(monitor, workspace, time, ws_layout.box);
        }
        if (const auto it = overview_layout.find(start_workspace->m_id);
            it != overview_layout.end() && tile_visible(it->second.box))
            render_workspace_at_box(monitor, start_workspace, time, it->second.box);

        monitor->m_activeWorkspace = start_workspace;
        Animation::Workspace::startAnimation(
            start_workspace, Animation::Workspace::ANIMATION_TYPE_IN, false, true
        );
        start_workspace->m_visible = true;
    }

    g_pHyprRenderer->damageMonitor(monitor);

    // Dragged window rendered on top, following the cursor.
    const PHTVIEW cursor_view = ht_manager->get_view_from_cursor();
    if (cursor_view == nullptr)
        return;
    const SP<Layout::ITarget> target = g_layoutManager->dragController()->target();
    if (target == nullptr)
        return;
    const PHLWINDOW dragged_window = target->window();
    if (dragged_window == nullptr)
        return;
    const Vector2D mouse_coords = g_pInputManager->getMouseCoordsInternal();
    const CBox window_box = dragged_window->getWindowMainSurfaceBox()
                                .translate(-mouse_coords)
                                .scale(cursor_view->layout->drag_window_scale())
                                .translate(mouse_coords);
    if (!window_box.intersection(monitor->logicalBox()).empty())
        render_window_at_box(dragged_window, monitor, time, window_box);
}

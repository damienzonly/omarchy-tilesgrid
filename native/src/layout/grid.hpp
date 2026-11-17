#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/render/Framebuffer.hpp>

#include "../mapping.hpp"
#include "../types.hpp"
#include "layout_base.hpp"

class HTLayoutGrid: public HTLayoutBase {
  private:
    PHLANIMVAR<float> scale;
    PHLANIMVAR<Vector2D> offset;

    int64_t map_base = 0;

    // Regular numbered workspaces on this monitor that are outside the static
    // map. Stable, sorted by workspace ID; never moved by the plugin.
    std::vector<WORKSPACEID> recovered_ids;

    // The workspace represented by each of the current page's 9 tiles, or
    // nullopt for an empty Recovered slot.
    std::array<std::optional<WORKSPACEID>, TilesGrid::TILES_PER_PAGE> tile_ids {};

    // Tile geometry for the current page, including empty Recovered slots.
    std::array<CBox, TilesGrid::TILES_PER_PAGE> tile_boxes {};

    // Cached, offscreen render of each tile's workspace. Re-rendering all nine
    // tiles into the main framebuffer every frame is the overview's dominant
    // GPU cost; these let the overview composite cheap textures and refresh a
    // few tiles per frame instead.
    struct CachedTile {
        SP<Render::IFramebuffer> fb;
        WORKSPACEID id = WORKSPACE_INVALID;
        Vector2D size {};
        int windows = -1;
        bool dirty = true;
    };
    std::array<CachedTile, TilesGrid::TILES_PER_PAGE> cached_tiles;
    size_t next_refresh = 0;
    uint64_t refresh_tick = 0;
    bool cache_warm = false;

    bool render_tile_cache(
        int index,
        WORKSPACEID slot_id,
        PHLMONITOR monitor,
        PHLWORKSPACE workspace,
        const CBox& box
    );
    void render_cached_previews();
    void invalidate_cached_tiles();

    struct PageTab {
        int page = 0;
        CBox box;
    };

    std::vector<PageTab> page_tabs();
    std::string page_label(int viewed_page) const;
    CBox closed_box_for_page_tile(int page, int tile) const;
    void reanchor_to(WORKSPACEID id);
    int64_t stride() const;
    int pages() const;

  public:
    HTLayoutGrid(VIEWID view_id);
    virtual ~HTLayoutGrid() = default;

    virtual std::string layout_name();

    virtual CBox calculate_ws_box(int x, int y, HTViewStage stage);

    virtual void close_open_lerp(float perc);
    virtual void on_show(CallbackFun on_complete);
    virtual void on_hide(CallbackFun on_complete);
    virtual void on_move(WORKSPACEID old_id, WORKSPACEID new_id, CallbackFun on_complete);
    virtual void on_move_swipe(Vector2D delta);
    virtual WORKSPACEID on_move_swipe_end();

    virtual WORKSPACEID get_ws_id_in_direction(int x, int y, std::string& direction);

    virtual bool should_render_window(PHLWINDOW window);
    virtual float drag_window_scale();
    virtual void init_position();
    virtual void build_overview_layout(HTViewStage stage);
    virtual void render();
    virtual void refresh_cached_previews() override;

    bool previews_cached() const;

    virtual bool on_mouse_axis(double delta) override;
    virtual std::optional<int> page_strip_at(Vector2D pos) override;
    virtual void render_page_strip() override;

    void set_monitor_base(int64_t base);
    int64_t monitor_base() const { return map_base; }

    // Read-only reconciliation of the Recovered list.
    void refresh_recovered();
    const std::vector<WORKSPACEID>& recovered_workspaces() const { return recovered_ids; }

    bool is_recovered_view() const;
    int recovered_subpage_index() const;
    int visible_page_count() const;

    // Fixed mapping helpers.
    std::optional<WORKSPACEID> tile_workspace_at(int viewed_page, int tile) const;
    std::optional<WORKSPACEID> viewed_tile_workspace(int tile) const;
    std::optional<TilesGrid::PageTile> page_tile_of(WORKSPACEID id) const;
    int page_of_workspace(WORKSPACEID id) const;
    std::optional<int> tile_index_at(Vector2D global);

    int normal_page_count() const;
    void set_viewed_page(int viewed_page) override;
    void change_page(int delta);

    int page_strip_height();
    CBox page_strip_box();
};

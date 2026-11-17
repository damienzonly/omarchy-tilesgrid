#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace TilesGrid {

inline constexpr int ROWS = 3;
inline constexpr int COLS = 3;
inline constexpr int TILES_PER_PAGE = ROWS * COLS;
inline constexpr int DEFAULT_PAGES = 3;
inline constexpr int DEFAULT_PAGE_STRIDE = 100;
inline constexpr int DEFAULT_FALLBACK_BASE = 30;
inline constexpr int MONITOR_BASE_STEP = 10;

struct PageTile {
    int page = 0; // 0-based
    int tile = 1; // 1-based, row-major (1..9)
};

// Fixed map: monitor base + page * stride + tile.
inline int64_t tile_workspace(int64_t base, int page, int tile, int stride = DEFAULT_PAGE_STRIDE) {
    return base + static_cast<int64_t>(page) * stride + tile;
}

// Inverse. Returns nullopt for anything outside this monitor's map (a recovered
// workspace, a special/named workspace, or an empty spacer ID).
inline std::optional<PageTile> page_tile_of_workspace(
    int64_t id,
    int64_t base,
    int pages,
    int stride = DEFAULT_PAGE_STRIDE
) {
    if (id <= 0 || base < 0 || pages <= 0 || stride <= 0)
        return std::nullopt;

    const int64_t rel = id - base - 1;
    if (rel < 0)
        return std::nullopt;

    const int64_t page = rel / stride;
    const int64_t tile0 = rel % stride;
    if (page >= pages || tile0 >= TILES_PER_PAGE)
        return std::nullopt;

    return PageTile {static_cast<int>(page), static_cast<int>(tile0) + 1};
}

inline bool in_map(int64_t id, int64_t base, int pages, int stride = DEFAULT_PAGE_STRIDE) {
    return page_tile_of_workspace(id, base, pages, stride).has_value();
}

inline bool is_recovered_page(int viewed_page, int pages) {
    return viewed_page >= pages;
}

inline int recovered_subpage(int viewed_page, int pages) {
    return viewed_page - pages;
}

inline int viewed_page_for_recovered(int recovered_subpage_index, int pages) {
    return pages + recovered_subpage_index;
}

inline int recovered_subpage_count(size_t recovered_count) {
    if (recovered_count == 0)
        return 0;
    return static_cast<int>((recovered_count + TILES_PER_PAGE - 1) / TILES_PER_PAGE);
}

// Row-major tile index (0-based) -> x/y.
inline int tile_x(int tile) {
    return (tile - 1) % COLS;
}

inline int tile_y(int tile) {
    return (tile - 1) / COLS;
}

} // namespace TilesGrid

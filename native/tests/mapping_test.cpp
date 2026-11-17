#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>

#include "mapping.hpp"

int main() {
    using namespace TilesGrid;

    std::set<int64_t> ids;

    for (int64_t base : {0, 10, 20, 30, 90}) {
        for (int page = 0; page < DEFAULT_PAGES; page++) {
            for (int tile = 1; tile <= TILES_PER_PAGE; tile++) {
                const int64_t id = tile_workspace(base, page, tile);
                const auto inverse = page_tile_of_workspace(id, base, DEFAULT_PAGES);

                assert(inverse.has_value());
                assert(inverse->page == page);
                assert(inverse->tile == tile);
                assert(in_map(id, base, DEFAULT_PAGES));
                assert(ids.insert(id).second);
            }
        }
    }

    // Page 1 keeps the existing workspace IDs.
    assert(tile_workspace(0, 0, 1) == 1);
    assert(tile_workspace(0, 0, 9) == 9);
    assert(tile_workspace(10, 0, 1) == 11);
    assert(tile_workspace(10, 0, 9) == 19);
    assert(tile_workspace(20, 0, 1) == 21);
    assert(tile_workspace(20, 0, 9) == 29);

    // Pages 2 and 3 preserve the 100-stride.
    assert(tile_workspace(0, 1, 1) == 101);
    assert(tile_workspace(0, 2, 9) == 209);
    assert(tile_workspace(20, 1, 1) == 121);
    assert(tile_workspace(20, 2, 9) == 229);

    // Spacers, monitor bases, and other gaps are out-of-map.
    for (int64_t id : {10, 20, 30, 100, 110, 120, 200, 210, 300, 1000})
        assert(!in_map(id, 20, DEFAULT_PAGES));

    // Recovered pagination.
    assert(recovered_subpage_count(0) == 0);
    assert(recovered_subpage_count(1) == 1);
    assert(recovered_subpage_count(9) == 1);
    assert(recovered_subpage_count(10) == 2);
    assert(recovered_subpage_count(27) == 3);
    assert(viewed_page_for_recovered(0, 3) == 3);
    assert(viewed_page_for_recovered(2, 3) == 5);
    assert(is_recovered_page(3, 3));
    assert(!is_recovered_page(2, 3));

    // Tile coordinates are row-major.
    assert(tile_x(1) == 0 && tile_y(1) == 0);
    assert(tile_x(3) == 2 && tile_y(3) == 0);
    assert(tile_x(4) == 0 && tile_y(4) == 1);
    assert(tile_x(9) == 2 && tile_y(9) == 2);

    std::cout << "mapping tests passed\n";
    return 0;
}

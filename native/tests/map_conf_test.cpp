#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "map.hpp"

int main() {
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / "tilesgrid-map-test";
    fs::remove_all(root);

    const fs::path state_dir = root / ".local/state/tilesgrid";
    fs::create_directories(state_dir);

    const fs::path map_file = state_dir / "map.conf";
    {
        std::ofstream file(map_file);
        file << "pages=4\n";
        file << "stride=200\n";
        file << "fallback=50\n";
        file << "monitor=FOO MONITOR|5\n";
        file << "monitor=BAR MONITOR|15\n";
        file << "monitor=ACME FOO MONITOR|35\n";
        file << "monitor=DP-3|25\n";
    }

    setenv("HOME", root.c_str(), 1);
    TilesGridMap::reload();

    const auto& cfg = TilesGridMap::current();
    assert(cfg.pages == 4);
    assert(cfg.stride == 200);
    assert(cfg.fallback == 50);
    assert(cfg.entries.size() == 4);

    const auto foo = cfg.base_for("ACME FOO MONITOR 123", "DP-9");
    assert(foo.has_value() && *foo == 5);

    const auto bar = cfg.base_for("unrelated", "BAR MONITOR connector");
    assert(bar.has_value() && *bar == 15);

    // Exact description beats a shorter substring entry.
    const auto exact = cfg.base_for("ACME FOO MONITOR", "DP-9");
    assert(exact.has_value() && *exact == 35);

    // An exact connector name (used for duplicate descriptions) beats a
    // substring description match.
    const auto connector = cfg.base_for("ACME FOO MONITOR 123", "DP-3");
    assert(connector.has_value() && *connector == 25);

    const auto missing = cfg.base_for("unrelated", "DP-1");
    assert(!missing.has_value());

    fs::remove(map_file);
    TilesGridMap::reload();

    const auto& defaults = TilesGridMap::current();
    assert(defaults.pages == 3);
    assert(defaults.stride == 100);
    assert(defaults.fallback == 30);
    assert(defaults.entries.empty());
    assert(!defaults.base_for("Samsung Electric Company LC34G55T", "DP-5").has_value());

    fs::remove_all(root);

    std::cout << "map.conf tests passed\n";
    return 0;
}

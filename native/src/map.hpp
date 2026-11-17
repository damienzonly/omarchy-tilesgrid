#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TilesGridMap {

struct Entry {
    std::string match;
    int64_t base = 0;
};

struct Config {
    int pages = 3;
    int stride = 100;
    int fallback = 30;
    std::vector<Entry> entries;

    std::optional<int64_t> base_for(const std::string& description, const std::string& name) const;
};

std::string path();
Config load();
const Config& current();
void reload();

} // namespace TilesGridMap

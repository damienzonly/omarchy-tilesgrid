#include "map.hpp"

#include <cstdlib>
#include <fstream>

#include "mapping.hpp"

namespace TilesGridMap {

namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

int parse_int(const std::string& value, int fallback) {
    try {
        const int parsed = std::stoi(value);
        return parsed;
    } catch (...) {
        return fallback;
    }
}

int64_t parse_i64(const std::string& value, int64_t fallback) {
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

Config defaults() {
    Config cfg;
    cfg.pages = TilesGrid::DEFAULT_PAGES;
    cfg.stride = TilesGrid::DEFAULT_PAGE_STRIDE;
    cfg.fallback = TilesGrid::DEFAULT_FALLBACK_BASE;
    return cfg;
}

} // namespace

std::string path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        return "/tmp/tilesgrid-map.conf";
    return std::string(home) + "/.local/state/tilesgrid/map.conf";
}

Config load() {
    Config cfg = defaults();

    std::ifstream file(path());
    if (!file)
        return cfg;

    Config parsed = defaults();
    parsed.entries.clear();

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "pages") {
            parsed.pages = parse_int(value, parsed.pages);
        } else if (key == "stride") {
            parsed.stride = parse_int(value, parsed.stride);
        } else if (key == "fallback") {
            parsed.fallback = parse_int(value, parsed.fallback);
        } else if (key == "monitor") {
            const auto bar = value.rfind('|');
            if (bar == std::string::npos)
                continue;
            const std::string match = trim(value.substr(0, bar));
            const std::string base = trim(value.substr(bar + 1));
            if (match.empty())
                continue;
            parsed.entries.push_back({match, parse_i64(base, 0)});
        }
    }

    if (parsed.pages <= 0)
        parsed.pages = TilesGrid::DEFAULT_PAGES;
    if (parsed.stride <= 0)
        parsed.stride = TilesGrid::DEFAULT_PAGE_STRIDE;
    if (parsed.fallback < 0)
        parsed.fallback = TilesGrid::DEFAULT_FALLBACK_BASE;

    return parsed;
}

std::optional<int64_t> Config::base_for(
    const std::string& description,
    const std::string& name
) const {
    // Exact matches win so a duplicated description stored by connector name
    // (or a name that is a prefix of another) cannot claim the wrong base.
    for (const auto& entry : entries) {
        if (!entry.match.empty() && entry.match == description)
            return entry.base;
    }
    for (const auto& entry : entries) {
        if (!entry.match.empty() && entry.match == name)
            return entry.base;
    }
    for (const auto& entry : entries) {
        if (!entry.match.empty() && description.find(entry.match) != std::string::npos)
            return entry.base;
    }
    for (const auto& entry : entries) {
        if (!entry.match.empty() && name.find(entry.match) != std::string::npos)
            return entry.base;
    }
    return std::nullopt;
}

const Config& current() {
    static Config cfg = load();
    return cfg;
}

void reload() {
    // current() owns the storage; assign through a mutable reference.
    const_cast<Config&>(current()) = load();
}

} // namespace TilesGridMap

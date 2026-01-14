#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

namespace ouroboros::backend {

struct Config {
    // Playback settings
    int default_volume = 50;
    bool shuffle = false;
    std::string repeat = "all";

    // UI settings
    std::string layout = "default";
    std::string theme = "dark";
    bool enable_album_art = true;
    int album_grid_columns = 4;

    // Sorting settings
    bool sort_albums_by_year = true;  // true = by release year (numeric), false = alphabetical
    bool sort_ignore_the_prefix = true;  // Ignore "The " prefix when sorting artists
    bool sort_ignore_bracket_prefix = true;  // Ignore "[" prefix when sorting artists

    // Keybinds
    std::unordered_map<std::string, std::string> keybinds;

    // Directory settings - supports multiple directories
    std::vector<std::filesystem::path> music_directories;

    // Performance settings (cached at load time, O(1) access)
    int artwork_max_workers = 0;        // 0 = auto (hardware_concurrency)
    int artwork_prefetch_items = 100;   // Items to prefetch beyond viewport
    int artwork_spawn_threshold = 10;   // Queue depth per worker to spawn new worker
    int artwork_memory_limit_mb = 3072; // Memory pressure limit for artwork cache (MB)

    // O(1) accessors - return by reference, no allocation
    const std::vector<std::filesystem::path>& get_music_directories() const { return music_directories; }
    int get_artwork_max_workers() const { return artwork_max_workers; }
    int get_artwork_prefetch_items() const { return artwork_prefetch_items; }
    int get_artwork_spawn_threshold() const { return artwork_spawn_threshold; }
    int get_artwork_memory_limit_mb() const { return artwork_memory_limit_mb; }

    static Config& instance();
};

class ConfigLoader {
public:
    static Config load_config();
    static Config load_from_file(const std::filesystem::path& path);
    static void save_config(const Config& cfg, const std::filesystem::path& path);

private:
    static std::filesystem::path get_config_file();
    static Config create_default_config();
};

}  // namespace ouroboros::backend

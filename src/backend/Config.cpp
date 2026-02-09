#include "backend/Config.hpp"
#include "util/TomlReader.hpp"
#include "util/Logger.hpp"
#include <cstdlib>
#include <fstream>
#include <string>

namespace ouroboros::backend {

Config& Config::instance() {
    static Config instance;
    return instance;
}

Config ConfigLoader::load_config() {
    ouroboros::util::Logger::info("Config: Loading configuration");

    auto config_file = get_config_file();
    Config cfg;
    if (std::filesystem::exists(config_file)) {
        cfg = load_from_file(config_file);
    } else {
        cfg = create_default_config();
        save_config(cfg, config_file);
        ouroboros::util::Logger::info("Config: Created default config at " + config_file.string());
    }

    Config::instance() = cfg;
    return cfg;
}

// Parse music_directories array: ["path1", "path2", ...]
static void parse_music_dirs(const std::string& value, std::vector<std::filesystem::path>& out) {
    out.clear();
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return;

    std::string inner = value.substr(1, value.size() - 2);
    size_t pos = 0;
    while (pos < inner.size()) {
        while (pos < inner.size() && (inner[pos] == ' ' || inner[pos] == '\t' || inner[pos] == ',')) pos++;
        if (pos >= inner.size()) break;

        if (inner[pos] == '"') {
            size_t qstart = pos + 1;
            size_t qend = inner.find('"', qstart);
            if (qend == std::string::npos) break;
            std::string path_str = inner.substr(qstart, qend - qstart);
            if (!path_str.empty() && path_str[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path_str = std::string(home) + path_str.substr(1);
            }
            out.emplace_back(path_str);
            pos = qend + 1;
        } else {
            pos++;
        }
    }
}

Config ConfigLoader::load_from_file(const std::filesystem::path& path) {
    ouroboros::util::Logger::debug("Config: Loading from file");

    Config cfg = create_default_config();

    ouroboros::util::TomlReader toml;
    if (!toml.load(path.string())) return cfg;

    // [playback]
    cfg.default_volume = toml.get_int("playback", "default_volume", cfg.default_volume);
    cfg.shuffle = toml.get_bool("playback", "shuffle", cfg.shuffle);
    cfg.repeat = toml.get_string("playback", "repeat", cfg.repeat);

    // [ui]
    cfg.layout = toml.get_string("ui", "layout", cfg.layout);
    cfg.enable_album_art = toml.get_bool("ui", "enable_album_art", cfg.enable_album_art);
    cfg.album_grid_columns = toml.get_int("ui", "album_grid_columns", cfg.album_grid_columns);
    cfg.sort_albums_by_year = toml.get_bool("ui", "sort_albums_by_year", cfg.sort_albums_by_year);
    cfg.sort_ignore_the_prefix = toml.get_bool("ui", "sort_ignore_the_prefix", cfg.sort_ignore_the_prefix);
    cfg.sort_ignore_bracket_prefix = toml.get_bool("ui", "sort_ignore_bracket_prefix", cfg.sort_ignore_bracket_prefix);

    // [keybinds] - check known keys from defaults
    for (auto& [key, value] : cfg.keybinds) {
        if (toml.has("keybinds", key)) {
            value = toml.get_string("keybinds", key, value);
        }
    }

    // [library] / [paths] music_directories (array format, inline parse)
    if (toml.has("library", "music_directories")) {
        parse_music_dirs(toml.get_string("library", "music_directories"), cfg.music_directories);
    } else if (toml.has("paths", "music_directories")) {
        parse_music_dirs(toml.get_string("paths", "music_directories"), cfg.music_directories);
    }
    // Legacy single-directory support
    if (toml.has("library", "music_directory")) {
        std::string path_str = toml.get_string("library", "music_directory");
        if (!path_str.empty() && path_str[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) path_str = std::string(home) + path_str.substr(1);
        }
        cfg.music_directories.clear();
        cfg.music_directories.emplace_back(path_str);
    }

    // [performance]
    cfg.artwork_max_workers = toml.get_int("performance", "artwork_max_workers", cfg.artwork_max_workers);
    cfg.artwork_prefetch_items = toml.get_int("performance", "artwork_prefetch_items", cfg.artwork_prefetch_items);
    cfg.artwork_spawn_threshold = toml.get_int("performance", "artwork_spawn_threshold", cfg.artwork_spawn_threshold);
    cfg.artwork_memory_limit_mb = toml.get_int("performance", "artwork_memory_limit_mb", cfg.artwork_memory_limit_mb);

    return cfg;
}

void ConfigLoader::save_config(const Config& cfg, const std::filesystem::path& path) {
    ouroboros::util::Logger::info("Config: Saving configuration");

    std::filesystem::create_directories(path.parent_path());

    std::ofstream file(path);
    if (!file) return;

    file << "# OUROBOROS Config\n";
    file << "# Generated on install; edit with care\n\n";

    file << "[playback]\n";
    file << "default_volume = " << cfg.default_volume << "\n";
    file << "shuffle = " << (cfg.shuffle ? "true" : "false") << "\n";
    file << "repeat = \"" << cfg.repeat << "\"\n\n";

    file << "[ui]\n";
    file << "layout = \"" << cfg.layout << "\"\n";
    file << "enable_album_art = " << (cfg.enable_album_art ? "true" : "false") << "\n";
    file << "album_grid_columns = " << cfg.album_grid_columns << "\n";
    file << "sort_albums_by_year = " << (cfg.sort_albums_by_year ? "true" : "false") << "\n";
    file << "sort_ignore_the_prefix = " << (cfg.sort_ignore_the_prefix ? "true" : "false") << "\n";
    file << "sort_ignore_bracket_prefix = " << (cfg.sort_ignore_bracket_prefix ? "true" : "false") << "\n\n";

    file << "[keybinds]\n";
    for (const auto& [key, value] : cfg.keybinds) {
        file << key << " = \"" << value << "\"\n";
    }
    file << "\n";

    file << "[library]\n";
    if (!cfg.music_directories.empty()) {
        file << "music_directories = [";
        for (size_t i = 0; i < cfg.music_directories.size(); ++i) {
            if (i > 0) file << ", ";
            file << "\"" << cfg.music_directories[i].string() << "\"";
        }
        file << "]\n";
    } else {
        file << "# music_directories = [\"~/Music\"]\n";
    }

    file << "\n[performance]\n";
    file << "artwork_max_workers = " << cfg.artwork_max_workers << "\n";
    file << "artwork_prefetch_items = " << cfg.artwork_prefetch_items << "\n";
    file << "artwork_spawn_threshold = " << cfg.artwork_spawn_threshold << "\n";
    file << "artwork_memory_limit_mb = " << cfg.artwork_memory_limit_mb << "\n";

    // Semantic color roles -- these drive the entire UI.
    // Values: named color (e.g. "green"), hex "#RRGGBB", or palette index (integer).
    // Named colors use your terminal palette, hex bypasses it with truecolor.
    file << "\n[roles]\n";
    file << "selection = \"brightyellow\"\n";
    file << "marked = \"yellow\"\n";
    file << "artist = \"cyan\"\n";
    file << "title = \"brightwhite\"\n";
    file << "album = \"blue\"\n";
    file << "muted = \"brightblack\"\n";
    file << "artwork_border = \"brightblack\"\n";
    file << "widget_border = \"brightblack\"\n";
    file << "accent = \"green\"\n";
    file << "separator = \"brightblack\"\n";
    file << "warning = \"red\"\n";
    file << "heading = \"brightyellow\"\n";
    file << "focus_title = \"green\"\n";

    // Optional: define a custom palette to use with integer role values.
    // e.g. artist = 14 would look up color14 below.
    file << "\n# [palette]\n";
    file << "# color0  = \"#1E1E2E\"\n";
    file << "# color14 = \"#94E2D5\"\n";
}

std::filesystem::path ConfigLoader::get_config_file() {
    auto home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "ouroboros" / "config.toml";
    }
    return ".config/ouroboros/config.toml";
}

Config ConfigLoader::create_default_config() {
    Config cfg;
    const char* home = std::getenv("HOME");
    if (home) {
        cfg.music_directories.emplace_back(std::filesystem::path(home) / "Music");
    }

    cfg.keybinds["quit"] = "q";
    cfg.keybinds["play"] = "space";
    cfg.keybinds["next"] = "n";
    cfg.keybinds["prev"] = "p";
    cfg.keybinds["volume_up"] = "+";
    cfg.keybinds["volume_down"] = "-";
    cfg.keybinds["seek_forward"] = "right";
    cfg.keybinds["seek_backward"] = "left";
    cfg.keybinds["repeat_cycle"] = "r";
    cfg.keybinds["shuffle_toggle"] = "s";
    cfg.keybinds["toggle_album_view"] = "ctrl+a";
    cfg.keybinds["clear_queue"] = "ctrl+d";
    cfg.keybinds["search"] = "ctrl+f";
    cfg.keybinds["help"] = "?";
    cfg.keybinds["tab"] = "tab";
    cfg.keybinds["nav_up"] = "k";
    cfg.keybinds["nav_down"] = "j";
    cfg.keybinds["nav_left"] = "h";
    cfg.keybinds["nav_right"] = "l";
    cfg.keybinds["nav_select_up"] = "K";
    cfg.keybinds["nav_select_down"] = "J";
    cfg.keybinds["select"] = "enter";

    return cfg;
}

}  // namespace ouroboros::backend

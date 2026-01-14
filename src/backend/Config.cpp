#include "backend/Config.hpp"
#include <cstdlib>
#include <fstream>
#include <string>
#include "util/Logger.hpp"

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
    }
    
    // Update global singleton
    Config::instance() = cfg;
    return cfg;
}

Config ConfigLoader::load_from_file(const std::filesystem::path& path) {
    ouroboros::util::Logger::debug("Config: Loading from file");

    Config cfg = create_default_config();
    
    std::ifstream file(path);
    if (!file) return cfg;
    
    std::string line, current_section;
    while (std::getline(file, line)) {
        // Trim whitespace
        auto start = line.find_first_not_of(" \t");
        auto end = line.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start, end - start + 1);
        
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        // Section header
        if (line[0] == '[' && line.back() == ']') {
            current_section = line.substr(1, line.length() - 2);
            continue;
        }
        
        // Key = value
        auto eq_pos = line.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 1);
            
            // Trim key and value
            auto key_start = key.find_first_not_of(" \t");
            auto key_end = key.find_last_not_of(" \t");
            if (key_start != std::string::npos) {
                key = key.substr(key_start, key_end - key_start + 1);
            }
            
            auto val_start = value.find_first_not_of(" \t");
            auto val_end = value.find_last_not_of(" \t");
            if (val_start != std::string::npos) {
                value = value.substr(val_start, val_end - val_start + 1);
            }
            
            // Remove quotes from strings
            if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.length() - 2);
            }
            
            // Parse based on section
            if (current_section == "playback") {
                if (key == "default_volume") {
                    try { cfg.default_volume = std::stoi(value); } catch(...) {}
                }
                else if (key == "shuffle") cfg.shuffle = (value == "true");
                else if (key == "repeat") cfg.repeat = value;
            }
            else if (current_section == "ui") {
                if (key == "layout") cfg.layout = value;
                else if (key == "theme") cfg.theme = value;
                else if (key == "enable_album_art") cfg.enable_album_art = (value == "true");
                else if (key == "album_grid_columns") {
                    try { cfg.album_grid_columns = std::stoi(value); } catch(...) {}
                }
                else if (key == "sort_albums_by_year") cfg.sort_albums_by_year = (value == "true");
                else if (key == "sort_ignore_the_prefix") cfg.sort_ignore_the_prefix = (value == "true");
                else if (key == "sort_ignore_bracket_prefix") cfg.sort_ignore_bracket_prefix = (value == "true");
            }
            else if (current_section == "keybinds") {
                cfg.keybinds[key] = value;
            }
            else if (current_section == "library" || current_section == "paths") {
                if (key == "music_directories") {
                    // Parse array format: ["path1", "path2", ...]
                    cfg.music_directories.clear();
                    if (value.front() == '[' && value.back() == ']') {
                        std::string inner = value.substr(1, value.length() - 2);
                        size_t pos = 0;
                        while (pos < inner.length()) {
                            // Skip whitespace
                            while (pos < inner.length() && (inner[pos] == ' ' || inner[pos] == '\t' || inner[pos] == ',')) pos++;
                            if (pos >= inner.length()) break;

                            // Find quoted string
                            if (inner[pos] == '"') {
                                size_t start = pos + 1;
                                size_t end = inner.find('"', start);
                                if (end != std::string::npos) {
                                    std::string path_str = inner.substr(start, end - start);
                                    // Expand ~ to home directory
                                    if (!path_str.empty() && path_str[0] == '~') {
                                        const char* home = std::getenv("HOME");
                                        if (home) path_str = std::string(home) + path_str.substr(1);
                                    }
                                    cfg.music_directories.emplace_back(path_str);
                                    pos = end + 1;
                                } else {
                                    break;
                                }
                            } else {
                                pos++;
                            }
                        }
                    }
                }
                else if (key == "music_directory") {
                    // Legacy single directory support
                    std::string path_str = value;
                    if (!path_str.empty() && path_str[0] == '~') {
                        const char* home = std::getenv("HOME");
                        if (home) path_str = std::string(home) + path_str.substr(1);
                    }
                    cfg.music_directories.clear();
                    cfg.music_directories.emplace_back(path_str);
                }
            }
            else if (current_section == "performance") {
                if (key == "artwork_max_workers") {
                    try { cfg.artwork_max_workers = std::stoi(value); } catch(...) {}
                }
                else if (key == "artwork_prefetch_items") {
                    try { cfg.artwork_prefetch_items = std::stoi(value); } catch(...) {}
                }
                else if (key == "artwork_spawn_threshold") {
                    try { cfg.artwork_spawn_threshold = std::stoi(value); } catch(...) {}
                }
                else if (key == "artwork_memory_limit_mb") {
                    try { cfg.artwork_memory_limit_mb = std::stoi(value); } catch(...) {}
                }
            }
        }
    }

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
    file << "# Default volume level (0-100)\n";
    file << "default_volume = " << cfg.default_volume << "\n\n";
    file << "# Enable shuffle by default\n";
    file << "shuffle = " << (cfg.shuffle ? "true" : "false") << "\n\n";
    file << "# Repeat mode: \"off\", \"one\", \"all\"\n";
    file << "repeat = \"" << cfg.repeat << "\"\n\n";

    file << "[ui]\n";
    file << "# Layout mode: \"default\", \"queue\", \"browser\"\n";
    file << "layout = \"" << cfg.layout << "\"\n\n";
    file << "# Theme: \"dark\", \"light\", \"monokai\"\n";
    file << "theme = \"" << cfg.theme << "\"\n\n";
    file << "# Enable album art display\n";
    file << "enable_album_art = " << (cfg.enable_album_art ? "true" : "false") << "\n\n";
    file << "# Number of album columns in grid view (default: 4)\n";
    file << "album_grid_columns = " << cfg.album_grid_columns << "\n\n";
    file << "# Sort albums by year instead of alphabetically\n";
    file << "sort_albums_by_year = " << (cfg.sort_albums_by_year ? "true" : "false") << "\n\n";
    file << "# Ignore 'The ' prefix when sorting artists (e.g., 'The Beatles' sorts as 'Beatles')\n";
    file << "sort_ignore_the_prefix = " << (cfg.sort_ignore_the_prefix ? "true" : "false") << "\n\n";
    file << "# Ignore '[' prefix when sorting artists (e.g., '[Unknown]' sorts as 'Unknown]')\n";
    file << "sort_ignore_bracket_prefix = " << (cfg.sort_ignore_bracket_prefix ? "true" : "false") << "\n\n";

    file << "[keybinds]\n";
    file << "# Playback controls\n";
    file << "play = \"" << (cfg.keybinds.count("play") ? cfg.keybinds.at("play") : "space") << "\"\n";
    file << "pause = \"" << (cfg.keybinds.count("pause") ? cfg.keybinds.at("pause") : "p") << "\"\n";
    file << "next = \"" << (cfg.keybinds.count("next") ? cfg.keybinds.at("next") : "n") << "\"\n";
    file << "prev = \"" << (cfg.keybinds.count("prev") ? cfg.keybinds.at("prev") : "N") << "\"\n\n";

    file << "# Quit\n";
    file << "quit = \"" << (cfg.keybinds.count("quit") ? cfg.keybinds.at("quit") : "q") << "\"\n\n";

    file << "# Volume control\n";
    file << "volume_up = \"" << (cfg.keybinds.count("volume_up") ? cfg.keybinds.at("volume_up") : "+") << "\"\n";
    file << "volume_down = \"" << (cfg.keybinds.count("volume_down") ? cfg.keybinds.at("volume_down") : "-") << "\"\n\n";

    file << "# Seek in song\n";
    file << "seek_forward = \"" << (cfg.keybinds.count("seek_forward") ? cfg.keybinds.at("seek_forward") : "j") << "\"\n";
    file << "seek_backward = \"" << (cfg.keybinds.count("seek_backward") ? cfg.keybinds.at("seek_backward") : "k") << "\"\n\n";

    file << "# Navigation\n";
    file << "tab_queue = \"" << (cfg.keybinds.count("tab_queue") ? cfg.keybinds.at("tab_queue") : "1") << "\"\n";
    file << "tab_browser = \"" << (cfg.keybinds.count("tab_browser") ? cfg.keybinds.at("tab_browser") : "2") << "\"\n";
    file << "tab_now_playing = \"" << (cfg.keybinds.count("tab_now_playing") ? cfg.keybinds.at("tab_now_playing") : "3") << "\"\n\n";

    file << "# Display toggles\n";
    file << "toggle_header = \"" << (cfg.keybinds.count("toggle_header") ? cfg.keybinds.at("toggle_header") : "H") << "\"\n";
    file << "toggle_controls = \"" << (cfg.keybinds.count("toggle_controls") ? cfg.keybinds.at("toggle_controls") : "C") << "\"\n";
    file << "toggle_browser = \"" << (cfg.keybinds.count("toggle_browser") ? cfg.keybinds.at("toggle_browser") : "B") << "\"\n";
    file << "toggle_queue = \"" << (cfg.keybinds.count("toggle_queue") ? cfg.keybinds.at("toggle_queue") : "Q") << "\"\n\n";

    file << "# Modes\n";
    file << "shuffle_toggle = \"" << (cfg.keybinds.count("shuffle_toggle") ? cfg.keybinds.at("shuffle_toggle") : "S") << "\"\n";
    file << "repeat_cycle = \"" << (cfg.keybinds.count("repeat_cycle") ? cfg.keybinds.at("repeat_cycle") : "R") << "\"\n\n";

    file << "[library]\n";
    file << "# Music library directories (array format)\n";
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
    file << "# 0 = auto (uses hardware_concurrency)\n";
    file << "artwork_max_workers = " << cfg.artwork_max_workers << "\n";
    file << "artwork_prefetch_items = " << cfg.artwork_prefetch_items << "\n";
    file << "artwork_spawn_threshold = " << cfg.artwork_spawn_threshold << "\n";
    file << "# Memory limit for artwork cache in MB (triggers eviction when exceeded)\n";
    file << "artwork_memory_limit_mb = " << cfg.artwork_memory_limit_mb << "\n";
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
    // Default to ~/Music
    const char* home = std::getenv("HOME");
    if (home) {
        cfg.music_directories.emplace_back(std::filesystem::path(home) / "Music");
    }
    cfg.keybinds["play"] = "space";
    cfg.keybinds["pause"] = "p";
    cfg.keybinds["next"] = "n";
    cfg.keybinds["quit"] = "q";
    return cfg;
}

}  // namespace ouroboros::backend

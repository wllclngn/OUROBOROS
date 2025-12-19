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
            }
            else if (current_section == "keybinds") {
                cfg.keybinds[key] = value;
            }
            else if (current_section == "library" || current_section == "paths") {
                if (key == "music_directory") {
                    cfg.music_directory = std::filesystem::path(value);
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

    file << "[paths]\n";
    file << "# Music library directory\n";
    if (!cfg.music_directory.empty()) {
        file << "music_directory = \"" << cfg.music_directory.string() << "\"\n";
    } else {
        file << "# music_directory = \"~/Music\"\n";
    }
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
    cfg.music_directory = ConfigLoader::get_config_file().parent_path();
    cfg.keybinds["play"] = "space";
    cfg.keybinds["pause"] = "p";
    cfg.keybinds["next"] = "n";
    cfg.keybinds["quit"] = "q";
    return cfg;
}

}  // namespace ouroboros::backend

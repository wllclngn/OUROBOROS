#include "backend/Config.hpp"
#include <cstdlib>
#include <fstream>
#include <string>
#include "util/Logger.hpp"

namespace ouroboros::backend {

Config ConfigLoader::load_config() {
    ouroboros::util::Logger::info("Config: Loading configuration");

    auto config_file = get_config_file();
    if (std::filesystem::exists(config_file)) {
        return load_from_file(config_file);
    }
    return create_default_config();
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
    
    file << "# OUROBOROS Configuration File\n\n";
    
    file << "[playback]\n";
    file << "default_volume = " << cfg.default_volume << "\n";
    file << "shuffle = " << (cfg.shuffle ? "true" : "false") << "\n";
    file << "repeat = \"" << cfg.repeat << "\"\n\n";
    
    file << "[ui]\n";
    file << "layout = \"" << cfg.layout << "\"\n";
    file << "theme = \"" << cfg.theme << "\"\n";
    file << "enable_album_art = " << (cfg.enable_album_art ? "true" : "false") << "\n\n";
    
    file << "[keybinds]\n";
    for (const auto& [key, value] : cfg.keybinds) {
        file << key << " = \"" << value << "\"\n";
    }
    file << "\n";
    
    if (!cfg.music_directory.empty()) {
        file << "[paths]\n";
        file << "music_directory = \"" << cfg.music_directory.string() << "\"\n";
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

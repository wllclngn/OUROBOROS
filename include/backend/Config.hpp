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

    // Keybinds
    std::unordered_map<std::string, std::string> keybinds;

    // Directory settings
    std::filesystem::path music_directory;
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

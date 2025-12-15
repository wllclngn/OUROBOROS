#include "config/Theme.hpp"

namespace ouroboros::config {

// Define the static member
std::unordered_map<std::string, Theme> ThemeManager::themes_;

void ThemeManager::init_default_themes() {
    // ONLY ONE THEME: terminal (inherits from host terminal)
    themes_["terminal"] = {
        "terminal",
        "",           // background - inherit from terminal
        "",           // foreground - inherit from terminal  
        "\033[1m",    // current_track - bold (uses terminal's bold color)
        "\033[2m"     // highlight - dim (uses terminal's dim color)
    };
}

Theme ThemeManager::get_theme(const std::string& name) {
    // Initialize themes on first call
    if (themes_.empty()) {
        init_default_themes();
    }
    
    // Always return terminal theme - it's the ONLY theme
    (void)name;  // Ignore the parameter
    return themes_["terminal"];
}

void ThemeManager::register_theme(const std::string& name, const Theme& theme) {
    // Not used - we only have terminal theme
    // But must exist since it's in the interface
    (void)name;
    (void)theme;
}

}  // namespace ouroboros::config

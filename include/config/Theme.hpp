#pragma once

#include <string>
#include <unordered_map>

namespace ouroboros::config {

struct Theme {
    std::string name;
    std::string background;
    std::string foreground;
    std::string current_track;
    std::string highlight;
};

class ThemeManager {
public:
    static Theme get_theme(const std::string& name);
    static void register_theme(const std::string& name, const Theme& theme);

private:
    static std::unordered_map<std::string, Theme> themes_;
    static void init_default_themes();
};

}  // namespace ouroboros::config

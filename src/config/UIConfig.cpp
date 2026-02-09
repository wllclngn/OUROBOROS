#include "config/UIConfig.hpp"
#include "util/TomlReader.hpp"
#include <cstdlib>
#include <string>

namespace ouroboros::config {

using ouroboros::ui::Color;
using ouroboros::ui::Attribute;
using ouroboros::ui::Style;
using ouroboros::ui::rgb_color;

// Parse "#RRGGBB" hex string to truecolor Color.
static Color parse_hex_color(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return Color::Default;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = hv(hex[static_cast<size_t>(i) + 1]);
        if (v[i] < 0) return Color::Default;
    }
    return rgb_color(
        static_cast<uint8_t>(v[0] * 16 + v[1]),
        static_cast<uint8_t>(v[2] * 16 + v[3]),
        static_cast<uint8_t>(v[4] * 16 + v[5]));
}

// Parse named color string to terminal palette Color enum.
// Accepts lowercase names: "black", "red", "green", "yellow", "blue",
// "magenta", "cyan", "white", and "bright" prefixed variants.
static Color parse_named_color(const std::string& name) {
    if (name == "default")       return Color::Default;
    if (name == "black")         return Color::Black;
    if (name == "red")           return Color::Red;
    if (name == "green")         return Color::Green;
    if (name == "yellow")        return Color::Yellow;
    if (name == "blue")          return Color::Blue;
    if (name == "magenta")       return Color::Magenta;
    if (name == "cyan")          return Color::Cyan;
    if (name == "white")         return Color::White;
    if (name == "brightblack")   return Color::BrightBlack;
    if (name == "brightred")     return Color::BrightRed;
    if (name == "brightgreen")   return Color::BrightGreen;
    if (name == "brightyellow")  return Color::BrightYellow;
    if (name == "brightblue")    return Color::BrightBlue;
    if (name == "brightmagenta") return Color::BrightMagenta;
    if (name == "brightcyan")    return Color::BrightCyan;
    if (name == "brightwhite")   return Color::BrightWhite;
    return Color::Default;
}

// Resolve a color role from TOML [roles] -> [palette] -> compiled default.
// Value in [roles] can be:
//   "green", "brightyellow", ... -> terminal palette color
//   "#RRGGBB" -> direct truecolor
//   integer -> look up [palette] colorN hex -> truecolor
//   missing -> compiled default
static Color resolve_color(
    const ouroboros::util::TomlReader& toml, bool have_toml,
    const char* role, Color compiled_default) {

    if (!have_toml || !toml.has("roles", role))
        return compiled_default;

    std::string val = toml.get_string("roles", role);
    if (val.empty()) return compiled_default;

    // Direct hex override
    if (val.size() == 7 && val[0] == '#')
        return parse_hex_color(val);

    // Integer = palette index
    if (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-') {
        int idx = 0;
        try { idx = std::stoi(val); } catch (...) { return compiled_default; }

        std::string pkey = "color" + std::to_string(idx);
        if (toml.has("palette", pkey)) {
            auto hex = toml.get_string("palette", pkey);
            auto c = parse_hex_color(hex);
            if (c != Color::Default) return c;
        }
    }

    // Named color (e.g. "green", "brightyellow")
    auto named = parse_named_color(val);
    if (named != Color::Default) return named;

    return compiled_default;
}

const UIConfig& ui_config() {
    static UIConfig uic = [] {
        UIConfig c{};

        ouroboros::util::TomlReader toml;
        const char* home = std::getenv("HOME");
        std::string path;
        if (home) path = std::string(home) + "/.config/ouroboros/config.toml";
        bool have_toml = !path.empty() && toml.load(path);

        // Compiled defaults -- all 12 roles resolved from TOML [roles] -> [palette] -> fallback
        auto selection_fg  = resolve_color(toml, have_toml, "selection",    Color::BrightYellow);
        auto marked_fg     = resolve_color(toml, have_toml, "marked",       Color::Yellow);
        auto artist_fg     = resolve_color(toml, have_toml, "artist",       Color::Cyan);
        auto title_fg      = resolve_color(toml, have_toml, "title",        Color::BrightWhite);
        auto album_fg      = resolve_color(toml, have_toml, "album",        Color::Blue);
        auto muted_fg      = resolve_color(toml, have_toml, "muted",        Color::BrightBlack);
        auto border_fg     = resolve_color(toml, have_toml, "border",       Color::BrightBlack);
        auto accent_fg     = resolve_color(toml, have_toml, "accent",       Color::Green);
        auto separator_fg  = resolve_color(toml, have_toml, "separator",    Color::BrightBlack);
        auto warning_fg    = resolve_color(toml, have_toml, "warning",      Color::Red);
        auto heading_fg    = resolve_color(toml, have_toml, "heading",      Color::BrightYellow);
        auto focus_title_fg = resolve_color(toml, have_toml, "focus_title", Color::Green);

        c.selection   = Style{selection_fg,  Color::Default, Attribute::Bold};
        c.marked      = Style{marked_fg,     Color::Default, Attribute::None};
        c.artist      = Style{artist_fg,     Color::Default, Attribute::None};
        c.title       = Style{title_fg,      Color::Default, Attribute::None};
        c.album       = Style{album_fg,      Color::Default, Attribute::None};
        c.muted       = Style{muted_fg,      Color::Default, Attribute::None};
        c.border      = Style{border_fg,     Color::Default, Attribute::None};
        c.accent      = Style{accent_fg,     Color::Default, Attribute::Bold};
        c.separator   = Style{separator_fg,  Color::Default, Attribute::None};
        c.warning     = Style{warning_fg,    Color::Default, Attribute::None};
        c.heading     = Style{heading_fg,    Color::Default, Attribute::Bold};
        c.focus_title = Style{focus_title_fg, Color::Default, Attribute::Bold};

        return c;
    }();
    return uic;
}

}  // namespace ouroboros::config

#pragma once

#include "ui/Color.hpp"

namespace ouroboros::config {

// Semantic color roles for widget rendering.
// Each field is a resolved Style suitable for Canvas operations.
// Resolved once at startup from [roles] + [palette] in config.toml.
// Compiled defaults match the original hardcoded colors exactly.
struct UIConfig {
    ouroboros::ui::Style selection;    // Cursor/selected item
    ouroboros::ui::Style marked;       // Multi-selected items
    ouroboros::ui::Style artist;       // Artist name
    ouroboros::ui::Style title;        // Track/album title
    ouroboros::ui::Style album;        // Album name
    ouroboros::ui::Style muted;        // Secondary text, dim metadata
    ouroboros::ui::Style artwork_border; // Album art frame border
    ouroboros::ui::Style widget_border;  // Widget box-drawing characters
    ouroboros::ui::Style accent;       // Panel titles, headings
    ouroboros::ui::Style separator;    // Bullet separators
    ouroboros::ui::Style warning;      // Errors, missing files
    ouroboros::ui::Style heading;      // Help overlay headings
    ouroboros::ui::Style focus_title;  // Focused panel title
    ouroboros::ui::Style nowplaying_info; // NowPlaying format/statusline data
};

[[nodiscard]] const UIConfig& ui_config();

}  // namespace ouroboros::config

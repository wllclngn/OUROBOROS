#pragma once

#include <optional>

namespace ouroboros::ui {

/**
 * Size constraints for a widget
 * Defines minimum, maximum, and preferred dimensions
 */
struct SizeConstraints {
    std::optional<int> min_width;
    std::optional<int> max_width;
    std::optional<int> min_height;
    std::optional<int> max_height;

    std::optional<int> preferred_width;
    std::optional<int> preferred_height;
};

/**
 * Flexbox-style growth and shrink properties
 */
struct FlexProperties {
    float flex_grow = 0.0f;     // How much to grow when extra space available (0 = don't grow)
    float flex_shrink = 1.0f;   // How much to shrink when space is tight (0 = don't shrink)
    int priority = 0;           // Higher priority = allocated space first
};

/**
 * Combined layout constraints for a widget
 */
struct LayoutConstraints {
    SizeConstraints size;
    FlexProperties flex;

    // Convenience constructors
    static LayoutConstraints fixed(int width, int height) {
        LayoutConstraints c;
        c.size.min_width = width;
        c.size.max_width = width;
        c.size.min_height = height;
        c.size.max_height = height;
        c.flex.flex_grow = 0.0f;
        c.flex.flex_shrink = 0.0f;
        return c;
    }

    static LayoutConstraints flexible(int min_w, int max_w, float grow = 1.0f) {
        LayoutConstraints c;
        c.size.min_width = min_w;
        c.size.max_width = max_w;
        c.flex.flex_grow = grow;
        return c;
    }

    static LayoutConstraints fill(float grow = 1.0f) {
        LayoutConstraints c;
        c.flex.flex_grow = grow;
        return c;
    }
};

/**
 * Computed layout rectangle for a widget
 */
struct LayoutRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const LayoutRect& other) const {
        return x == other.x && y == other.y &&
               width == other.width && height == other.height;
    }

    bool operator!=(const LayoutRect& other) const {
        return !(*this == other);
    }
};

}  // namespace ouroboros::ui

#pragma once

#include "ui/LayoutConstraints.hpp"
#include <vector>
#include <memory>

namespace ouroboros::ui {

class Component;  // Forward declaration

/**
 * Flex layout direction
 */
enum class FlexDirection {
    Row,     // Horizontal layout (left to right)
    Column   // Vertical layout (top to bottom)
};

/**
 * Item in a flex layout with its constraints
 */
struct FlexItem {
    Component* component = nullptr;
    LayoutConstraints constraints;
    LayoutRect computed_rect;  // Result after layout computation
};

/**
 * Flexbox-inspired layout container
 *
 * Arranges child components according to flexbox-style constraints:
 * - Respects min/max size constraints
 * - Distributes space based on flex_grow and flex_shrink
 * - Supports row (horizontal) and column (vertical) directions
 * - Handles spacing between items
 */
class FlexLayout {
public:
    FlexLayout() = default;

    /**
     * Add a component to the layout with its constraints
     */
    void add_item(Component* comp, const LayoutConstraints& constraints);

    /**
     * Set layout direction (row or column)
     */
    void set_direction(FlexDirection dir) { direction_ = dir; }

    /**
     * Set spacing between items in pixels/columns
     */
    void set_spacing(int spacing) { spacing_ = spacing; }

    /**
     * Compute layout for all items given available space
     * Returns vector of computed rectangles (one per item)
     */
    std::vector<LayoutRect> compute_layout(int available_width, int available_height);

    /**
     * Clear all items from layout
     */
    void clear() { items_.clear(); }

    /**
     * Get number of items in layout
     */
    size_t item_count() const { return items_.size(); }

private:
    std::vector<FlexItem> items_;
    FlexDirection direction_ = FlexDirection::Row;
    int spacing_ = 0;

    /**
     * Compute sizes along the main axis (width for Row, height for Column)
     * Implements the flexbox sizing algorithm
     */
    std::vector<int> compute_main_axis_sizes(int available_space);

    /**
     * Compute sizes along the cross axis
     */
    std::vector<int> compute_cross_axis_sizes(int available_space);

    /**
     * Clamp a size to constraints (min/max)
     */
    int clamp_to_constraints(
        int size,
        const SizeConstraints& constraints,
        bool is_width
    ) const;

    /**
     * Get the minimum size from constraints
     */
    int get_min_size(const SizeConstraints& constraints, bool is_width) const;

    /**
     * Get the maximum size from constraints
     */
    int get_max_size(const SizeConstraints& constraints, bool is_width) const;
};

}  // namespace ouroboros::ui

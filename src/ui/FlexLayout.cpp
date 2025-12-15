#include "ui/FlexLayout.hpp"
#include "ui/Component.hpp"
#include <algorithm>
#include <limits>
#include <cmath>
#include <numeric>

namespace ouroboros::ui {

void FlexLayout::add_item(Component* comp, const LayoutConstraints& constraints) {
    FlexItem item;
    item.component = comp;
    item.constraints = constraints;
    items_.push_back(item);
}

std::vector<LayoutRect> FlexLayout::compute_layout(int available_width, int available_height) {
    if (items_.empty()) {
        return {};
    }

    // Compute sizes along main and cross axes
    std::vector<int> main_sizes = compute_main_axis_sizes(
        direction_ == FlexDirection::Row ? available_width : available_height
    );
    std::vector<int> cross_sizes = compute_cross_axis_sizes(
        direction_ == FlexDirection::Row ? available_height : available_width
    );

    // Position items and create rectangles
    std::vector<LayoutRect> result;
    result.reserve(items_.size());

    int main_pos = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        LayoutRect rect;

        if (direction_ == FlexDirection::Row) {
            rect.x = main_pos;
            rect.y = 0;
            rect.width = main_sizes[i];
            rect.height = cross_sizes[i];
            main_pos += main_sizes[i] + spacing_;
        } else {  // Column
            rect.x = 0;
            rect.y = main_pos;
            rect.width = cross_sizes[i];
            rect.height = main_sizes[i];
            main_pos += main_sizes[i] + spacing_;
        }

        result.push_back(rect);
    }

    return result;
}

std::vector<int> FlexLayout::compute_main_axis_sizes(int available_space) {
    size_t n = items_.size();
    std::vector<int> sizes(n, 0);
    std::vector<bool> frozen(n, false);
    
    bool is_width = (direction_ == FlexDirection::Row);

    // 1. Initialize with basis (preferred size or min size)
    int used_space = 0;
    for (size_t i = 0; i < n; ++i) {
        int basis = 0;
        if (is_width) {
            basis = items_[i].constraints.size.preferred_width.value_or(
                items_[i].constraints.size.min_width.value_or(0));
        } else {
             basis = items_[i].constraints.size.preferred_height.value_or(
                items_[i].constraints.size.min_height.value_or(0));
        }
        sizes[i] = basis;
        used_space += basis;
    }
    
    // Add spacing
    if (n > 1) {
        used_space += spacing_ * static_cast<int>(n - 1);
    }
    
    // Iterative distribution loop
    // We continue as long as we find constraint violations that require freezing items
    while (true) {
        int free_space = available_space - used_space;
        
        // If practically zero, we are done
        if (free_space == 0) break;
        
        // Calculate total flex for unfrozen items
        float total_flex = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            if (frozen[i]) continue;
            if (free_space > 0) {
                total_flex += items_[i].constraints.flex.flex_grow;
            } else {
                total_flex += items_[i].constraints.flex.flex_shrink;
            }
        }

        // If no flexibility left, we can't distribute further
        if (total_flex <= 0.0f) break;

        // Attempt to distribute
        bool violation_found = false;
        std::vector<int> proposed_sizes = sizes;
        
        // Use double for precision during distribution
        double remaining_distribution = static_cast<double>(free_space);
        
        for (size_t i = 0; i < n; ++i) {
            if (frozen[i]) continue;
            
            float flex = (free_space > 0) ? items_[i].constraints.flex.flex_grow 
                                          : items_[i].constraints.flex.flex_shrink;
            
            if (flex <= 0.0f) continue;
            
            double share = remaining_distribution * (flex / total_flex);
            int delta = static_cast<int>(std::round(share));
            
            // Adjust remaining for next items to minimize rounding error accumulation
            // (Note: simple approach here; usually works fine for UI)
            
            int tentative = sizes[i] + delta;
            
            int min_s = get_min_size(items_[i].constraints.size, is_width);
            int max_s = get_max_size(items_[i].constraints.size, is_width);
            
            // Check for violation
            if (tentative < min_s) {
                sizes[i] = min_s;
                frozen[i] = true;
                violation_found = true;
                break; // Restart loop
            } else if (tentative > max_s) {
                sizes[i] = max_s;
                frozen[i] = true;
                violation_found = true;
                break; // Restart loop
            }
            
            proposed_sizes[i] = tentative;
        }
        
        if (violation_found) {
            // Re-calculate used space with the frozen item's new fixed size
            used_space = 0;
            for (int s : sizes) used_space += s;
            if (n > 1) used_space += spacing_ * static_cast<int>(n - 1);
            continue; // Loop again
        } else {
            // No violations! Apply proposed sizes
            sizes = proposed_sizes;
            
            // Distribute any final rounding error to the last flexible, unfrozen item
            int final_used = 0;
            for (int s : sizes) final_used += s;
            if (n > 1) final_used += spacing_ * static_cast<int>(n - 1);
            
            int remainder = available_space - final_used;
            if (remainder != 0) {
                 for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
                     if (!frozen[i]) {
                         float flex = (free_space > 0) ? items_[i].constraints.flex.flex_grow 
                                                       : items_[i].constraints.flex.flex_shrink;
                         if (flex > 0) {
                             sizes[i] += remainder;
                             // Ensure we didn't break constraints with remainder (edge case)
                             int min_s = get_min_size(items_[i].constraints.size, is_width);
                             int max_s = get_max_size(items_[i].constraints.size, is_width);
                             sizes[i] = std::clamp(sizes[i], min_s, max_s);
                             break;
                         }
                     }
                 }
            }
            
            break; // Done
        }
    }

    return sizes;
}

std::vector<int> FlexLayout::compute_cross_axis_sizes(int available_space) {
    std::vector<int> sizes(items_.size(), 0);
    bool is_width = (direction_ == FlexDirection::Column);  // Opposite of main axis

    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& constraints = items_[i].constraints.size;

        // Start with available space (stretch)
        int size = available_space;

        // If preferred size is set, use it
        if (is_width && constraints.preferred_width) {
            size = *constraints.preferred_width;
        } else if (!is_width && constraints.preferred_height) {
            size = *constraints.preferred_height;
        }

        // Clamp to min/max
        size = clamp_to_constraints(size, constraints, is_width);

        sizes[i] = size;
    }

    return sizes;
}

int FlexLayout::clamp_to_constraints(
    int size,
    const SizeConstraints& constraints,
    bool is_width
) const {
    int min_size = get_min_size(constraints, is_width);
    int max_size = get_max_size(constraints, is_width);

    return std::clamp(size, min_size, max_size);
}

int FlexLayout::get_min_size(const SizeConstraints& constraints, bool is_width) const {
    if (is_width && constraints.min_width) {
        return *constraints.min_width;
    }
    if (!is_width && constraints.min_height) {
        return *constraints.min_height;
    }
    return 0;  // No minimum
}

int FlexLayout::get_max_size(const SizeConstraints& constraints, bool is_width) const {
    if (is_width && constraints.max_width) {
        return *constraints.max_width;
    }
    if (!is_width && constraints.max_height) {
        return *constraints.max_height;
    }
    return std::numeric_limits<int>::max();  // No maximum
}

}  // namespace ouroboros::ui
#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>
#include <set>
#include <memory>
#include <optional>

#include "sublimation_text.h"

namespace ouroboros::ui::widgets {

class Browser : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused);

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    // Sets the query and compiles the matcher for it. Out-of-line because the
    // compile belongs here: the query is the only thing that changes it.
    void set_filter(const std::string& query);

    // Multi-select support
    void toggle_selection(int index);
    void clear_selection() { selected_indices_.clear(); }
    bool is_selected(int index) const { return selected_indices_.count(index) > 0; }
    const std::set<int>& get_selected_indices() const { return selected_indices_; }

private:
    // Render loading animation when library is scanning
    void render_loading_indicator(Canvas& canvas, const LayoutRect& content_rect, const model::Snapshot& snap);

    // Filter logic
    void update_filtered_indices(const model::Snapshot& snap);

    int selected_index_ = 0;
    int scroll_offset_ = 0;
    
    std::string filter_query_;
    // The compiled query, literal face with ASCII case folding. Compiled once in
    // set_filter and matched against every track field; empty when there is no
    // filter. Held by value -- the program is a value object that never heap-
    // allocates, and its find calls are reentrant over a shared const program.
    std::optional<sublimation_search> matcher_;
    bool filter_dirty_ = true;
    size_t last_library_size_ = 0; // To detect library updates
    
    std::vector<int> filtered_indices_; // Indices into snap.library.tracks
    // Per-chunk match sinks for the parallel filter, one vector per worker.
    // Held across keystrokes so their capacity is reused rather than
    // reallocated on every query change.
    std::vector<std::vector<int>> filter_chunks_;
    std::set<int> selected_indices_;    // Multi-select: tracks selected for batch operations
};

}  // namespace ouroboros::ui::widgets

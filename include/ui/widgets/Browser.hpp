#pragma once

#include "ui/Component.hpp"
#include "ui/widgets/SearchBox.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>
#include <set>
#include <memory>

namespace ouroboros::ui::widgets {

class Browser : public Component {
public:
    Browser(); // Need constructor to init SearchBox

    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused);

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    void set_filter(const std::string& query) {
        if (filter_query_ != query) {
            filter_query_ = query;
            filter_dirty_ = true;
            selected_index_ = 0;
            scroll_offset_ = 0;
        }
    }

    void start_search() {
        state_ = State::Searching;
    }

    bool is_searching() const { return state_ == State::Searching; }

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

    enum class State {
        Browsing,
        Searching
    };
    State state_ = State::Browsing;

    std::unique_ptr<SearchBox> search_box_;

    int selected_index_ = 0;
    int scroll_offset_ = 0;
    
    std::string filter_query_;
    bool filter_dirty_ = true;
    size_t last_library_size_ = 0; // To detect library updates
    
    std::vector<int> filtered_indices_; // Indices into snap.library.tracks
    std::set<int> selected_indices_;    // Multi-select: tracks selected for batch operations
};

}  // namespace ouroboros::ui::widgets

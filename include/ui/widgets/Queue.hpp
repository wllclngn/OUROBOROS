#pragma once

#include "ui/Component.hpp"
#include <optional>
#include <string>
#include <vector>

#include "sublimation_text.h"
// See Browser.hpp: c23_compat.h's bare `unreachable()` macro collides with
// libstdc++'s std::unreachable declaration. Drop it at the header boundary.
#undef unreachable

namespace ouroboros::ui::widgets {

class Queue : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused);

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    // Narrow the queue to tracks matching `query` (artist, album or title).
    // Empty clears the filter. Same compiled-matcher model as Browser.
    void set_filter(const std::string& query);

private:
    int scroll_offset_ = 0;
    int cursor_index_ = 0;   // Cursor row in the RENDERED list (filtered when a filter is set)
    int total_items_ = 0;    // Rendered-list size from last render (clamps cursor in handle_input)

    std::string filter_query_;
    std::optional<sublimation_search> matcher_;

    // Rendered row -> index into the UNFILTERED display list. JumpToQueueIndex
    // addresses the unfiltered list, so the cursor must be translated before it
    // is published or a filtered Enter jumps to the wrong track.
    std::vector<int> row_to_display_;
};

}  // namespace ouroboros::ui::widgets

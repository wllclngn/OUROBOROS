#pragma once

#include "ui/Component.hpp"

namespace ouroboros::ui::widgets {

class Queue : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused);

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

private:
    int scroll_offset_ = 0;
    int cursor_index_ = 0;   // Cursor row in queue display order (Browser-style movement)
    int total_items_ = 0;    // Display-list size from last render (clamps cursor in handle_input)
};

}  // namespace ouroboros::ui::widgets

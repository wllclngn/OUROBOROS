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
};

}  // namespace ouroboros::ui::widgets

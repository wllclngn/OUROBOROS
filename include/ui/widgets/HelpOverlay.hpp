#pragma once

#include "ui/Component.hpp"

namespace ouroboros::ui::widgets {

class HelpOverlay : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    bool is_visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

private:
    bool visible_ = false;
};

}  // namespace ouroboros::ui::widgets

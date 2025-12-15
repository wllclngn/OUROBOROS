#pragma once

#include "ui/Component.hpp"

namespace ouroboros::ui::widgets {

class StatusBar : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;
};

}  // namespace ouroboros::ui::widgets

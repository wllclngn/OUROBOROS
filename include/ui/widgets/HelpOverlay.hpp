#pragma once

#include "ui/Component.hpp"
#include <string>
#include <vector>

namespace ouroboros::ui::widgets {

class HelpOverlay : public Component {
public:
    HelpOverlay();

    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void handle_input(const InputEvent& event) override;
    SizeConstraints get_constraints() const override;

    bool is_visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; if (v) scroll_offset_ = 0; }

private:
    struct Line {
        enum Type { Blank, Heading, SubHeading, KeyValue, Text, Divider };
        Type type = Text;
        std::string left;   // key or text
        std::string right;  // value (for KeyValue lines)
    };

    void build_content();

    bool visible_ = false;
    int scroll_offset_ = 0;
    std::vector<Line> lines_;
};

}  // namespace ouroboros::ui::widgets

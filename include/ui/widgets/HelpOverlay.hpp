#pragma once

#include "ui/Component.hpp"
#include <string>
#include <vector>

namespace ouroboros::ui::widgets {

// The help view is the manpage. `ouroboros.1` is the single source of truth for
// keybindings, configuration, environment and troubleshooting; this renders it
// rather than restating it, so the two cannot drift.
class HelpOverlay : public Component {
public:
    HelpOverlay();

    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void handle_input(const InputEvent& event) override;
    SizeConstraints get_constraints() const override;

    bool is_visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; if (v) scroll_offset_ = 0; }

private:
    // Format the manpage through `man -P cat` and keep the plain text. `col -b`
    // strips the overstrike bold man emits for a pager. Tries the installed page
    // first, then `ouroboros.1` beside the source tree so a dev build works
    // before `install.py` has put the page anywhere man can find it.
    void load_from_man();
    bool read_lines(const std::string& command);

    bool visible_ = false;
    int scroll_offset_ = 0;
    std::vector<std::string> lines_;
};

}  // namespace ouroboros::ui::widgets

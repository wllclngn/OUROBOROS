#include "ui/widgets/SearchBox.hpp"
#include "ui/Formatting.hpp"
#include "config/Theme.hpp"
#include "util/BoyerMoore.hpp"
#include "util/Logger.hpp"

namespace ouroboros::ui::widgets {

void SearchBox::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    (void)snap;

    if (!visible_) return;

    auto theme = config::ThemeManager::get_theme("terminal");

    // Draw search box border
    auto content_rect = draw_box_border(canvas, rect, "FIND");

    // Draw search prompt and query
    std::string prompt = "Search: ";
    std::string content = prompt + query_;

    // Add cursor at end
    if (cursor_pos_ == (int)query_.size()) {
        content += "█";
    }

    canvas.draw_text(content_rect.x, content_rect.y, content,
                    Style{Color::BrightYellow, Color::Default, Attribute::Bold});

    // Instructions
    std::string instructions = "Enter to search | Esc to cancel";
    canvas.draw_text(content_rect.x, content_rect.y + 1, instructions,
                    Style{Color::BrightBlack, Color::Default, Attribute::Dim});
}

void SearchBox::handle_input(const InputEvent& event) {
    handle_search_input(event);
}

SearchBox::Result SearchBox::handle_search_input(const InputEvent& event) {
    if (!visible_) return Result::None;

    if (event.key_name == "enter" || event.key == '\n') {
        util::Logger::info("SearchBox: Submitting query: '" + query_ + "'");
        // No longer publishing event here - parent handles it
        visible_ = false;
        return Result::Submit;
    }

    if (event.key_name == "escape" || event.key == 27) {
        util::Logger::debug("SearchBox: Cancelled");
        visible_ = false;
        return Result::Cancel;
    }

    if (event.key_name == "backspace" || event.key == 127) {
        if (!query_.empty()) {
            query_.pop_back();
            cursor_pos_--;
        }
        return Result::None;
    }

    if (event.type == InputEvent::Type::KeyPress && !event.key_name.empty() && event.key_name.length() == 1) {
        char c = event.key_name[0];
        if (std::isprint(c)) {
            query_ += c;
            cursor_pos_++;
        }
    }
    
    return Result::None;
}

void SearchBox::set_visible(bool v) {
    if (visible_ != v) {
        util::Logger::debug("SearchBox: Visibility changed to " + std::string(v ? "true" : "false"));
        visible_ = v;
    }
}

void SearchBox::clear() {
    query_ = "";
    cursor_pos_ = 0;
}

SizeConstraints SearchBox::get_constraints() const {
    // SearchBox is a fixed-height overlay (input box + border)
    SizeConstraints constraints;
    constraints.min_height = 3;
    constraints.max_height = 3;
    return constraints;
}

}  // namespace ouroboros::ui::widgets

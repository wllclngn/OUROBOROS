#include "ui/widgets/HelpOverlay.hpp"

namespace ouroboros::ui::widgets {

void HelpOverlay::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    (void)snap;  // Unused

    if (!visible_) return;

    // Center the help box
    int box_width = 70;
    int box_height = 22;
    int box_x = (rect.width - box_width) / 2;
    int box_y = (rect.height - box_height) / 2;

    // Ensure it fits on screen
    if (box_x < 0) box_x = 0;
    if (box_y < 0) box_y = 0;
    if (box_x + box_width > rect.width) box_width = rect.width - box_x;
    if (box_y + box_height > rect.height) box_height = rect.height - box_y;

    LayoutRect help_rect{box_x, box_y, box_width, box_height};

    // Draw semi-transparent background (use darker color)
    for (int y = help_rect.y; y < help_rect.y + help_rect.height; ++y) {
        for (int x = help_rect.x; x < help_rect.x + help_rect.width; ++x) {
            canvas.draw_text(x, y, " ", Style{Color::Default, Color::Black, Attribute::None});
        }
    }

    // Draw border with title
    auto content_rect = draw_box_border(canvas, help_rect, "HELP");

    // Draw help content
    int y = content_rect.y;

    Style heading_style{Color::BrightYellow, Color::Black, Attribute::Bold};
    Style text_style{Color::BrightWhite, Color::Black, Attribute::None};
    Style key_style{Color::BrightCyan, Color::Black, Attribute::Bold};

    canvas.draw_text(content_rect.x + 2, y++, "OUROBOROS MUSIC PLAYER", heading_style);
    y++;  // Blank line

    canvas.draw_text(content_rect.x + 2, y++, "Navigation:", heading_style);
    canvas.draw_text(content_rect.x + 4, y++, "j/k or ↑/↓    Navigate up/down", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "h/l or ←/→    Navigate left/right (album grid)", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "Tab           Switch focus (Browser/Queue)", text_style);
    y++;  // Blank line

    canvas.draw_text(content_rect.x + 2, y++, "Playback:", heading_style);
    canvas.draw_text(content_rect.x + 4, y++, "Space         Play/Pause", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "n             Next track", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "p             Previous track", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "←/→           Seek backward/forward 5s", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "+/-           Volume up/down", text_style);
    y++;  // Blank line

    canvas.draw_text(content_rect.x + 2, y++, "Library:", heading_style);
    canvas.draw_text(content_rect.x + 4, y++, "Enter         Add track/album to queue", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "Shift+J/K     Multi-select tracks", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "Ctrl+a        Toggle album grid view", text_style);
    canvas.draw_text(content_rect.x + 4, y++, "Ctrl+f        Search/Find (not yet implemented)", text_style);
    y++;  // Blank line

    canvas.draw_text(content_rect.x + 2, y++, "?             Show this help", key_style);
    canvas.draw_text(content_rect.x + 2, y++, "q             Quit", key_style);
}

void HelpOverlay::handle_input(const InputEvent&) {
    // Help overlay doesn't handle input directly
    // It's toggled by pressing '?' in Renderer
}

SizeConstraints HelpOverlay::get_constraints() const {
    // Help overlay is a fullscreen overlay, flexible size
    SizeConstraints constraints;
    return constraints;  // No constraints, fills screen
}

}  // namespace ouroboros::ui::widgets

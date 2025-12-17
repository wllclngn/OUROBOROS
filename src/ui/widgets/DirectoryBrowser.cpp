#include "ui/widgets/DirectoryBrowser.hpp"
#include "ui/Formatting.hpp"
#include "config/Theme.hpp"
#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace ouroboros::ui::widgets {

DirectoryBrowser::DirectoryBrowser() {
    util::Logger::debug("DirectoryBrowser: Initialized");
}

void DirectoryBrowser::set_directories(const std::vector<backend::DirectoryMetadata>& directories) {
    directories_ = directories;
    needs_refresh_ = true;

    // Reset selection if out of bounds
    if (selected_index_ >= static_cast<int>(directories_.size())) {
        selected_index_ = 0;
    }

    util::Logger::info("DirectoryBrowser: Loaded " + std::to_string(directories_.size()) + " directories");
}

std::optional<std::string> DirectoryBrowser::get_selected_directory() const {
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(directories_.size())) {
        return directories_[selected_index_].path;
    }
    return std::nullopt;
}

void DirectoryBrowser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    render(canvas, rect, snap, false);
}

void DirectoryBrowser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused) {
    (void)snap; // Unused for now

    // Draw border
    Style border_style;
    if (is_focused) {
        border_style.fg = Color::Cyan;
        border_style.attr = Attribute::Bold;
    } else {
        border_style.fg = Color::White;
    }

    canvas.draw_rect(rect.x, rect.y, rect.width, rect.height, border_style);

    // Draw title
    std::string title = " DIRECTORIES ";
    int title_x = rect.x + 2;
    canvas.draw_text(title_x, rect.y, title, border_style);

    // Adjust content rect for border
    LayoutRect content_rect;
    content_rect.x = rect.x + 1;
    content_rect.y = rect.y + 1;
    content_rect.width = rect.width - 2;
    content_rect.height = rect.height - 2;

    // Render directory list
    render_directory_list(canvas, content_rect, is_focused);
}

void DirectoryBrowser::render_directory_list(Canvas& canvas, const LayoutRect& content_rect, bool is_focused) {
    if (directories_.empty()) {
        // Show "No directories" message
        std::string msg = "No directories found";
        int msg_x = content_rect.x + (content_rect.width - static_cast<int>(msg.size())) / 2;
        int msg_y = content_rect.y + content_rect.height / 2;

        Style msg_style;
        msg_style.fg = Color::White;
        msg_style.attr = Attribute::Dim;
        canvas.draw_text(msg_x, msg_y, msg, msg_style);
        return;
    }

    // Calculate visible range
    int visible_lines = content_rect.height;
    int total_dirs = directories_.size();

    // Adjust scroll to keep selection visible
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    }
    if (selected_index_ >= scroll_offset_ + visible_lines) {
        scroll_offset_ = selected_index_ - visible_lines + 1;
    }

    int start_idx = scroll_offset_;
    int end_idx = std::min(scroll_offset_ + visible_lines, total_dirs);

    // Render directories
    for (int i = start_idx; i < end_idx; ++i) {
        int y = content_rect.y + (i - scroll_offset_);
        const auto& dir = directories_[i];

        bool is_selected = (i == selected_index_);

        // Format directory entry
        std::ostringstream line;
        if (is_selected) {
            line << "> ";
        } else {
            line << "  ";
        }

        // Directory name
        line << dir.path;

        // Track count
        line << " (" << dir.track_count << " tracks)";

        // Determine style
        Style line_style;
        if (is_selected && is_focused) {
            line_style.fg = Color::Cyan;
            line_style.attr = Attribute::Bold;
        } else if (is_selected) {
            line_style.fg = Color::White;
            line_style.attr = Attribute::Bold;
        } else {
            line_style.fg = Color::White;
        }

        // Truncate if too long
        std::string line_str = line.str();
        if (static_cast<int>(line_str.size()) > content_rect.width) {
            line_str = line_str.substr(0, content_rect.width - 3) + "...";
        }

        canvas.draw_text(content_rect.x, y, line_str, line_style);
    }

    // Render scrollbar if needed
    if (total_dirs > visible_lines) {
        int scrollbar_x = content_rect.x + content_rect.width - 1;
        int scrollbar_height = visible_lines;
        int thumb_size = std::max(1, (visible_lines * visible_lines) / total_dirs);
        int thumb_pos = (scroll_offset_ * scrollbar_height) / total_dirs;

        Style scrollbar_style;
        scrollbar_style.fg = Color::White;

        Style thumb_style;
        thumb_style.fg = Color::Cyan;
        thumb_style.attr = Attribute::Bold;

        for (int y = 0; y < scrollbar_height; ++y) {
            if (y >= thumb_pos && y < thumb_pos + thumb_size) {
                canvas.draw_text(scrollbar_x, content_rect.y + y, "\u2588", thumb_style); // Full block
            } else {
                canvas.draw_text(scrollbar_x, content_rect.y + y, "\u2502", scrollbar_style); // Vertical line
            }
        }
    }
}

void DirectoryBrowser::handle_input(const InputEvent& event) {
    if (directories_.empty()) {
        return;
    }

    int total_dirs = directories_.size();

    if (event.key == 'j' || event.key_name == "down") {
        // Move down
        selected_index_ = std::min(selected_index_ + 1, total_dirs - 1);
        util::Logger::debug("DirectoryBrowser: Selected index " + std::to_string(selected_index_));
    } else if (event.key == 'k' || event.key_name == "up") {
        // Move up
        selected_index_ = std::max(selected_index_ - 1, 0);
        util::Logger::debug("DirectoryBrowser: Selected index " + std::to_string(selected_index_));
    } else if (event.key == 'g') {
        // Go to top
        selected_index_ = 0;
        scroll_offset_ = 0;
        util::Logger::debug("DirectoryBrowser: Jump to top");
    } else if (event.key == 'G') {
        // Go to bottom
        selected_index_ = total_dirs - 1;
        util::Logger::debug("DirectoryBrowser: Jump to bottom");
    } else if (event.key_name == "enter" || event.key == '\n' || event.key == '\r' || event.key == ' ') {
        // Select directory and emit event
        auto selected = get_selected_directory();
        if (selected) {
            util::Logger::info("DirectoryBrowser: Selected directory: " + *selected);

            // TODO: Emit directory selection event when event system supports it
            // For now, this is a placeholder for future directory navigation
        }
    }
}

SizeConstraints DirectoryBrowser::get_constraints() const {
    SizeConstraints constraints;
    constraints.min_width = 30;
    constraints.min_height = 5;
    constraints.preferred_width = 50;
    constraints.preferred_height = 20;
    return constraints;
}

}  // namespace ouroboros::ui::widgets

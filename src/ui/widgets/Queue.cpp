#include "ui/widgets/Queue.hpp"
#include "ui/Formatting.hpp"
#include "ui/VisualBlocks.hpp"
#include "ui/InputEvent.hpp"
#include "config/Theme.hpp"
#include <algorithm>
#include <sstream>
#include "util/Logger.hpp"

namespace ouroboros::ui::widgets {

void Queue::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    render(canvas, rect, snap, false);
}

void Queue::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused) {
    auto theme = config::ThemeManager::get_theme("terminal");

    // Defensive: Check queue exists
    if (!snap.queue) {
        ouroboros::util::Logger::error("Queue::render: snap.queue is null!");
        draw_box_border(canvas, rect, "QUEUE [ERROR]");
        return;
    }

    // Two Stacks: Build display list (history + current + future)
    // Display in ADD order: history first, then current, then future
    std::vector<std::pair<int, bool>> display_tracks; // (track_index, is_current)

    // History (played tracks, oldest first)
    for (int idx : snap.queue->history) {
        display_tracks.emplace_back(idx, false);
    }
    // Current track
    if (snap.queue->current.has_value()) {
        display_tracks.emplace_back(*snap.queue->current, true);
    }
    // Future (upcoming tracks, in add order - front is next)
    for (int idx : snap.queue->future) {
        display_tracks.emplace_back(idx, false);
    }

    // Draw border and title (highlight when focused)
    std::string title = "QUEUE [" + std::to_string(display_tracks.size()) + " TRACKS]";
    auto content_rect = draw_box_border(canvas, rect, title, Style{}, is_focused);

    // Empty queue
    if (display_tracks.empty()) {
        return;  // Keep UI clean - no placeholder text
    }

    // Defensive: Check library exists
    if (!snap.library) {
        ouroboros::util::Logger::error("Queue::render: snap.library is null!");
        return;
    }

    // Bounds checking for scroll
    if (scroll_offset_ >= static_cast<int>(display_tracks.size())) {
        scroll_offset_ = std::max(0, static_cast<int>(display_tracks.size()) - 1);
    }
    if (scroll_offset_ < 0) {
        scroll_offset_ = 0;
    }

    // Render visible tracks
    int y = content_rect.y;
    int available_lines = content_rect.height;

    int end_index = std::min(static_cast<int>(display_tracks.size()), scroll_offset_ + available_lines);

    for (int i = scroll_offset_; i < end_index; ++i) {
        const auto& [track_idx, is_current] = display_tracks[i];

        // Bounds check
        if (track_idx < 0 || track_idx >= static_cast<int>(snap.library->tracks.size())) {
            ouroboros::util::Logger::error("Queue::render: Invalid track_idx=" + std::to_string(track_idx));
            continue;
        }
        const auto& track = snap.library->tracks[track_idx];

        // Match Browser formatting: Artist Album: TrackNum Title
        if (is_current) {
            // Current track: single-color highlight (BrightYellow + Bold)
            std::string prefix = "▶ ";
            std::ostringstream oss;
            oss << prefix;

            // Artist
            if (!track.artist.empty()) {
                oss << track.artist;
            } else {
                oss << "Unknown Artist";
            }

            // Album
            if (!track.album.empty()) {
                oss << " " << track.album;
            }

            oss << ": ";

            // Track number
            if (track.track_number > 0) {
                oss << std::setfill('0') << std::setw(2) << track.track_number << " ";
            }

            // Title
            if (!track.title.empty()) {
                oss << track.title;
            } else {
                oss << "Untitled";
            }

            std::string line = oss.str();
            Style style = Style{Color::BrightYellow, Color::Default, Attribute::Bold};
            canvas.draw_text(content_rect.x, y++, truncate_text(line, content_rect.width), style);
        } else {
            // Normal track: multi-color rendering (matches Browser)
            int x = content_rect.x;
            int line_y = y++;
            int remaining_w = content_rect.width;

            // Helper to draw and advance
            auto draw_part = [&](const std::string& text, Style s) {
                if (remaining_w <= 0) return;
                std::string t = truncate_text(text, remaining_w);
                canvas.draw_text(x, line_y, t, s);
                int len = display_cols(t);
                x += len;
                remaining_w -= len;
            };

            // Prefix
            draw_part("  ", Style{});

            // Artist (Cyan)
            draw_part(!track.artist.empty() ? track.artist : "Unknown Artist",
                     Style{Color::Cyan, Color::Default, Attribute::None});

            // Album (Default)
            if (!track.album.empty()) {
                draw_part(" " + track.album, Style{Color::Default, Color::Default, Attribute::None});
            }

            // Separator
            draw_part(": ", Style{});

            // Track number (Dim)
            if (track.track_number > 0) {
                std::ostringstream num_oss;
                num_oss << std::setfill('0') << std::setw(2) << track.track_number << " ";
                draw_part(num_oss.str(), Style{Color::Default, Color::Default, Attribute::Dim});
            }

            // Title (BrightWhite)
            draw_part(!track.title.empty() ? track.title : "Untitled",
                     Style{Color::BrightWhite, Color::Default, Attribute::None});
        }

        if (y >= content_rect.y + content_rect.height) break;
    }
}

void Queue::handle_input(const InputEvent& event) {
    // Navigation (from TOML: nav_up, nav_down)
    if (matches_keybind(event, "nav_up")) {
        if (scroll_offset_ > 0) scroll_offset_--;
    }
    else if (matches_keybind(event, "nav_down")) {
        scroll_offset_++;
    }
}

SizeConstraints Queue::get_constraints() const {
    // Queue is flexible - should show at least 5 tracks
    SizeConstraints constraints;
    constraints.min_height = 7;  // 5 tracks + 2 for border
    return constraints;
}

}  // namespace ouroboros::ui::widgets

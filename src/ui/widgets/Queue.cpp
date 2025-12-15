#include "ui/widgets/Queue.hpp"
#include "ui/Formatting.hpp"
#include "ui/VisualBlocks.hpp"
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

    const auto& track_indices = snap.queue->track_indices;

    // Draw border and title (highlight when focused)
    std::string title = "QUEUE [" + std::to_string(track_indices.size()) + " TRACKS]";
    auto content_rect = draw_box_border(canvas, rect, title, Style{}, is_focused);

    // Empty queue
    if (track_indices.empty()) {
        ouroboros::util::Logger::debug("Queue: Queue is empty");
        return;  // Keep UI clean - no placeholder text
    }

    // Defensive: Check library exists
    if (!snap.library) {
        ouroboros::util::Logger::error("Queue::render: snap.library is null!");
        return;
    }

    // Bounds checking for scroll
    if (scroll_offset_ >= static_cast<int>(track_indices.size())) {
        scroll_offset_ = std::max(0, static_cast<int>(track_indices.size()) - 1);
    }
    if (scroll_offset_ < 0) {
        scroll_offset_ = 0;
    }

    // Render visible tracks
    int y = content_rect.y;
    int available_lines = content_rect.height;

    int end_index = std::min(static_cast<int>(track_indices.size()), scroll_offset_ + available_lines);

    for (int i = scroll_offset_; i < end_index; ++i) {
        // Resolve track index to actual Track via Library
        int track_idx = track_indices[i];
        if (track_idx < 0 || track_idx >= static_cast<int>(snap.library->tracks.size())) {
            ouroboros::util::Logger::error("Queue::render: Invalid track_idx=" + std::to_string(track_idx) +
                " at queue position " + std::to_string(i) + ", library size=" +
                std::to_string(snap.library->tracks.size()));
            continue;  // Skip invalid indices
        }
        const auto& track = snap.library->tracks[track_idx];
        bool is_current = (static_cast<size_t>(i) == snap.queue->current_index);

        // Build track line: Artist - Album - Title [duration]
        std::string prefix = is_current ? "▶ " : "  ";
        std::ostringstream oss;
        oss << prefix;

        if (!track.artist.empty()) {
            oss << track.artist << " - ";
        }

        if (!track.album.empty()) {
            oss << track.album << " - ";
        }

        oss << track.title;

        if (track.duration_ms > 0) {
            oss << " [" << format_duration(track.duration_ms / 1000) << "]";
        }

        std::string line = oss.str();

        // Determine style
        Style style;
        if (is_current) {
            style = Style{Color::BrightYellow, Color::Default, Attribute::Bold};
        } else {
            style = Style{Color::Default, Color::Default, Attribute::None};
        }

        canvas.draw_text(content_rect.x, y++, truncate_text(line, content_rect.width), style);

        if (y >= content_rect.y + content_rect.height) break;
    }
}

void Queue::handle_input(const InputEvent& event) {
    if (event.key_name == "up" || event.key == 'k') {
        if (scroll_offset_ > 0) scroll_offset_--;
    }
    else if (event.key_name == "down" || event.key == 'j') {
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

#include "ui/widgets/Queue.hpp"
#include "ui/Formatting.hpp"
#include "ui/VisualBlocks.hpp"
#include "ui/InputEvent.hpp"
#include "config/UIConfig.hpp"
#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include "util/Platform.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>

namespace ouroboros::ui::widgets {

void Queue::set_filter(const std::string& query) {
    if (filter_query_ == query) return;
    filter_query_ = query;
    cursor_index_ = 0;
    scroll_offset_ = 0;

    // Compiled once per query, matched against every queued track. Literal face
    // with ASCII case folding, the same shape Browser uses.
    matcher_.reset();
    if (!filter_query_.empty()) {
        matcher_.emplace();
        sublimation_search_compile(&*matcher_, filter_query_.data(), filter_query_.size(),
                                   SUBLIMATION_SEARCH_FIXED | SUBLIMATION_SEARCH_ICASE, 0);
        if (!sublimation_search_valid(&*matcher_)) matcher_.reset();
    }
}

void Queue::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    render(canvas, rect, snap, false);
}

void Queue::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused) {
    const auto& uc = config::ui_config();

    // Defensive: Check queue exists
    if (!snap.queue) {
        ouroboros::util::Logger::error("Queue::render: snap.queue is null!");
        draw_box_border(canvas, rect, "QUEUE: ERROR");
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

    // Defensive: Check library exists (needed before filtering, which reads tracks)
    if (!snap.library) {
        ouroboros::util::Logger::error("Queue::render: snap.library is null!");
        draw_box_border(canvas, rect, "QUEUE: ERROR");
        return;
    }

    // Filter, keeping the map back to the unfiltered list. JumpToQueueIndex
    // addresses the unfiltered display order, never these rows.
    const size_t queued_total = display_tracks.size();
    row_to_display_.clear();
    if (matcher_) {
        const sublimation_search& m = *matcher_;
        std::vector<std::pair<int, bool>> kept;
        kept.reserve(display_tracks.size());
        for (size_t i = 0; i < display_tracks.size(); ++i) {
            const int ti = display_tracks[i].first;
            if (ti < 0 || ti >= util::narrow_cast<int>(snap.library->tracks.size())) continue;
            const auto& t = snap.library->tracks[ti];
            if (sublimation_search_find(&m, t.artist.data(), t.artist.size(), nullptr) != -1 ||
                sublimation_search_find(&m, t.album.data(),  t.album.size(),  nullptr) != -1 ||
                sublimation_search_find(&m, t.title.data(),  t.title.size(),  nullptr) != -1) {
                kept.push_back(display_tracks[i]);
                row_to_display_.push_back(util::narrow_cast<int>(i));
            }
        }
        display_tracks.swap(kept);
    } else {
        row_to_display_.resize(display_tracks.size());
        std::iota(row_to_display_.begin(), row_to_display_.end(), 0);
    }

    // Draw border and title (highlight when focused)
    std::string title = "QUEUE: ";
    if (matcher_) {
        title += "SEARCH \"" + filter_query_ + "\", " + std::to_string(display_tracks.size())
               + "/" + std::to_string(queued_total) + " TRACKS";
    } else {
        title += std::to_string(display_tracks.size()) + " TRACKS";
    }
    auto content_rect = draw_box_border(canvas, rect, title, is_focused);

    // Empty queue, or a filter that matched nothing
    if (display_tracks.empty()) {
        return;  // Keep UI clean - no placeholder text
    }

    // Clamp cursor to the display list and scroll-follow (Browser pattern)
    total_items_ = util::narrow_cast<int>(display_tracks.size());
    if (cursor_index_ >= total_items_) cursor_index_ = total_items_ - 1;
    if (cursor_index_ < 0) cursor_index_ = 0;

    int available_lines = content_rect.height;
    if (cursor_index_ < scroll_offset_) {
        scroll_offset_ = cursor_index_;
    } else if (cursor_index_ >= scroll_offset_ + available_lines) {
        scroll_offset_ = cursor_index_ - available_lines + 1;
    }
    if (scroll_offset_ > total_items_ - available_lines) {
        scroll_offset_ = std::max(0, total_items_ - available_lines);
    }
    if (scroll_offset_ < 0) scroll_offset_ = 0;

    // Render visible tracks
    int y = content_rect.y;

    int end_index = std::min(total_items_, scroll_offset_ + available_lines);

    for (int i = scroll_offset_; i < end_index; ++i) {
        const auto& [track_idx, is_current] = display_tracks[i];
        bool is_cursor = (is_focused && i == cursor_index_);

        // Bounds check
        if (track_idx < 0 || track_idx >= util::narrow_cast<int>(snap.library->tracks.size())) {
            ouroboros::util::Logger::error("Queue::render: Invalid track_idx=" + std::to_string(track_idx));
            continue;
        }
        const auto& track = snap.library->tracks[track_idx];

        // Match Browser formatting: Artist Album: TrackNum Title
        if (is_current || is_cursor) {
            // Current track and cursor row: single-color highlight
            std::string prefix = is_current ? "▶ " : "  ";
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
            Style style = uc.selection;
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
            draw_part("  ", uc.muted);

            // Artist (Cyan)
            draw_part(!track.artist.empty() ? track.artist : "Unknown Artist",
                     uc.artist);

            // Album (Default)
            if (!track.album.empty()) {
                draw_part(" " + track.album, uc.album);
            }

            // Separator
            draw_part(": ", uc.separator);

            // Track number (Dim)
            if (track.track_number > 0) {
                std::ostringstream num_oss;
                num_oss << std::setfill('0') << std::setw(2) << track.track_number << " ";
                draw_part(num_oss.str(), uc.muted);
            }

            // Title (BrightWhite)
            draw_part(!track.title.empty() ? track.title : "Untitled",
                     uc.title);
        }

        if (y >= content_rect.y + content_rect.height) break;
    }
}

void Queue::handle_input(const InputEvent& event) {
    // Escape clears an active filter before anything else looks at the key.
    if ((event.key_name == "escape" || event.key == 27) && !filter_query_.empty()) {
        set_filter("");
        return;
    }
    // Browser-style cursor movement (from TOML: nav_up, nav_down);
    // scroll follows the cursor in render()
    if (matches_keybind(event, "nav_up")) {
        if (cursor_index_ > 0) cursor_index_--;
    }
    else if (matches_keybind(event, "nav_down")) {
        cursor_index_++;
        if (cursor_index_ >= total_items_) cursor_index_ = std::max(0, total_items_ - 1);
    }
    // Jump playback to the track under the cursor (from TOML: select)
    else if (matches_keybind(event, "select")) {
        if (total_items_ <= 0) return;
        if (cursor_index_ < 0 || cursor_index_ >= util::narrow_cast<int>(row_to_display_.size())) return;
        events::Event evt;
        evt.type = events::Event::Type::JumpToQueueIndex;
        evt.index = row_to_display_[cursor_index_];   // unfiltered display index
        events::EventBus::instance().publish(evt);
    }
}

SizeConstraints Queue::get_constraints() const {
    // Queue is flexible - should show at least 5 tracks
    SizeConstraints constraints;
    constraints.min_height = 7;  // 5 tracks + 2 for border
    return constraints;
}

}  // namespace ouroboros::ui::widgets

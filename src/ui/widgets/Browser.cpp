#include "ui/widgets/Browser.hpp"
#include "ui/Formatting.hpp"
#include "ui/InputEvent.hpp"
#include "config/Theme.hpp"
#include "events/EventBus.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <ctime>
#include "util/Logger.hpp"
#include "util/BoyerMoore.hpp"

namespace ouroboros::ui::widgets {

// Global SHARED POINTER to current snapshot for event handling
// This prevents dangling pointer issues when snap reference goes out of scope
static std::shared_ptr<const model::Snapshot> g_current_snapshot = nullptr;

void Browser::update_filtered_indices(const model::Snapshot& snap) {
    const auto& tracks = snap.library->tracks;
    filtered_indices_.clear();
    filtered_indices_.reserve(tracks.size());

    // Case 1: No filter -> All tracks
    if (filter_query_.empty()) {
        for (size_t i = 0; i < tracks.size(); ++i) {
            filtered_indices_.push_back(i);
        }
        filter_dirty_ = false;
        last_library_size_ = tracks.size();
        return;
    }

    // Case 2: Filter active -> Search
    ouroboros::util::BoyerMooreSearch searcher(filter_query_, false); // Case-insensitive
    
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        bool match = false;
        
        // Search in Artist, Album, Title
        if (searcher.search(t.artist) != -1) match = true;
        else if (searcher.search(t.album) != -1) match = true;
        else if (searcher.search(t.title) != -1) match = true;
        
        if (match) {
            filtered_indices_.push_back(i);
        }
    }

    filter_dirty_ = false;
    last_library_size_ = tracks.size();
    
    // Log filter results
    ouroboros::util::Logger::debug("Browser: Filtered " + std::to_string(tracks.size()) + 
                                  " -> " + std::to_string(filtered_indices_.size()) + 
                                  " tracks (query: '" + filter_query_ + "')");
}

void Browser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    render(canvas, rect, snap, false);
}

void Browser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused) {
    // Store as shared_ptr to keep snapshot alive even if original goes out of scope
    g_current_snapshot = std::make_shared<model::Snapshot>(snap);

    auto theme = config::ThemeManager::get_theme("terminal");
    const auto& tracks = snap.library->tracks;

    // Check if filter needs update (dirty flag or library changed size)
    if (filter_dirty_ || tracks.size() != last_library_size_) {
        update_filtered_indices(snap);
    }

    // Layout calculations
    LayoutRect content_rect = rect;
    
    // Draw border and title (highlight when focused)
    std::string title = "LIBRARY";
    if (!filter_query_.empty()) {
        title += " [SEARCH: " + filter_query_ + "]";
        title += " [" + std::to_string(filtered_indices_.size()) + "/" + std::to_string(tracks.size()) + "]";
    } else {
        title += " [" + std::to_string(tracks.size()) + " TRACKS]";
    }
    
    auto inner_rect = draw_box_border(canvas, content_rect, title, Style{}, is_focused);

    // Empty library - show loading indicator if scanning
    if (tracks.empty()) {
        if (snap.library->is_scanning) {
            render_loading_indicator(canvas, inner_rect, snap);
        }
        return;
    }

    // Clamp selection to FILTERED list
    int total_items = static_cast<int>(filtered_indices_.size());
    if (total_items == 0) {
        canvas.draw_text(inner_rect.x + 2, inner_rect.y + 2, "(no matches)", 
                        Style{Color::Default, Color::Default, Attribute::Dim});
        return;
    }

    if (selected_index_ >= total_items) {
        selected_index_ = std::max(0, total_items - 1);
    }

    // Calculate viewport (available lines)
    int available_lines = inner_rect.height;
    if (available_lines < 1) available_lines = 1;

    // Smart scrolling: keep selection visible
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + available_lines) {
        scroll_offset_ = selected_index_ - available_lines + 1;
    }

    // Clamp scroll
    if (scroll_offset_ > total_items - available_lines) {
        scroll_offset_ = std::max(0, total_items - available_lines);
    }
    if (scroll_offset_ < 0) scroll_offset_ = 0;

    // Render visible tracks
    int end_index = std::min(total_items, scroll_offset_ + available_lines);
    int y = inner_rect.y;

    for (int i = scroll_offset_; i < end_index; ++i) {
        // Map visual index 'i' to real track index
        int real_index = filtered_indices_[i];
        const auto& track = tracks[real_index];
        
        bool is_cursor = (i == selected_index_);
        bool is_marked = is_selected(real_index);

        // If cursor or marked, highlight the entire line with single color
        if (is_cursor || is_marked) {
            // Build single-color line
            std::string prefix = is_cursor ? "▶ " : "  ";
            std::ostringstream oss;
            oss << prefix;

            // Format: [Artist] [Album]: [Track Number] [Song]
            if (!track.artist.empty()) {
                oss << track.artist;
            } else {
                oss << "Unknown Artist";
            }

            if (!track.album.empty()) {
                oss << " " << track.album;
            }

            oss << ": ";

            if (track.track_number > 0) {
                oss << std::setfill('0') << std::setw(2) << track.track_number << " ";
            }

            if (!track.title.empty()) {
                oss << track.title;
            } else {
                oss << "Untitled";
            }

            Style style = is_cursor ?
                Style{Color::BrightYellow, Color::Default, Attribute::Bold} :
                Style{Color::Yellow, Color::Default, Attribute::None};

            // Truncate the entire line to fit width
            std::string full_line = oss.str();
            canvas.draw_text(inner_rect.x, y++, truncate_text(full_line, inner_rect.width), style);
        } else {
            // Normal: multi-color rendering
            int x = inner_rect.x;
            int line_y = y++;
            int remaining_w = inner_rect.width;

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

            // Album (Default, no quotes)
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

            // Title (BrightWhite, no quotes)
            draw_part(!track.title.empty() ? track.title : "Untitled", 
                     Style{Color::BrightWhite, Color::Default, Attribute::None});
        }

        if (y >= inner_rect.y + inner_rect.height) break;
    }
}

// ... render_loading_indicator unchanged ...
void Browser::render_loading_indicator(Canvas& canvas, const LayoutRect& content_rect, const model::Snapshot& snap) {
    using namespace std::chrono;

    auto now = steady_clock::now();

    // Initialize on first call
    static auto first_call = now;
    auto elapsed = duration_cast<milliseconds>(now - first_call);

    // TOGGLE MESSAGE every 3 seconds
    int cycle = (elapsed.count() / 3000) % 2;
    std::string message = (cycle == 0) ? "Loading..." : "Building LIBRARY cache file...";

    // CHARACTER HIGHLIGHT every 250ms
    int char_index = (elapsed.count() / 250) % message.length();

    // CENTER MESSAGE in content area
    int center_y = content_rect.y + (content_rect.height / 2);
    int center_x = content_rect.x + (content_rect.width / 2) - (message.length() / 2);

    // RENDER character-by-character with highlight
    for (size_t i = 0; i < message.length(); ++i) {
        Style style = (static_cast<size_t>(char_index) == i) ?
            Style{Color::BrightWhite, Color::Default, Attribute::Bold} :  // Highlighted char (whiteish)
            Style{Color::BrightYellow, Color::Default, Attribute::None};   // Normal char (yellow)

        canvas.draw_text(center_x + static_cast<int>(i), center_y, std::string(1, message[i]), style);
    }

    // Show progress count if available (below main message)
    int progress_y = center_y + 2;

    if (snap.library->total_count > 0) {
        std::string progress = "[" + std::to_string(snap.library->scanned_count) + "/" +
                              std::to_string(snap.library->total_count) + " TRACKS LOADED...]";
        int progress_x = content_rect.x + (content_rect.width / 2) - (progress.length() / 2);

        canvas.draw_text(progress_x, progress_y, progress,
                        Style{Color::White, Color::Default, Attribute::None});

        // Calculate and display estimated completion time
        auto elapsed_seconds = duration_cast<seconds>(elapsed).count();
        if (elapsed_seconds > 0 && snap.library->scanned_count > 0) {
            int remaining = snap.library->total_count - snap.library->scanned_count;
            double tracks_per_second = static_cast<double>(snap.library->scanned_count) / elapsed_seconds;

            if (tracks_per_second > 0 && remaining > 0) {
                int eta_seconds = static_cast<int>(remaining / tracks_per_second);

                // Calculate actual completion time
                auto completion_time = std::chrono::system_clock::now() + seconds(eta_seconds);
                auto completion_time_t = std::chrono::system_clock::to_time_t(completion_time);
                std::tm* tm_info = std::localtime(&completion_time_t);

                char time_buf[16];
                std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

                std::string eta_str = "Estimated completion: " + std::string(time_buf);

                int eta_x = content_rect.x + (content_rect.width / 2) - (eta_str.length() / 2);
                canvas.draw_text(eta_x, progress_y + 1, eta_str,
                                Style{Color::Cyan, Color::Default, Attribute::None});
            }

            // Slow scan detection - show notice after 15 seconds if rate < 1000 tracks/min
            double tracks_per_minute = tracks_per_second * 60.0;
            if (elapsed_seconds >= 15 && tracks_per_minute < 1000.0) {
                std::string notice = "NOTICE: OUROBOROS has detected slow cache rendering. Please be patient as the cache is built.";
                std::string notice2 = "Computations within OUROBOROS will resolve faster upon completion.";

                int notice_x = content_rect.x + (content_rect.width / 2) - (notice.length() / 2);
                int notice2_x = content_rect.x + (content_rect.width / 2) - (notice2.length() / 2);
                int notice_y = progress_y + 3;

                canvas.draw_text(notice_x, notice_y, notice,
                                Style{Color::Yellow, Color::Default, Attribute::None});
                canvas.draw_text(notice2_x, notice_y + 1, notice2,
                                Style{Color::Yellow, Color::Default, Attribute::None});
            }
        }
    } else {
        // Validation phase - use IDENTICAL format to scan phase
        std::string progress = "[VALIDATING CACHE...]";
        int progress_x = content_rect.x + (content_rect.width / 2) - (progress.length() / 2);

        canvas.draw_text(progress_x, progress_y, progress,
                        Style{Color::White, Color::Default, Attribute::None});

        auto elapsed_seconds = duration_cast<seconds>(elapsed).count();
        if (elapsed_seconds >= 5) {
            // Estimated completion: show elapsed time (same Cyan style)
            std::string eta_str = "Estimated completion: calculating...";

            int eta_x = content_rect.x + (content_rect.width / 2) - (eta_str.length() / 2);
            canvas.draw_text(eta_x, progress_y + 1, eta_str,
                            Style{Color::Cyan, Color::Default, Attribute::None});
        }

        // NOTICE after 15 seconds (same Yellow style, same format)
        if (elapsed_seconds >= 15) {
            std::string notice = "NOTICE: OUROBOROS has detected slow cache validation. Please be patient as the cache is validated.";
            std::string notice2 = "Computations within OUROBOROS will resolve faster upon completion.";

            int notice_x = content_rect.x + (content_rect.width / 2) - (notice.length() / 2);
            int notice2_x = content_rect.x + (content_rect.width / 2) - (notice2.length() / 2);
            int notice_y = progress_y + 3;

            canvas.draw_text(notice_x, notice_y, notice,
                            Style{Color::Yellow, Color::Default, Attribute::None});
            canvas.draw_text(notice2_x, notice_y + 1, notice2,
                            Style{Color::Yellow, Color::Default, Attribute::None});
        }
    }
}

// ... toggle_selection unchanged ...
void Browser::toggle_selection(int index) {
    if (selected_indices_.count(index)) {
        selected_indices_.erase(index);
    } else {
        selected_indices_.insert(index);
    }
}

void Browser::handle_input(const InputEvent& event) {
    // ESC: Clear Filter
    if ((event.key_name == "escape" || event.key == 27) && !filter_query_.empty()) {
        set_filter("");
        return;
    }

    // Determine effective size (filtered or full)
    int total_items = static_cast<int>(filtered_indices_.size());
    if (total_items == 0 && filtered_indices_.empty() && g_current_snapshot) {
         if (!g_current_snapshot->library->tracks.empty() && filter_query_.empty()) {
             total_items = g_current_snapshot->library->tracks.size();
         }
    }

    // Navigation (from TOML: nav_up, nav_down)
    if (matches_keybind(event, "nav_up")) {
        if (selected_index_ > 0) selected_index_--;
    }
    else if (matches_keybind(event, "nav_down")) {
        selected_index_++;
        if (selected_index_ >= total_items) selected_index_ = std::max(0, total_items - 1);
    }
    // Move down AND toggle selection (from TOML: nav_select_down)
    else if (matches_keybind(event, "nav_select_down")) {
        if (selected_index_ < total_items) {
             if (selected_index_ < (int)filtered_indices_.size()) {
                 toggle_selection(filtered_indices_[selected_index_]);
             }
             selected_index_++;
        }
    }
    // Move up AND toggle selection (from TOML: nav_select_up)
    else if (matches_keybind(event, "nav_select_up")) {
        if (selected_index_ < (int)filtered_indices_.size()) {
            toggle_selection(filtered_indices_[selected_index_]);
        }
        if (selected_index_ > 0) selected_index_--;
    }
    else if (matches_keybind(event, "select")) {
        if (!g_current_snapshot || g_current_snapshot->library->tracks.empty()) {
            return;
        }

        auto& bus = events::EventBus::instance();

        if (!selected_indices_.empty()) {
            for (int idx : selected_indices_) {
                if (idx >= 0 && idx < static_cast<int>(g_current_snapshot->library->tracks.size())) {
                    events::Event evt;
                    evt.type = events::Event::Type::AddTrackToQueue;
                    evt.index = idx;
                    bus.publish(evt);
                }
            }
            clear_selection();
        }
        else if (selected_index_ >= 0 && selected_index_ < total_items) {
            if (selected_index_ < (int)filtered_indices_.size()) {
                int real_index = filtered_indices_[selected_index_];
                
                events::Event evt;
                evt.type = events::Event::Type::AddTrackToQueue;
                evt.index = real_index;
                bus.publish(evt);
            }
        }
    }
}

SizeConstraints Browser::get_constraints() const {
    SizeConstraints constraints;
    constraints.min_height = 12; 
    return constraints;
}

}  // namespace ouroboros::ui::widgets
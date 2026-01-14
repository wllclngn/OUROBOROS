#include "ui/widgets/AlbumBrowser.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/ArtworkWindow.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/Config.hpp"
#include "config/Theme.hpp"
#include "events/EventBus.hpp"
#include "util/TimSort.hpp"
#include "util/BoyerMoore.hpp"
#include "util/Logger.hpp"
#include "util/UnicodeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>
#include <numeric>
#include <set>

namespace ouroboros::ui::widgets {

// Global SHARED POINTER to current snapshot for event handling
// This prevents dangling pointer issues when snap reference goes out of scope
static std::shared_ptr<const model::Snapshot> g_current_snapshot = nullptr;

void AlbumBrowser::set_filter(const std::string& query) {
    ouroboros::util::Logger::debug("AlbumBrowser::set_filter: '" + query + "' (current: '" + filter_query_ + "')");
    if (filter_query_ != query) {
        filter_query_ = query;
        filter_dirty_ = true;
        prefetch_completed_ = false; // Reset prefetch on filter change
    }
}

void AlbumBrowser::update_filtered_albums() {
    filtered_album_indices_.clear();

    if (filter_query_.empty()) {
        filtered_album_indices_.resize(albums_.size());
        std::iota(filtered_album_indices_.begin(), filtered_album_indices_.end(), 0);
        return;
    }

    // Normalize query for Unicode-aware search (bjork matches Björk)
    std::string normalized_query = util::normalize_for_search(filter_query_);
    util::BoyerMooreSearch searcher(normalized_query);

    for (size_t i = 0; i < albums_.size(); ++i) {
        const auto& album = albums_[i];
        bool match = false;

        // Search in normalized Album Title
        if (searcher.search(util::normalize_for_search(album.title)) != -1) {
            match = true;
        }
        // Search in normalized Artist
        else if (searcher.search(util::normalize_for_search(album.artist)) != -1) {
            match = true;
        }

        if (match) {
            filtered_album_indices_.push_back(i);
        }
    }

    // Reset selection if out of bounds
    if (selected_index_ >= (int)filtered_album_indices_.size()) {
        selected_index_ = 0;
    }

    content_changed_ = true;

    ouroboros::util::Logger::debug("AlbumBrowser: Filtered " + std::to_string(albums_.size()) +
                                   " -> " + std::to_string(filtered_album_indices_.size()) + " albums");
}

void AlbumBrowser::refresh_cache(const model::Snapshot& snap) {
    ouroboros::util::Logger::info("AlbumBrowser: refresh_cache called with " +
                                  std::to_string(snap.library->tracks.size()) + " tracks");

    // Group tracks by Album + Artist
    std::map<std::string, AlbumGroup> groups;

    for (size_t i = 0; i < snap.library->tracks.size(); ++i) {
        const auto& track = snap.library->tracks[i];

        // Extract directory for grouping key (handles compilation/featured artist albums)
        std::string album_dir;
        if (!track.path.empty()) {
            namespace fs = std::filesystem;
            album_dir = fs::path(track.path).parent_path().string();
        }

        // Group by: album name + directory (prevents duplicates for various-artist/featured albums)
        std::string key = track.album + "::" + album_dir;

        if (groups.find(key) == groups.end()) {
            AlbumGroup g;
            g.title = track.album.empty() ? "Unknown Album" : track.album;
            g.artist = track.artist.empty() ? "Unknown Artist" : track.artist;
            g.year = track.date;  // Use date field for year

            // Store first track path for artwork lookup via ArtworkWindow
            g.representative_track_path = track.path;

            // Store directory
            g.album_directory = album_dir;

            groups[key] = g;

            ouroboros::util::Logger::debug("AlbumBrowser: Created album group '" + g.title +
                                          "' with representative_track_path: " + g.representative_track_path);
        }
        groups[key].track_indices.push_back(i);
    }

    albums_.clear();
    for (auto& [k, v] : groups) {
        albums_.push_back(std::move(v));
    }

    // Sort tracks within each album by track number
    for (auto& album : albums_) {
        std::sort(album.track_indices.begin(), album.track_indices.end(),
            [&snap](int a, int b) {
                return snap.library->tracks[a].track_number <
                       snap.library->tracks[b].track_number;
            });
    }

    // Sort albums by Artist, then by Year or Title (based on config)
    const auto& cfg = backend::Config::instance();
    bool sort_by_year = cfg.sort_albums_by_year;
    bool ignore_the = cfg.sort_ignore_the_prefix;
    bool ignore_bracket = cfg.sort_ignore_bracket_prefix;

    // Helper to get sort key for artist (strips prefixes based on config)
    auto get_artist_sort_key = [ignore_the, ignore_bracket](const std::string& artist) -> std::string {
        if (artist.empty()) return artist;
        size_t start = 0;
        // Strip "The " prefix if configured (case-insensitive)
        if (ignore_the && artist.size() >= 4) {
            if ((artist[0] == 'T' || artist[0] == 't') &&
                (artist[1] == 'H' || artist[1] == 'h') &&
                (artist[2] == 'E' || artist[2] == 'e') &&
                artist[3] == ' ') {
                start = 4;
            }
        }
        // Strip "[" prefix if configured
        if (ignore_bracket && start < artist.size() && artist[start] == '[') {
            start++;
        }
        return (start > 0) ? artist.substr(start) : artist;
    };

    // Helper to convert year string to int for numeric comparison
    auto year_to_int = [](const std::string& y) -> int {
        if (y.empty()) return 9999;  // Unknown years sort last
        try {
            // Extract first 4 digits (handles "2020-01-15" format)
            std::string year_str = y.substr(0, 4);
            return std::stoi(year_str);
        } catch (...) {
            return 9999;
        }
    };

    ouroboros::util::timsort(albums_, [sort_by_year, &get_artist_sort_key, &year_to_int](const AlbumGroup& a, const AlbumGroup& b) {
        // Case-insensitive artist comparison with prefix stripping
        int cmp = util::case_insensitive_compare(get_artist_sort_key(a.artist), get_artist_sort_key(b.artist));
        if (cmp != 0) return cmp < 0;

        // Within same artist: sort by year (NUMERIC) or title
        if (sort_by_year) {
            int ya = year_to_int(a.year);
            int yb = year_to_int(b.year);
            if (ya != yb) return ya < yb;
        }
        // Fall back to case-insensitive title comparison
        return util::case_insensitive_compare(a.title, b.title) < 0;
    });

    // Initial update of filtered indices (matches all)
    update_filtered_albums();

    ouroboros::util::Logger::info("AlbumBrowser: refresh_cache complete - " +
                                  std::to_string(albums_.size()) + " albums created");
}

void AlbumBrowser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    render(canvas, rect, snap, false);
}

void AlbumBrowser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused) {
    // Store as shared_ptr to keep snapshot alive even if original goes out of scope
    g_current_snapshot = std::make_shared<model::Snapshot>(snap);

    // Refresh cache if library changed
    if (albums_.empty() && !snap.library->tracks.empty()) {
        refresh_cache(snap);
    }

    auto theme = config::ThemeManager::get_theme("terminal");

    // Calculate content rect (inner area minus 1-char border on all sides)
    LayoutRect content_rect{
        rect.x + 1,
        rect.y + 1,
        rect.width - 2,
        rect.height - 2
    };

    // Update filter if needed
    if (filter_dirty_) {
        ouroboros::util::Logger::debug("AlbumBrowser::render: filter_dirty_ is true, updating...");
        update_filtered_albums();
        filter_dirty_ = false;
    }

    // DYNAMIC CALCULATION: Grid dimensions based on container

    // Configurable columns
    cols_ = backend::Config::instance().album_grid_columns;
    if (cols_ < 1) cols_ = 1;

    // 1. Calculate Width First (Fill the screen - NO horizontal gap between columns)
    const int cell_w = content_rect.width / cols_;

    // 2. Calculate Height Second (Maintain aspect ratio)
    // Terminal aspect ratio correction: 1 col width ≈ 0.5 row height visually
    // So for a square image: height_rows = width_cols / 2
    const int TEXT_LINES = 1;
    const int BORDER_H = 2;
    const int PADDING_W = 2; // Left + Right borders

    int art_width = cell_w - PADDING_W;
    int art_height = art_width / 2; // Aspect ratio correction

    // Total cell height
    const int cell_h = art_height + TEXT_LINES + BORDER_H;

    // Center the grid
    int grid_width = cols_ * cell_w;
    int x_offset = (content_rect.width - grid_width) / 2;

    // Use filtered indices for rendering
    int total_albums = filtered_album_indices_.size();
    if (total_albums == 0) {
        canvas.draw_text(content_rect.x + 2, content_rect.y + 2,
                        filter_query_.empty() ? "(no albums)" : "(no matching albums)",
                        Style{Color::Default, Color::Default, Attribute::Dim});
        return;
    }

    int total_rows = (total_albums + cols_ - 1) / cols_;

    // Ensure selection in bounds
    if (selected_index_ >= total_albums) selected_index_ = std::max(0, total_albums - 1);

    int selected_row = selected_index_ / cols_;

    // Scrolling logic
    int visible_rows = content_rect.height / cell_h;
    if (visible_rows < 1) visible_rows = 1;

    if (selected_row < scroll_offset_) {
        scroll_offset_ = selected_row;
    } else if (selected_row >= scroll_offset_ + visible_rows) {
        scroll_offset_ = selected_row - visible_rows + 1;
    }

    // Box Dimensions
    int box_w = art_width + PADDING_W;
    int box_h = cell_h;

    // Render visible albums in grid
    int y_offset = content_rect.y;

    for (int r = scroll_offset_; r < total_rows; ++r) {
        // Allow partial rows to render - border will clip overflow naturally
        if (y_offset >= content_rect.y + content_rect.height) break;

        for (int c = 0; c < cols_; ++c) {
            int idx = r * cols_ + c;
            if (idx >= total_albums) break;

            // Lookup actual album from filtered list
            size_t album_idx = filtered_album_indices_[idx];
            const auto& album = albums_[album_idx];
            bool is_selected = (idx == selected_index_);

            // Calculate cell position
            int cell_x = content_rect.x + x_offset + c * cell_w;
            int cell_y = y_offset;

            // Box position - centered in cell
            int box_x = cell_x + (cell_w - box_w) / 2;
            int box_y = cell_y;

            // Determine border color
            Style border_style;
            if (is_selected) {
                border_style = Style{Color::BrightYellow, Color::Default, Attribute::Bold};
            } else {
                border_style = Style{Color::BrightBlack, Color::Default, Attribute::None};
            }

            // Draw border around ARTWORK BOX
            canvas.draw_rect(box_x, box_y, box_w, box_h, border_style);

            // Text width based on box width
            int text_width = box_w - 3;  // Box width - borders - padding

            // Prepare strings
            std::string artist_str = album.artist + ": ";
            std::string album_str = album.title;

            // Calculate visual widths
            int artist_display_w = ouroboros::ui::display_cols(artist_str);
            int album_display_w = ouroboros::ui::display_cols(album_str);

            int max_artist_w = text_width;

            // Smart truncation strategy
            if (artist_display_w + album_display_w > text_width) {
                // If total is too long...
                if (artist_display_w > text_width * 0.6) {
                    // Cap artist at 60% (min 5 chars)
                    max_artist_w = std::max(static_cast<int>(text_width * 0.6), 5);
                } else {
                    // Artist fits in <60%, let it stay, squeeze album
                    max_artist_w = artist_display_w;
                }
            }

            // Draw Artist (Bold)
            std::string art_trunc = truncate_text(artist_str, max_artist_w);
            int next_x = canvas.draw_text(box_x + 1, box_y + box_h - 2, art_trunc,
                                        Style{Color::BrightWhite, Color::Default, Attribute::Bold});

            // Draw Album (Dim) in remaining space
            int remaining = (box_x + 1 + text_width) - next_x;
            if (remaining > 2) {
                std::string alb_trunc = truncate_text(album_str, remaining);
                canvas.draw_text(next_x, box_y + box_h - 2, alb_trunc,
                               Style{Color::Default, Color::Default, Attribute::Dim});
            }
        }

        y_offset += box_h;
    }

    // Draw border (will be redrawn on top of images in draw_border_overlay)
    std::string title = "LIBRARY";
    if (!filter_query_.empty()) {
        title += " [SEARCH: " + filter_query_ + "]";
    } else {
        title += " [" + std::to_string(albums_.size()) + " ALBUMS]";
    }

    draw_box_border(canvas, rect, title, Style{}, is_focused);
}

void AlbumBrowser::handle_input(const InputEvent& event) {
    // Clear filter on ESC if not searching
    if ((event.key == 27 || event.key_name == "escape") && !filter_query_.empty()) {
        ouroboros::util::Logger::debug("AlbumBrowser: ESC pressed, clearing filter");
        set_filter("");
        return;
    }

    if (albums_.empty()) return;
    int total_albums = filtered_album_indices_.size();
    if (total_albums == 0) return;

    // Track old selection for change detection
    int old_selected = selected_index_;

    // Grid navigation: hjkl or arrow keys
    if (event.key_name == "right" || event.key == 'l') {
        if (selected_index_ < total_albums - 1) selected_index_++;
    }
    else if (event.key_name == "left" || event.key == 'h') {
        if (selected_index_ > 0) selected_index_--;
    }
    else if (event.key_name == "down" || event.key == 'j') {
        if (selected_index_ + cols_ < total_albums) selected_index_ += cols_;
    }
    else if (event.key_name == "up" || event.key == 'k') {
        if (selected_index_ - cols_ >= 0) selected_index_ -= cols_;
    }

    // If selection moved significantly (more than 2 rows), clear cache and reload
    int jump_threshold = cols_ * 2;  // Allow normal row navigation
    if (std::abs(selected_index_ - old_selected) > jump_threshold) {
        ouroboros::util::Logger::debug("AlbumBrowser: Large selection jump detected (from " +
                                      std::to_string(old_selected) + " to " +
                                      std::to_string(selected_index_) +
                                      "), resetting artwork window");
        ArtworkWindow::instance().reset();
    }

    if (event.key_name == "enter" || event.key == '\n' || event.key == '\r') {
        // Add all tracks in album to queue (using safe shared_ptr pattern from Browser)
        if (!g_current_snapshot || g_current_snapshot->library->tracks.empty()) {
            return;
        }

        auto& bus = events::EventBus::instance();
        // Lookup actual album from filtered list
        size_t album_idx = filtered_album_indices_[selected_index_];
        const auto& album = albums_[album_idx];

        // Queue each track in the album (same pattern as Browser multi-select)
        for (int idx : album.track_indices) {
            // Safety check: ensure index is valid
            if (idx >= 0 && idx < static_cast<int>(g_current_snapshot->library->tracks.size())) {
                events::Event evt;
                evt.type = events::Event::Type::AddTrackToQueue;
                evt.index = idx;
                bus.publish(evt);
            }
        }
    }
}

SizeConstraints AlbumBrowser::get_constraints() const {
    // AlbumBrowser needs space for album grid - minimum size to show at least 1 album tile
    SizeConstraints constraints;
    constraints.min_height = 12;  // Enough for 1 album tile + border
    return constraints;
}

void AlbumBrowser::render_images_if_needed(const LayoutRect& rect, bool force_render) {
    auto& img_renderer = ImageRenderer::instance();
    if (!img_renderer.images_supported()) {
        ouroboros::util::Logger::warn("AlbumBrowser: Images not supported, skipping artwork render");
        return;
    }

    auto& artwork_window = ArtworkWindow::instance();

    // Fix 0: Content Changed - Full wipe if filter changed
    if (content_changed_) {
        ouroboros::util::Logger::debug("AlbumBrowser: Content changed (filter), wiping " + std::to_string(displayed_images_.size()) + " images");
        for (const auto& [key, info] : displayed_images_) {
            img_renderer.delete_image_by_id(info.image_id);
        }
        displayed_images_.clear();
        force_render = true;
        content_changed_ = false;
    }

    // Check if ArtworkWindow has pending updates (artwork just finished loading)
    bool has_async_updates = artwork_window.has_updates();
    if (has_async_updates) {
        artwork_window.clear_updates();
        ouroboros::util::Logger::debug("AlbumBrowser: ArtworkWindow has pending updates, forcing render");
        force_render = true;
    }

    // Fix 1: Initial Load - Force render if we have albums but haven't displayed anything yet
    if (!force_render && !filtered_album_indices_.empty() && displayed_images_.empty()) {
        ouroboros::util::Logger::debug("AlbumBrowser: First render detected, forcing update");
        force_render = true;
    }

    // Smart prefetch: Debounce to prevent per-frame request spam
    auto now = std::chrono::steady_clock::now();

    // Track scroll position changes BEFORE debounce (must always run)
    // Big jump = moved MANY rows in SHORT time (holding key, not pressing slowly)

    // First-call initialization
    if (last_scroll_offset_ < 0) {
        last_scroll_offset_ = scroll_offset_;
        scroll_start_offset_ = scroll_offset_;
        scroll_start_time_ = now;
        was_scrolling_ = false;
    }

    bool scroll_changed = (last_scroll_offset_ != scroll_offset_);

    if (scroll_changed) {
        // User is moving
        if (!was_scrolling_) {
            // IDLE -> MOVING: Start new scroll session
            scroll_start_offset_ = last_scroll_offset_;
            scroll_start_time_ = now;
            was_scrolling_ = true;
        }

        last_scroll_time_ = now;
        prefetch_completed_ = false;
        last_scroll_offset_ = scroll_offset_;

    } else if (was_scrolling_) {
        // MOVING -> IDLE: User just stopped
        // Calculate if this was a big jump using velocity + distance formula
        int distance = std::abs(scroll_offset_ - scroll_start_offset_);
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - scroll_start_time_).count();

        // Velocity = rows per second (avoid div by zero)
        double time_seconds = (elapsed_ms > 0 ? elapsed_ms : 1) / 1000.0;
        double velocity = distance / time_seconds;

        // Big jump if: (distance >= 8 AND velocity > 10) OR distance > 25
        bool is_big_jump = (distance >= 8 && velocity > 10.0) || (distance > HUGE_JUMP_ROWS);

        ouroboros::util::Logger::debug("AlbumBrowser: Stopped - " + std::to_string(distance) +
                                      " rows, " + std::to_string(elapsed_ms) + "ms, vel=" +
                                      std::to_string(velocity).substr(0, 4) + " r/s" +
                                      (is_big_jump ? " -> BIG JUMP" : ""));

        if (is_big_jump) {
            ouroboros::util::Logger::info("AlbumBrowser: BIG JUMP - resetting artwork cache");

            artwork_window.reset();

            for (const auto& [key, info] : displayed_images_) {
                img_renderer.delete_image_by_id(info.image_id);
            }
            displayed_images_.clear();
            force_render = true;
        }

        was_scrolling_ = false;
    }

    // Debounce: Skip artwork requests if called too rapidly (per-frame spam during scroll)
    if (!force_render && (now - last_request_time_) < SCROLL_DEBOUNCE_MS) {
        return;
    }
    last_request_time_ = now;

    // DYNAMIC CALCULATION: Use same logic as render()

    // Configurable columns
    int cols_available = backend::Config::instance().album_grid_columns;
    if (cols_available < 1) cols_available = 1;

    // Calculate content area from actual widget rect
    int content_x = rect.x + 1;
    int content_y = rect.y + 1;
    int content_width = rect.width - 2;
    int content_height = rect.height - 2;

    // NO gap between cells - must match render()
    const int cell_w = content_width / cols_available;

    const int TEXT_LINES = 1;
    const int BORDER_H = 2;
    const int PADDING_W = 2;

    int art_width = cell_w - PADDING_W;
    int art_height = art_width / 2;

    int art_cols = art_width;
    int art_rows = art_height;

    const int cell_h = art_height + TEXT_LINES + BORDER_H;

    int grid_width = cols_available * cell_w;
    int x_offset = (content_width - grid_width) / 2;

    // Calculate visible rows
    int visible_rows_grid = (content_height + cell_h - 1) / cell_h;
    if (visible_rows_grid < 1) visible_rows_grid = 1;

    int start_row = scroll_offset_;
    int end_row = start_row + visible_rows_grid + 1;

    int total_filtered = filtered_album_indices_.size();

    // Calculate selected album's grid position for Manhattan distance calculation
    int selected_row = selected_index_ / cols_available;
    int selected_col = selected_index_ % cols_available;

    ouroboros::util::Logger::debug("AlbumBrowser: Selected position - row=" + std::to_string(selected_row) +
                                  ", col=" + std::to_string(selected_col) +
                                  ", index=" + std::to_string(selected_index_));

    // PRE-LOAD PHASE: Request artwork prioritized by Manhattan distance from selection
    // Batch requests without notifying workers until all are queued (ensures priority ordering)
    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            size_t album_idx = filtered_album_indices_[idx];
            auto& album = albums_[album_idx];

            if (!album.representative_track_path.empty()) {
                // Calculate Manhattan distance from selected album
                int distance = std::abs(r - selected_row) + std::abs(c - selected_col);

                // Request with notify=false to batch (priority ordering preserved)
                artwork_window.request(
                    album.representative_track_path,
                    distance,
                    art_cols,
                    art_rows,
                    false  // Don't notify yet
                );
            }
        }
    }
    // Now notify all workers to process the batch in priority order
    artwork_window.flush_requests();

    // RENDER PHASE - Radial order (by distance from selected)
    int processed_count = 0;
    int ready_count = 0;

    std::unordered_map<std::string, DisplayedImageInfo> new_displayed_images;
    std::unordered_set<uint32_t> active_ids;

    // Build list of visible items sorted by distance from selection
    struct RenderItem {
        int row, col, idx;
        int distance;
        size_t album_idx;
    };
    std::vector<RenderItem> render_items;
    render_items.reserve((end_row - start_row + 1) * cols_available);

    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            int distance = std::abs(r - selected_row) + std::abs(c - selected_col);
            size_t album_idx = filtered_album_indices_[idx];

            render_items.push_back({r, c, idx, distance, album_idx});
        }
    }

    // Sort by distance (radial order from selection)
    std::sort(render_items.begin(), render_items.end(),
              [](const RenderItem& a, const RenderItem& b) { return a.distance < b.distance; });

    // Render in radial order
    for (const auto& item : render_items) {
        auto& album = albums_[item.album_idx];
        processed_count++;

        // Skip albums with missing artwork path
        if (album.representative_track_path.empty()) continue;

        // Query ArtworkWindow for decoded pixels
        const auto* artwork = artwork_window.get_decoded(album.representative_track_path, art_cols, art_rows);
        if (!artwork) continue;

        ready_count++;

        // Calculate cell position from row/col (no gaps)
        int cell_x = content_x + x_offset + item.col * cell_w;
        int cell_y = content_y + (item.row - start_row) * cell_h;

        // Box position - Center in cell
        int box_w = art_cols + 2;
        int box_x = cell_x + (cell_w - box_w) / 2;

        // Artwork area: inside box border
        int art_x = box_x + 1;
        int art_y = cell_y + 1;

        // Clipping logic
        int art_bottom_y = art_y + art_rows;
        int container_bottom_y = content_y + content_height;
        int visible_art_rows = -1;

        if (art_y >= container_bottom_y) {
            continue; // Fully clipped
        }

        if (art_bottom_y > container_bottom_y) {
            visible_art_rows = container_bottom_y - art_y;
            if (visible_art_rows <= 0) continue;
        }

        // Also check right edge
        if (art_x + art_cols > content_x + content_width) continue;

        // Unique key for this position
        std::string display_key = std::to_string(art_x) + "," + std::to_string(art_y) + "," + artwork->hash.substr(0, 16);

        // Check if this image is already displayed EXACTLY here
        auto display_it = displayed_images_.find(display_key);
        if (display_it != displayed_images_.end() && display_it->second.hash == artwork->hash) {
            new_displayed_images[display_key] = display_it->second;
            active_ids.insert(display_it->second.image_id);
            continue;
        }

        // Render artwork using pre-decoded pixels from ArtworkWindow
        ouroboros::util::Logger::debug("AlbumBrowser: RENDERING new image at " + display_key);
        uint32_t image_id = img_renderer.render_image(
            artwork->data,
            artwork->data_size,
            artwork->width,
            artwork->height,
            artwork->format,
            art_x,
            art_y,
            art_cols,
            art_rows,
            artwork->hash,
            visible_art_rows
        );

        if (image_id != 0) {
            new_displayed_images[display_key] = {artwork->hash, image_id};
            active_ids.insert(image_id);
        }
    }

    // CLEANUP PHASE: Delete images that are no longer visible
    int deleted_count = 0;
    for (const auto& [key, info] : displayed_images_) {
        if (new_displayed_images.find(key) == new_displayed_images.end()) {
            if (active_ids.find(info.image_id) == active_ids.end()) {
                img_renderer.delete_image_by_id(info.image_id);
                deleted_count++;
            }
        }
    }
    if (deleted_count > 0) {
        ouroboros::util::Logger::debug("AlbumBrowser: CLEANUP deleted " + std::to_string(deleted_count) +
                                      " images, old=" + std::to_string(displayed_images_.size()) +
                                      " new=" + std::to_string(new_displayed_images.size()));
    }

    // Update state
    displayed_images_ = std::move(new_displayed_images);

    // PREFETCH PHASE (Sliding Window) - Only run when scroll is idle
    auto time_since_scroll = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_scroll_time_
    );

    if (time_since_scroll >= PREFETCH_DELAY_MS && !prefetch_completed_) {
        ouroboros::util::Logger::debug("AlbumBrowser: PREFETCH activated - scroll idle for " + std::to_string(time_since_scroll.count()) + "ms");
        const int PREFETCH_ITEMS = 20;
        int prefetch_rows_count = (PREFETCH_ITEMS + cols_available - 1) / cols_available;

        auto process_prefetch = [&](int r) {
            if (r < 0) return;

            for (int c = 0; c < cols_available; ++c) {
                int idx = r * cols_available + c;
                if (idx >= total_filtered) continue;

                size_t album_idx = filtered_album_indices_[idx];
                auto& album = albums_[album_idx];

                if (album.representative_track_path.empty()) continue;

                // Calculate Manhattan distance for prefetch items
                int distance = std::abs(r - selected_row) + std::abs(c - selected_col);
                // Add 1000 to priority to make prefetch lower priority than visible
                artwork_window.request(
                    album.representative_track_path,
                    distance + 1000,
                    art_cols,
                    art_rows,
                    false  // Batch - don't notify yet
                );
            }
        };

        // Prefetch Previous rows
        for (int r = start_row - prefetch_rows_count; r < start_row; ++r) process_prefetch(r);
        // Prefetch Next rows
        for (int r = end_row; r < end_row + prefetch_rows_count; ++r) process_prefetch(r);
        // Flush after prefetch batch
        artwork_window.flush_requests();
        prefetch_completed_ = true;
        ouroboros::util::Logger::debug("AlbumBrowser: PREFETCH complete");
    }

    ouroboros::util::Logger::info("AlbumBrowser: " +
                                  std::to_string(ready_count) + "/" +
                                  std::to_string(processed_count) +
                                  " artworks ready");
}

}  // namespace ouroboros::ui::widgets

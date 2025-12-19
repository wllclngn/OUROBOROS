#include "ui/widgets/AlbumBrowser.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/ArtworkLoader.hpp"
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

            // Store first track path for artwork lookup via ArtworkLoader
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
        albums_.push_back(v);
    }

    // Sort albums by Artist, then by Year or Title (based on config)
    bool sort_by_year = backend::Config::instance().sort_albums_by_year;
    ouroboros::util::timsort(albums_, [sort_by_year](const AlbumGroup& a, const AlbumGroup& b) {
        // Case-insensitive artist comparison
        int cmp = util::case_insensitive_compare(a.artist, b.artist);
        if (cmp != 0) return cmp < 0;

        // Within same artist: sort by year or title
        if (sort_by_year) {
            if (a.year != b.year) return a.year < b.year;
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

    // 1. Calculate Width First (Fill the screen)
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

    // Center the grid globally
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

    // Box Dimensions (fully dynamic now)
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

            // Calculate cell position (for grid spacing)
            int cell_x = content_rect.x + x_offset + (c * cell_w);
            int cell_y = y_offset;

            // Box position - Center in cell
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

        y_offset += box_h;  // Use actual box height instead of cell_h
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

    // If selection moved significantly, drain stale artwork requests
    if (std::abs(selected_index_ - old_selected) > 2) {
        ouroboros::util::Logger::debug("AlbumBrowser: Large selection jump detected (from " +
                                      std::to_string(old_selected) + " to " +
                                      std::to_string(selected_index_) +
                                      "), clearing stale requests");
        ArtworkLoader::instance().clear_requests();
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
    // ouroboros::util::Logger::debug("AlbumBrowser: render_images_if_needed called - rect(" +
    //                               std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
    //                               std::to_string(rect.width) + "x" + std::to_string(rect.height) +
    //                               ") force_render=" + (force_render ? "true" : "false"));

    auto& img_renderer = ImageRenderer::instance();
    if (!img_renderer.images_supported()) {
        ouroboros::util::Logger::warn("AlbumBrowser: Images not supported, skipping artwork render");
        return;
    }

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

    // Check if ImageRenderer has pending async updates (artwork just finished loading)
    bool has_async_updates = img_renderer.has_pending_updates();
    if (has_async_updates) {
        img_renderer.clear_pending_updates();
        ouroboros::util::Logger::debug("AlbumBrowser: ImageRenderer has pending updates, forcing render");
        force_render = true;
    }

    // Fix 1: Initial Load - Force render if we have albums but haven't displayed anything yet
    if (!force_render && !filtered_album_indices_.empty() && displayed_images_.empty()) {
        ouroboros::util::Logger::debug("AlbumBrowser: First render detected, forcing update");
        force_render = true;
    }

    // OPTIMIZATION: Removed aggressive "delete all on scroll" to prevent flicker.
    // Instead, we track active images and only delete those that scroll off-screen.

    // Smart prefetch: Debounce to prevent per-frame request spam
    auto now = std::chrono::steady_clock::now();

    // Debounce: Skip if called too rapidly (per-frame spam during scroll)
    if (!force_render && (now - last_request_time_) < SCROLL_DEBOUNCE_MS) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time_).count();
        ouroboros::util::Logger::debug("AlbumBrowser: DEBOUNCE - Skipping request (elapsed=" + std::to_string(elapsed_ms) + "ms)");
        return;
    }
    last_request_time_ = now;

    // Track scroll position changes
    if (last_scroll_offset_ != scroll_offset_) {
        last_scroll_time_ = now;
        ouroboros::util::Logger::debug("AlbumBrowser: SCROLL detected - offset changed from " + std::to_string(last_scroll_offset_) + " to " + std::to_string(scroll_offset_));
        last_scroll_offset_ = scroll_offset_;
    }

    // DYNAMIC CALCULATION: Use same logic as render()
    
    // Configurable columns
    int cols_available = backend::Config::instance().album_grid_columns;
    if (cols_available < 1) cols_available = 1;

    // 1. Calculate Width First (Fill the screen)
    // Calculate content area from actual widget rect (draw_box_border uses 1 cell for borders)
    int content_x = rect.x + 1;
    int content_y = rect.y + 1;
    int content_width = rect.width - 2;   // Subtract left+right borders
    int content_height = rect.height - 2;  // Subtract top+bottom borders

    const int cell_w = content_width / cols_available;

    // 2. Calculate Height Second (Maintain aspect ratio)
    const int TEXT_LINES = 1;
    const int BORDER_H = 2;
    const int PADDING_W = 2;

        int art_width = cell_w - PADDING_W;

        int art_height = art_width / 2; // Aspect ratio correction

    

        // Aliases for compatibility with existing code

        int art_cols = art_width;

        int art_rows = art_height;

    

        // Total cell height

        const int cell_h = art_height + TEXT_LINES + BORDER_H;

    

        // Center the grid globally

        int grid_width = cols_available * cell_w;
    int x_offset = (content_width - grid_width) / 2;

    // Calculate visible rows from actual container height
    // Use CEILING division to include partial rows (DYNAMISM)
    int visible_rows_grid = (content_height + cell_h - 1) / cell_h;
    if (visible_rows_grid < 1) visible_rows_grid = 1;

    int start_row = scroll_offset_;
    int end_row = start_row + visible_rows_grid + 1;  // +1 buffer for partially visible rows

    auto& loader = ArtworkLoader::instance();
    int total_filtered = filtered_album_indices_.size();

    // Calculate selected album's grid position for Manhattan distance calculation
    int selected_row = selected_index_ / cols_available;
    int selected_col = selected_index_ % cols_available;

    ouroboros::util::Logger::debug("AlbumBrowser: Selected position - row=" + std::to_string(selected_row) +
                                  ", col=" + std::to_string(selected_col) +
                                  ", index=" + std::to_string(selected_index_));

    // Build visible paths list for viewport tracking
    std::vector<std::string> visible_paths;
    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            size_t album_idx = filtered_album_indices_[idx];
            auto& album = albums_[album_idx];

            if (!album.representative_track_path.empty()) {
                visible_paths.push_back(album.representative_track_path);
            }
        }
    }

    // Update viewport state (dirty flag pattern)
    loader.update_viewport(scroll_offset_, selected_index_, visible_paths);

    ouroboros::util::Logger::debug("AlbumBrowser: Updated viewport with " +
                                  std::to_string(visible_paths.size()) + " visible albums");

    // PRE-LOAD PHASE: Request artwork prioritized by Manhattan distance from selection
    // Priority Queue: Processes by viewport tier (0=visible), then distance (closer first)
    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            size_t album_idx = filtered_album_indices_[idx];
            auto& album = albums_[album_idx];

            if (!album.representative_track_path.empty()) {
                // Calculate Manhattan distance from selected album
                int distance = std::abs(r - selected_row) + std::abs(c - selected_col);

                // Request with priority: tier 0 (visible), distance-based ordering
                loader.request_artwork_with_priority(
                    album.representative_track_path,
                    distance,
                    0  // viewport_tier = 0 (visible, highest priority)
                );
            }
        }
    }

    // RENDER PHASE
    int processed_count = 0;
    int ready_count = 0;

    std::unordered_map<std::string, DisplayedImageInfo> new_displayed_images;
    std::unordered_set<uint32_t> active_ids; // To track which image IDs are still active

    int y_offset = content_y;
    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) break;

            size_t album_idx = filtered_album_indices_[idx];
            auto& album = albums_[album_idx];

            processed_count++;

            // Skip albums with missing artwork path
            if (album.representative_track_path.empty()) continue;

            // Query ArtworkLoader cache (zero-copy reference)
            const auto* artwork = loader.get_artwork_ref(album.representative_track_path);
            if (!artwork || !artwork->loaded) continue;

            ready_count++;

            // Calculate cell position
            int cell_x = content_x + x_offset + (c * cell_w);
            int cell_y = y_offset;

            // Box position - Center in cell
            int box_w = art_cols + 2;
            int box_x = cell_x + (cell_w - box_w) / 2;

            // Artwork area: inside box border, leave 2 lines at bottom for text
            int art_x = box_x + 1;
            int art_y = cell_y + 1;

            // Fix 3: Clipping Logic ("The Boss")
            // Check if artwork extends beyond the content container
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
            
            // Check if this image is already displayed EXACTLY here (no move needed)
            auto display_it = displayed_images_.find(display_key);
            if (display_it != displayed_images_.end() && display_it->second.hash == artwork->hash) {
                // Already drawn at this position, keep it
                new_displayed_images[display_key] = display_it->second;
                active_ids.insert(display_it->second.image_id);
                continue;
            }

            // Render artwork (re-upload/move)
            uint32_t image_id = img_renderer.render_image(
                artwork->jpeg_data,
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
        
        int box_h = art_rows + TEXT_LINES + BORDER_H;
        y_offset += box_h;
    }

    // CLEANUP PHASE: Delete images that are no longer visible anywhere
    for (const auto& [key, info] : displayed_images_) {
        // If this exact position key is not in new set...
        if (new_displayed_images.find(key) == new_displayed_images.end()) {
            // AND the image ID is NOT used elsewhere in the new set...
            if (active_ids.find(info.image_id) == active_ids.end()) {
                // Delete it (it scrolled off screen completely)
                img_renderer.delete_image_by_id(info.image_id);
            }
        }
    }

    // Update state
    displayed_images_ = std::move(new_displayed_images);

    // PREFETCH PHASE (Sliding Window) - Only run when scroll is idle
    auto time_since_scroll = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_scroll_time_
    );

    if (time_since_scroll >= PREFETCH_DELAY_MS) {
        ouroboros::util::Logger::debug("AlbumBrowser: PREFETCH activated - scroll idle for " + std::to_string(time_since_scroll.count()) + "ms");
        // User has paused scrolling - safe to prefetch adjacent rows
        const int PREFETCH_ITEMS = 20;
        int prefetch_rows_count = (PREFETCH_ITEMS + cols_available - 1) / cols_available;

        auto process_prefetch = [&](int r, bool reverse_cols) {
            if (r < 0) return;

            if (reverse_cols) {
                // Priority Queue: Process by distance from selection
                for (int c = cols_available - 1; c >= 0; --c) {
                    int idx = r * cols_available + c;
                    if (idx >= total_filtered) continue;

                    size_t album_idx = filtered_album_indices_[idx];
                    auto& album = albums_[album_idx];

                    if (album.representative_track_path.empty()) continue;

                    // Calculate Manhattan distance for prefetch items
                    int distance = std::abs(r - selected_row) + std::abs(c - selected_col);
                    loader.request_artwork_with_priority(
                        album.representative_track_path,
                        distance,
                        1  // viewport_tier = 1 (prefetch, lower priority than visible)
                    );
                    const auto* artwork = loader.get_artwork_ref(album.representative_track_path);
                    if (artwork && artwork->loaded) {
                        img_renderer.preload_image(
                            artwork->jpeg_data,
                            art_cols,
                            art_rows,
                            artwork->hash
                        );
                    }
                }
            } else {
                for (int c = 0; c < cols_available; ++c) {
                    int idx = r * cols_available + c;
                    if (idx >= total_filtered) break;

                    size_t album_idx = filtered_album_indices_[idx];
                    auto& album = albums_[album_idx];

                    if (album.representative_track_path.empty()) continue;

                    // Calculate Manhattan distance for prefetch items
                    int distance = std::abs(r - selected_row) + std::abs(c - selected_col);
                    loader.request_artwork_with_priority(
                        album.representative_track_path,
                        distance,
                        1  // viewport_tier = 1 (prefetch, lower priority than visible)
                    );
                    const auto* artwork = loader.get_artwork_ref(album.representative_track_path);
                    if (artwork && artwork->loaded) {
                        img_renderer.preload_image(
                            artwork->jpeg_data,
                            art_cols,
                            art_rows,
                            artwork->hash
                        );
                    }
                }
            }
        };

        // Prefetch Previous: Iterate furthest to closest, reverse columns
        // So closest row to viewport pops first
        for (int r = start_row - prefetch_rows_count; r < start_row; ++r) process_prefetch(r, true);
        // Prefetch Next: Iterate closest to furthest, normal columns
        // So closest row to viewport loads first, but gets pushed last (pops first)
        for (int r = end_row + prefetch_rows_count - 1; r >= end_row; --r) process_prefetch(r, true);
        ouroboros::util::Logger::debug("AlbumBrowser: PREFETCH complete");
    } else {
        ouroboros::util::Logger::debug("AlbumBrowser: PREFETCH skipped - scroll active (idle_time=" + std::to_string(time_since_scroll.count()) + "ms)");
    }

    ouroboros::util::Logger::info("AlbumBrowser: " +
                                  std::to_string(ready_count) + "/" +
                                  std::to_string(processed_count) +
                                  " artworks ready");
}

}  // namespace ouroboros::ui::widgets

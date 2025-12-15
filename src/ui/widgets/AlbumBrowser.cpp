#include "ui/widgets/AlbumBrowser.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/ArtworkLoader.hpp"
#include "backend/MetadataParser.hpp"
#include "config/Theme.hpp"
#include "events/EventBus.hpp"
#include "util/TimSort.hpp"
#include "util/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace ouroboros::ui::widgets {

// Global SHARED POINTER to current snapshot for event handling
// This prevents dangling pointer issues when snap reference goes out of scope
static std::shared_ptr<const model::Snapshot> g_current_snapshot = nullptr;

void AlbumBrowser::refresh_cache(const model::Snapshot& snap) {
    ouroboros::util::Logger::info("AlbumBrowser: refresh_cache called with " +
                                  std::to_string(snap.library->tracks.size()) + " tracks");

    // Group tracks by Album + Artist
    std::map<std::string, AlbumGroup> groups;

    for (size_t i = 0; i < snap.library->tracks.size(); ++i) {
        const auto& track = snap.library->tracks[i];
        std::string key = track.album + "::" + track.artist;

        if (groups.find(key) == groups.end()) {
            AlbumGroup g;
            g.title = track.album.empty() ? "Unknown Album" : track.album;
            g.artist = track.artist.empty() ? "Unknown Artist" : track.artist;
            g.year = track.date;  // Use date field for year

            // Store first track path for artwork lookup via ArtworkLoader
            g.representative_track_path = track.path;

            // Extract directory from first track in album
            if (!track.path.empty()) {
                namespace fs = std::filesystem;
                g.album_directory = fs::path(track.path).parent_path().string();
            }

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

    // Sort albums by Artist then Title using Timsort
    ouroboros::util::timsort(albums_, [](const AlbumGroup& a, const AlbumGroup& b) {
        if (a.artist != b.artist) return a.artist < b.artist;
        return a.title < b.title;
    });

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

    // DYNAMIC CALCULATION: Grid dimensions based on container
    // Preferred cell size (user likes 24x10)
    const int preferred_cell_w = 24;
    const int preferred_cell_h = 18;  // Increased for larger artwork

    // Calculate columns that fit in the actual container width
    int cols_available = content_rect.width / preferred_cell_w;
    if (cols_available < 1) cols_available = 1;
    cols_ = cols_available;

    // Actual cell dimensions (use container width, keep preferred height)
    const int cell_w = content_rect.width / cols_available;
    const int cell_h = preferred_cell_h;

    int total_albums = albums_.size();
    if (total_albums == 0) {
        canvas.draw_text(content_rect.x + 2, content_rect.y + 2, "(no albums)",
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

    // Calculate standard box dimensions ONCE (all albums have same cell_w)
    // Terminal cells are ~1:2 (width:height), so cols = 2 × rows for visual square
    const int TEXT_LINES = 2;  // artist + album title
    const int BORDER_H = 2;    // Top + bottom border

    int available_art_width = cell_w - 2;  // Account for borders
    int available_art_height = cell_h - (BORDER_H + TEXT_LINES); // Dynamic calculation, no magic numbers

    int art_cols = available_art_width;
    int art_rows = art_cols / 2;

    if (art_rows > available_art_height) {
        art_rows = available_art_height;
        art_cols = art_rows * 2;
    }

    if (art_cols % 2 != 0) art_cols--;
    art_rows = art_cols / 2;
    int box_w = art_cols + 2;  // Artwork width + left/right borders
    int box_h = art_rows + TEXT_LINES + BORDER_H;  // Artwork + text + borders

    // Render visible albums in grid
    int y_offset = content_rect.y;

    for (int r = scroll_offset_; r < total_rows; ++r) {
        // Allow partial rows to render - border will clip overflow naturally
        if (y_offset >= content_rect.y + content_rect.height) break;

        for (int c = 0; c < cols_; ++c) {
            int idx = r * cols_ + c;
            if (idx >= total_albums) break;

            const auto& album = albums_[idx];
            bool is_selected = (idx == selected_index_);

            // Calculate cell position (for grid spacing)
            int cell_x = content_rect.x + (c * cell_w);
            int cell_y = y_offset;

            // Box position - shift right by 1 to center
            int box_x = cell_x + 1;
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

            // Draw artist (bottom - 2)
            std::string artist = truncate_text(album.artist, text_width);
            canvas.draw_text(box_x + 1, box_y + box_h - 3, artist,
                           Style{Color::BrightWhite, Color::Default, Attribute::Bold});

            // Draw album title (bottom - 1)
            std::string title = truncate_text(album.title, text_width);
            canvas.draw_text(box_x + 1, box_y + box_h - 2, title,
                           Style{Color::Default, Color::Default, Attribute::Dim});
        }

        y_offset += box_h;  // Use actual box height instead of cell_h
    }

    // Draw border (will be redrawn on top of images in draw_border_overlay)
    draw_box_border(canvas, rect, "LIBRARY [" + std::to_string(albums_.size()) + " ALBUMS]", Style{}, is_focused);
}

void AlbumBrowser::handle_input(const InputEvent& event) {
    if (albums_.empty()) return;

    // Grid navigation: hjkl or arrow keys
    if (event.key_name == "right" || event.key == 'l') {
        if (selected_index_ < (int)albums_.size() - 1) selected_index_++;
    }
    else if (event.key_name == "left" || event.key == 'h') {
        if (selected_index_ > 0) selected_index_--;
    }
    else if (event.key_name == "down" || event.key == 'j') {
        if (selected_index_ + cols_ < (int)albums_.size()) selected_index_ += cols_;
    }
    else if (event.key_name == "up" || event.key == 'k') {
        if (selected_index_ - cols_ >= 0) selected_index_ -= cols_;
    }
    else if (event.key_name == "enter" || event.key == '\n' || event.key == '\r') {
        // Add all tracks in album to queue (using safe shared_ptr pattern from Browser)
        if (!g_current_snapshot || g_current_snapshot->library->tracks.empty()) {
            return;
        }

        auto& bus = events::EventBus::instance();
        const auto& album = albums_[selected_index_];

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
    ouroboros::util::Logger::debug("AlbumBrowser: render_images_if_needed called - rect(" +
                                  std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
                                  std::to_string(rect.width) + "x" + std::to_string(rect.height) +
                                  ") force_render=" + (force_render ? "true" : "false"));

    auto& img_renderer = ImageRenderer::instance();
    if (!img_renderer.images_supported()) {
        ouroboros::util::Logger::warn("AlbumBrowser: Images not supported, skipping artwork render");
        return;
    }

    // Check if ImageRenderer has pending async updates (artwork just finished loading)
    bool has_async_updates = img_renderer.has_pending_updates();
    if (has_async_updates) {
        img_renderer.clear_pending_updates();
        ouroboros::util::Logger::debug("AlbumBrowser: ImageRenderer has pending updates, forcing render");
        force_render = true;
    }

    // Fix 1: Initial Load - Force render if we have albums but haven't displayed anything yet
    if (!force_render && !albums_.empty() && displayed_images_.empty()) {
        ouroboros::util::Logger::debug("AlbumBrowser: First render detected, forcing update");
        force_render = true;
    }

    // Fix 2: Stale Images - Surgically delete old images on scroll
    if (scroll_offset_ != last_scroll_offset_) {
        ouroboros::util::Logger::debug("AlbumBrowser: Scroll changed (" + 
                                      std::to_string(last_scroll_offset_) + " -> " + 
                                      std::to_string(scroll_offset_) + "), clearing old images");
        for (const auto& [key, hash] : displayed_images_) {
            img_renderer.delete_image(hash);
        }
        displayed_images_.clear();
        force_render = true;
    } else if (scroll_offset_ == last_scroll_offset_ && !force_render) {
        // Optimization: Skip if nothing changed
        ouroboros::util::Logger::debug("AlbumBrowser: Skipping render - no changes");
        return;
    }

    // Update last scroll offset for next frame
    last_scroll_offset_ = scroll_offset_;

    // DYNAMIC CALCULATION: Use same logic as render() - NO MAGIC NUMBERS
    const int preferred_cell_w = 24;
    const int preferred_cell_h = 18;  // Match render() cell height

    // Calculate content area from actual widget rect (draw_box_border uses 1 cell for borders)
    int content_x = rect.x + 1;
    int content_y = rect.y + 1;
    int content_width = rect.width - 2;   // Subtract left+right borders
    int content_height = rect.height - 2;  // Subtract top+bottom borders

    // Calculate grid dimensions from actual content area
    int cols_available = content_width / preferred_cell_w;
    if (cols_available < 1) cols_available = 1;

    const int cell_w = content_width / cols_available;
    const int cell_h = preferred_cell_h;

    // Calculate visible rows from actual container height
    // Use CEILING division to include partial rows (DYNAMISM)
    int visible_rows_grid = (content_height + cell_h - 1) / cell_h;
    if (visible_rows_grid < 1) visible_rows_grid = 1;

    int start_row = scroll_offset_;
    int end_row = start_row + visible_rows_grid + 1;  // +1 buffer for partially visible rows

    // Calculate standard box dimensions ONCE (must match render() function)
    const int TEXT_LINES = 2;  // artist + album title
    const int BORDER_H = 2;    // Top + bottom border

    int available_art_width = cell_w - 2;
    int available_art_height = cell_h - (BORDER_H + TEXT_LINES);

    int art_cols = available_art_width;
    int art_rows = art_cols / 2;

    if (art_rows > available_art_height) {
        art_rows = available_art_height;
        art_cols = art_rows * 2;
    }

    if (art_cols % 2 != 0) art_cols--;
    art_rows = art_cols / 2;

    auto& loader = ArtworkLoader::instance();

    // PRE-LOAD PHASE: Request all visible artwork BEFORE rendering
    for (int r = start_row; r < end_row && r < (int)albums_.size() / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= (int)albums_.size()) break;

            auto& album = albums_[idx];
            if (!album.representative_track_path.empty()) {
                loader.request_artwork(album.representative_track_path);
            }
        }
    }

    // RENDER PHASE
    int processed_count = 0;
    int ready_count = 0;

    int y_offset = content_y;
    for (int r = start_row; r < end_row && r < (int)albums_.size() / cols_available + 1; ++r) {
        // Early termination check removed - we handle partials below

        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= (int)albums_.size()) break;

            auto& album = albums_[idx];
            processed_count++;

            // Skip albums with missing artwork path
            if (album.representative_track_path.empty()) continue;

            // Query ArtworkLoader cache (zero-copy reference)
            const auto* artwork = loader.get_artwork_ref(album.representative_track_path);
            if (!artwork || !artwork->loaded) continue;

            ready_count++;

            // Calculate cell position
            int cell_x = content_x + (c * cell_w);
            int cell_y = y_offset;

            // Box position - shift right by 1 to center (match render())
            int box_x = cell_x + 1;

            // Artwork area: inside box border, leave 2 lines at bottom for text
            int art_x = box_x + 1;
            int art_y = cell_y + 1;

            // Fix 3: Clipping Logic ("The Boss")
            // Check if artwork extends beyond the content container
            int art_bottom_y = art_y + art_rows;
            int container_bottom_y = content_y + content_height;
            int visible_art_rows = -1;

            if (art_y >= container_bottom_y) {
                // Starts below container - fully clipped
                continue;
            }

            if (art_bottom_y > container_bottom_y) {
                // Partially visible
                visible_art_rows = container_bottom_y - art_y;
                if (visible_art_rows <= 0) continue;
                
                ouroboros::util::Logger::debug("AlbumBrowser: CLIPPING row " + std::to_string(r) + 
                    " item " + std::to_string(c) + ": " + 
                    std::to_string(visible_art_rows) + "/" + std::to_string(art_rows) + " rows");
            }

            // Also check right edge
            if (art_x + art_cols > content_x + content_width) continue;

            // Check if this image is already displayed at this position
            std::string display_key = std::to_string(art_x) + "," + std::to_string(art_y) + "," + artwork->hash.substr(0, 16);
            auto display_it = displayed_images_.find(display_key);

            if (display_it != displayed_images_.end() && display_it->second == artwork->hash) {
                continue;
            }

            // Render artwork (ImageRenderer caches internally)
            bool drawn = img_renderer.render_image(
                artwork->jpeg_data,
                art_x,
                art_y,
                art_cols,
                art_rows,
                artwork->hash,
                visible_art_rows // Pass clipping info
            );

            // Track that this image is now displayed
            if (drawn) {
                displayed_images_[display_key] = artwork->hash;
            }
        }
        
        // Calculate standard box height for y_offset increment
        // Note: We use the FULL box height for stepping, even if clipped, 
        // to maintain grid alignment logic
        int box_h = art_rows + TEXT_LINES + BORDER_H;
        y_offset += box_h;
    }

    // PREFETCH PHASE (Sliding Window)
    // Prefetch nearby images for smooth scrolling
    const int PREFETCH_ITEMS = 20; 
    int prefetch_rows_count = (PREFETCH_ITEMS + cols_available - 1) / cols_available;

    auto process_prefetch = [&](int r) {
        if (r < 0) return;
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= (int)albums_.size()) break;

            auto& album = albums_[idx];
            if (album.representative_track_path.empty()) continue;

            // 1. Ensure Loaded from Disk
            loader.request_artwork(album.representative_track_path);

            // 2. Ensure Decoded in RAM
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
    };

    // Prefetch Previous
    for (int r = start_row - prefetch_rows_count; r < start_row; ++r) process_prefetch(r);
    // Prefetch Next
    for (int r = end_row; r < end_row + prefetch_rows_count; ++r) process_prefetch(r);

    ouroboros::util::Logger::info("AlbumBrowser: " +
                                  std::to_string(ready_count) + "/" +
                                  std::to_string(processed_count) +
                                  " artworks ready");
}

}  // namespace ouroboros::ui::widgets

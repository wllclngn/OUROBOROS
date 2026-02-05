#include "ui/widgets/AlbumBrowser.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/ArtworkWindow.hpp"
#include "ui/InputEvent.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/Config.hpp"
#include "config/Theme.hpp"
#include "events/EventBus.hpp"
#include "util/TimSort.hpp"
#include "util/BoyerMoore.hpp"
#include "util/Logger.hpp"
#include "util/UnicodeUtils.hpp"
#include "util/Platform.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>
#include <numeric>
#include <set>
#include <unordered_map>

namespace ouroboros::ui::widgets {

// ============================================================================
// Atomic Slot Helper Methods
// ============================================================================

size_t AlbumBrowser::get_slot_index(int visible_row, int col) const {
    size_t idx = static_cast<size_t>(visible_row * cols_ + col);
    return (idx < MAX_VISIBLE_SLOTS) ? idx : MAX_VISIBLE_SLOTS - 1;
}

AlbumBrowserSlot* AlbumBrowser::get_slot(int visible_row, int col) {
    return &slots_[get_slot_index(visible_row, col)];
}

void AlbumBrowser::assign_slot(size_t slot_idx, const std::string& album_dir,
                                int x, int y, int cols, int rows) {
    if (slot_idx >= MAX_VISIBLE_SLOTS) return;
    auto& slot = slots_[slot_idx];

    // Check if slot is being reassigned to different album
    if (slot.album_dir != album_dir) {
        // DON'T delete image here - let cleanup phase handle it after new renders
        // Just mark slot as needing new data
        SlotState old_state = slot.state.load(std::memory_order_acquire);
        slot.generation.fetch_add(1, std::memory_order_release);
        slot.album_dir = album_dir;
        slot.state.store(SlotState::Empty, std::memory_order_release);
        ouroboros::util::Logger::debug("AlbumBrowser::assign_slot: RESET slot=" + std::to_string(slot_idx) +
            " old_state=" + std::to_string(static_cast<int>(old_state)) +
            " new_album=" + album_dir.substr(album_dir.rfind('/') + 1));
        slot.decoded_pixels.clear();
        slot.hash.clear();
        slot.image_id = 0;  // Clear ID but don't delete - orphan cleanup handles it
        slot.rendered_x = -1;
        slot.rendered_y = -1;
    }

    // Update display position
    slot.display_x = x;
    slot.display_y = y;
    slot.display_cols = cols;
    slot.display_rows = rows;
}

void AlbumBrowser::clear_all_slots() {
    for (auto& slot : slots_) {
        slot.generation.fetch_add(1, std::memory_order_release);
        slot.state.store(SlotState::Empty, std::memory_order_release);
        slot.album_dir.clear();
        slot.decoded_pixels.clear();
        slot.decoded_pixels.shrink_to_fit();
        slot.hash.clear();
        if (slot.image_id != 0) {
            ImageRenderer::instance().delete_image_by_id(slot.image_id);
            slot.image_id = 0;
        }
        slot.rendered_x = -1;
        slot.rendered_y = -1;
    }
    ouroboros::util::Logger::debug("AlbumBrowser: Cleared all " +
                                   std::to_string(MAX_VISIBLE_SLOTS) + " slots");
}

void AlbumBrowser::clear_all_images() {
    // Delete all tracked images from displayed_images_ map
    auto& img_renderer = ImageRenderer::instance();
    for (const auto& [key, info] : displayed_images_) {
        img_renderer.delete_image_by_id(info.image_id);
    }
    displayed_images_.clear();

    // Also clear slots
    clear_all_slots();

    ouroboros::util::Logger::info("AlbumBrowser: Cleared all album images");
}

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

    // Normalize query once for Unicode-aware search (bjork matches Björk)
    std::string normalized_query = util::normalize_for_search(filter_query_);
    util::BoyerMooreSearch searcher(normalized_query);

    for (size_t i = 0; i < albums_.size(); ++i) {
        const auto& album = albums_[i];

        // Search in pre-computed normalized strings (no allocation per album)
        if (searcher.search(album.normalized_title) != -1 ||
            searcher.search(album.normalized_artist) != -1) {
            filtered_album_indices_.push_back(i);
        }
    }

    // Reset selection if out of bounds
    if (selected_index_ >= util::narrow_cast<int>(filtered_album_indices_.size())) {
        selected_index_ = 0;
    }

    content_changed_ = true;

    ouroboros::util::Logger::debug("AlbumBrowser: Filtered " + std::to_string(albums_.size()) +
                                   " -> " + std::to_string(filtered_album_indices_.size()) + " albums");
}

void AlbumBrowser::refresh_cache(const model::Snapshot& snap) {
    // Albums are pre-computed at library load time (async) - just copy when ready
    if (snap.library->albums.empty()) {
        // Albums still computing in background - will refresh on next snapshot update
        return;
    }

    albums_.clear();
    albums_.reserve(snap.library->albums.size());

    // Copy album groups from snapshot (they use model::AlbumGroup, we use local AlbumGroup)
    for (const auto& album : snap.library->albums) {
        AlbumGroup g;
        g.title = album.title;
        g.artist = album.artist;
        g.year = album.year;
        g.track_indices = album.track_indices;
        g.representative_track_path = album.representative_track_path;
        g.album_directory = album.album_directory;
        g.normalized_title = album.normalized_title;
        g.normalized_artist = album.normalized_artist;
        g.is_scattered = album.is_scattered;
        albums_.push_back(std::move(g));
    }

    // Initial update of filtered indices (matches all)
    update_filtered_albums();

    ouroboros::util::Logger::info("AlbumBrowser: Loaded " + std::to_string(albums_.size()) +
                                  " pre-computed albums from snapshot");
}

void AlbumBrowser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    render(canvas, rect, snap, false);
}

void AlbumBrowser::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused) {
    // Store as shared_ptr to keep snapshot alive even if original goes out of scope
    g_current_snapshot = std::make_shared<model::Snapshot>(snap);

    // Check if albums are still being computed in background
    if (snap.library->albums.empty() && !snap.library->tracks.empty()) {
        // Show loading message while albums compute
        draw_box_border(canvas, rect, "LIBRARY: LOADING ALBUMS...", Style{}, is_focused);
        canvas.draw_text(rect.x + 2, rect.y + 2, "Computing album groups...",
                        Style{Color::Default, Color::Default, Attribute::Dim});
        return;
    }

    // Refresh cache if library changed (albums now available)
    if (albums_.empty() && !snap.library->albums.empty()) {
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
    int total_albums = util::narrow_cast<int>(filtered_album_indices_.size());
    if (total_albums == 0) {
        canvas.draw_text(content_rect.x + 2, content_rect.y + 2,
                        filter_query_.empty() ? "No albums." : "No matching album found.",
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

            // Scattered (compilation) albums: show only title
            // Unified (single-artist) albums: show "Artist: Title"
            if (album.is_scattered) {
                // Compilation - just show album title
                std::string title_trunc = truncate_text(album.title, text_width);
                canvas.draw_text(box_x + 1, box_y + box_h - 2, title_trunc,
                               Style{Color::BrightWhite, Color::Default, Attribute::Bold});
            } else {
                // Single-artist album - show "Artist: Title"
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
        }

        y_offset += box_h;
    }

    // Draw border (will be redrawn on top of images in draw_border_overlay)
    std::string title = "LIBRARY";
    if (!filter_query_.empty()) {
        title += " SEARCH: \"" + filter_query_ + "\", ";
        title += std::to_string(filtered_album_indices_.size()) + "/" + std::to_string(albums_.size()) + " ALBUMS";
    } else {
        title += ": " + std::to_string(albums_.size()) + " ALBUMS";
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
    int total_albums = util::narrow_cast<int>(filtered_album_indices_.size());
    if (total_albums == 0) return;

    // Track old selection for change detection
    int old_selected = selected_index_;

    // Grid navigation (from TOML: nav_right, nav_left, nav_down, nav_up)
    if (matches_keybind(event, "nav_right")) {
        if (selected_index_ < total_albums - 1) selected_index_++;
    }
    else if (matches_keybind(event, "nav_left")) {
        if (selected_index_ > 0) selected_index_--;
    }
    else if (matches_keybind(event, "nav_down")) {
        if (selected_index_ + cols_ < total_albums) selected_index_ += cols_;
    }
    else if (matches_keybind(event, "nav_up")) {
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

    if (matches_keybind(event, "select")) {
        // Lookup actual album from filtered list
        size_t album_idx = filtered_album_indices_[selected_index_];

        // If searching: jump to album in unfiltered view
        if (!filter_query_.empty()) {
            ouroboros::util::Logger::debug("AlbumBrowser: ENTER during search, jumping to album " +
                                          std::to_string(album_idx));
            set_filter("");  // Clear search
            selected_index_ = util::narrow_cast<int>(album_idx);  // Jump to album
            ArtworkWindow::instance().reset();  // Reset artwork for new position
            return;
        }

        // Normal mode: add all tracks in album to queue
        if (!g_current_snapshot || g_current_snapshot->library->tracks.empty()) {
            return;
        }

        auto& bus = events::EventBus::instance();
        const auto& album = albums_[album_idx];

        for (int idx : album.track_indices) {
            if (idx >= 0 && idx < util::narrow_cast<int>(g_current_snapshot->library->tracks.size())) {
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

    // Content Changed - Clear all slots and displayed images if filter changed
    if (content_changed_) {
        ouroboros::util::Logger::debug("AlbumBrowser: Content changed (filter), clearing all");
        // Delete all displayed images
        for (const auto& [key, info] : displayed_images_) {
            img_renderer.delete_image_by_id(info.image_id);
        }
        displayed_images_.clear();
        clear_all_slots();
        force_render = true;
        content_changed_ = false;
    }

    // Check if ArtworkWindow has pending updates (artwork just finished loading)
    bool has_async_updates = artwork_window.has_updates();
    if (has_async_updates) {
        artwork_window.clear_updates();
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
        last_atomic_scroll_offset_ = scroll_offset_;
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
            ouroboros::util::Logger::info("AlbumBrowser: BIG JUMP - clearing all and resetting cache");
            artwork_window.reset();
            // Delete all displayed images
            for (const auto& [key, info] : displayed_images_) {
                img_renderer.delete_image_by_id(info.image_id);
            }
            displayed_images_.clear();
            clear_all_slots();
            force_render = true;
        }

        was_scrolling_ = false;
    }

    // Detect scroll position change for slot reassignment
    bool slots_need_reassign = (last_atomic_scroll_offset_ != scroll_offset_);
    if (slots_need_reassign) {
        last_atomic_scroll_offset_ = scroll_offset_;
    }

    // Debounce: Skip artwork requests if called too rapidly (per-frame spam during scroll)
    if (!force_render && !has_async_updates && (now - last_request_time_) < SCROLL_DEBOUNCE_MS) {
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

    int total_filtered = util::narrow_cast<int>(filtered_album_indices_.size());

    // Calculate selected album's grid position for Manhattan distance calculation
    int selected_row = selected_index_ / cols_available;
    int selected_col = selected_index_ % cols_available;

    // ========================================================================
    // PHASE 1: SLOT ASSIGNMENT + REQUEST (with progressive batching)
    // Assign visible albums to atomic slots, request artwork from ArtworkWindow
    // Progressive loading: limit requests per frame for responsive initial display
    // ========================================================================

    // Progressive batching: limit new requests per frame to prevent overwhelming workers
    // This allows top rows to render while lower rows load in background
    static constexpr int MAX_NEW_REQUESTS_PER_FRAME = 8;
    int new_requests_this_frame = 0;

    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            size_t album_idx = filtered_album_indices_[idx];
            auto& album = albums_[album_idx];

            // Calculate visible row for slot index
            int visible_row = r - start_row;
            size_t slot_idx = get_slot_index(visible_row, c);
            if (slot_idx >= MAX_VISIBLE_SLOTS) continue;

            // Calculate cell position from row/col (no gaps)
            int cell_x = content_x + x_offset + c * cell_w;
            int cell_y = content_y + visible_row * cell_h;

            // Box position - Center in cell
            int box_w = art_cols + 2;
            int box_x = cell_x + (cell_w - box_w) / 2;

            // Artwork area: inside box border
            int art_x = box_x + 1;
            int art_y = cell_y + 1;

            // Assign slot (bumps generation if album changed)
            assign_slot(slot_idx, album.album_directory, art_x, art_y, art_cols, art_rows);

            // Request artwork from ArtworkWindow - but SKIP if slot already Ready or Loading
            // This prevents cascade re-requests and duplicate requests for pending items
            auto& slot = slots_[slot_idx];
            SlotState current_state = slot.state.load(std::memory_order_acquire);
            bool needs_request = (current_state == SlotState::Empty);

            // DEBUG: Log why requests are skipped (only for Loading, not Ready or Failed)
            if (!needs_request && current_state == SlotState::Loading) {
                std::string filename = album.representative_track_path.substr(
                    album.representative_track_path.rfind('/') + 1);
                ouroboros::util::Logger::warn("AlbumBrowser: SKIP slot=" + std::to_string(slot_idx) +
                    " state=Loading album_dir=" + album.album_directory.substr(album.album_directory.rfind('/') + 1) +
                    " track=" + filename);
            }

            if (!album.representative_track_path.empty() && needs_request) {
                // Progressive batching: limit requests per frame
                if (new_requests_this_frame >= MAX_NEW_REQUESTS_PER_FRAME) {
                    ouroboros::util::Logger::debug("AlbumBrowser: BATCH LIMIT slot=" + std::to_string(slot_idx) +
                        " will retry next frame");
                    continue;  // Skip - will be requested in subsequent frames
                }

                // Mark as Loading BEFORE requesting to prevent duplicate requests
                slot.state.store(SlotState::Loading, std::memory_order_release);

                int distance = std::abs(r - selected_row) + std::abs(c - selected_col);
                artwork_window.request(
                    album.representative_track_path,
                    distance,
                    art_cols,
                    art_rows,
                    false  // Batch - don't notify yet
                );
                ++new_requests_this_frame;
            }
        }
    }
    artwork_window.flush_requests();

    // ========================================================================
    // PHASE 2: POPULATE SLOTS
    // Copy decoded artwork from ArtworkWindow to slots (takes mutex briefly)
    // This is the ONLY place we take the ArtworkWindow mutex
    // ========================================================================

    int populated_count = 0;
    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            int visible_row = r - start_row;
            size_t slot_idx = get_slot_index(visible_row, c);
            if (slot_idx >= MAX_VISIBLE_SLOTS) continue;

            auto& slot = slots_[slot_idx];
            SlotState state = slot.state.load(std::memory_order_acquire);

            // Skip if already ready
            if (state == SlotState::Ready) continue;

            size_t album_idx = filtered_album_indices_[idx];
            auto& album = albums_[album_idx];
            if (album.representative_track_path.empty()) continue;

            // Query ArtworkWindow for decoded pixels
            const auto* artwork = artwork_window.get_decoded(album.representative_track_path, art_cols, art_rows);
            if (!artwork) {
                // Check if artwork is marked as Failed (no artwork exists)
                if (state == SlotState::Loading &&
                    artwork_window.is_failed(album.representative_track_path, art_cols, art_rows)) {
                    slot.state.store(SlotState::Failed, std::memory_order_release);
                    ouroboros::util::Logger::debug("AlbumBrowser: Slot " + std::to_string(slot_idx) +
                        " marked Failed - no artwork for " +
                        album.album_directory.substr(album.album_directory.rfind('/') + 1));
                }
                continue;
            }

            // Copy to slot (this is the atomic publish pattern)
            // 1. Copy all data while state is still Empty/Loading
            slot.decoded_pixels.assign(artwork->data, artwork->data + artwork->data_size);
            slot.width = artwork->width;
            slot.height = artwork->height;
            slot.format = artwork->format;
            slot.hash = artwork->hash;

            // 2. Set state to Ready (release) - publishes all prior writes
            slot.state.store(SlotState::Ready, std::memory_order_release);
            populated_count++;
        }
    }

    if (populated_count > 0) {
        ouroboros::util::Logger::debug("AlbumBrowser: Populated " + std::to_string(populated_count) + " slots");
    }

    // ========================================================================
    // PHASE 3: RENDER FROM SLOTS (LOCK-FREE)
    // Read from slots atomically, render to terminal
    // Use displayed_images_ map for proper cleanup (delete AFTER render)
    // ========================================================================

    int ready_count = 0;
    int rendered_count = 0;

    std::unordered_map<std::string, DisplayedImageInfo> new_displayed_images;
    std::unordered_set<uint32_t> active_ids;

    // Build list sorted by distance from selection for radial rendering
    struct RenderItem {
        int visible_row, col, grid_row;
        int distance;
        size_t slot_idx;
    };
    std::vector<RenderItem> render_items;
    render_items.reserve((end_row - start_row + 1) * cols_available);

    for (int r = start_row; r < end_row && r < total_filtered / cols_available + 1; ++r) {
        for (int c = 0; c < cols_available; ++c) {
            int idx = r * cols_available + c;
            if (idx >= total_filtered) continue;

            int visible_row = r - start_row;
            size_t slot_idx = get_slot_index(visible_row, c);
            int distance = std::abs(r - selected_row) + std::abs(c - selected_col);

            render_items.push_back({visible_row, c, r, distance, slot_idx});
        }
    }

    // Sort by distance (radial order from selection)
    std::sort(render_items.begin(), render_items.end(),
              [](const RenderItem& a, const RenderItem& b) { return a.distance < b.distance; });

    // Render in radial order
    for (const auto& item : render_items) {
        if (item.slot_idx >= MAX_VISIBLE_SLOTS) continue;

        auto& slot = slots_[item.slot_idx];

        // Atomic read of state (acquire) - synchronizes with store(release) in populate phase
        SlotState state = slot.state.load(std::memory_order_acquire);
        if (state != SlotState::Ready) continue;
        if (slot.decoded_pixels.empty()) continue;

        ready_count++;

        // Read slot data (safe because state is Ready)
        int art_x = slot.display_x;
        int art_y = slot.display_y;
        int art_cols_slot = slot.display_cols;
        int art_rows_slot = slot.display_rows;

        // Clipping logic
        int art_bottom_y = art_y + art_rows_slot;
        int container_bottom_y = content_y + content_height;
        int visible_art_rows = -1;

        if (art_y >= container_bottom_y) continue; // Fully clipped

        if (art_bottom_y > container_bottom_y) {
            visible_art_rows = container_bottom_y - art_y;
            if (visible_art_rows <= 0) continue;
        }

        // Also check right edge
        if (art_x + art_cols_slot > content_x + content_width) continue;

        // Create display key (position + hash) - same as old approach
        std::string display_key = std::to_string(art_x) + "," + std::to_string(art_y) + "," + slot.hash.substr(0, 16);

        // Check if already displayed at this exact position with same hash
        auto display_it = displayed_images_.find(display_key);
        if (display_it != displayed_images_.end() && display_it->second.hash == slot.hash) {
            // Already displayed - just track it, no re-render needed
            new_displayed_images[display_key] = display_it->second;
            active_ids.insert(display_it->second.image_id);
            continue;
        }

        // Render new image
        uint32_t image_id = img_renderer.render_image(
            slot.decoded_pixels.data(),
            slot.decoded_pixels.size(),
            slot.width,
            slot.height,
            slot.format,
            art_x,
            art_y,
            art_cols_slot,
            art_rows_slot,
            slot.hash,
            visible_art_rows
        );

        if (image_id != 0) {
            new_displayed_images[display_key] = {slot.hash, image_id};
            active_ids.insert(image_id);
            rendered_count++;
        }
    }

    // ========================================================================
    // PHASE 4: CLEANUP - Delete images no longer on screen (AFTER all renders)
    // This is the key to no-flash: render new first, then delete old
    // ========================================================================

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
        ouroboros::util::Logger::debug("AlbumBrowser: CLEANUP deleted " + std::to_string(deleted_count) + " orphaned images");
    }

    // Update displayed images map
    displayed_images_ = std::move(new_displayed_images);

    // ========================================================================
    // PHASE 5: PREFETCH (when idle AND visible slots mostly loaded)
    // Don't prefetch until at least 80% of visible slots are Ready
    // ========================================================================

    auto time_since_scroll = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_scroll_time_
    );

    // Only prefetch when 5% of visible slots are Ready - start prefetch early
    int total_visible = util::narrow_cast<int>(render_items.size());
    bool enough_visible_ready = (total_visible == 0) || (ready_count * 100 / total_visible >= 5);

    if (time_since_scroll >= PREFETCH_DELAY_MS && !prefetch_completed_ && enough_visible_ready) {
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

                int distance = std::abs(r - selected_row) + std::abs(c - selected_col);
                artwork_window.request(
                    album.representative_track_path,
                    distance + 1000,
                    art_cols,
                    art_rows,
                    false
                );
            }
        };

        for (int r = start_row - prefetch_rows_count; r < start_row; ++r) process_prefetch(r);
        for (int r = end_row; r < end_row + prefetch_rows_count; ++r) process_prefetch(r);
        artwork_window.flush_requests();
        prefetch_completed_ = true;
    }

    ouroboros::util::Logger::info("AlbumBrowser: " +
                                  std::to_string(ready_count) + "/" +
                                  std::to_string(render_items.size()) +
                                  " slots ready, " + std::to_string(rendered_count) + " rendered");
}

}  // namespace ouroboros::ui::widgets

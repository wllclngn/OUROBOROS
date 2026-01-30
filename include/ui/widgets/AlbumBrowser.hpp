#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include "ui/ImageRenderer.hpp"  // For CachedFormat
#include <string>
#include <vector>
#include <array>
#include <map>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <chrono>
#include <atomic>

namespace ouroboros::ui::widgets {

// Atomic slot states for flicker-free rendering
enum class SlotState : uint8_t {
    Empty,    // Slot not assigned or cleared
    Loading,  // Request in flight
    Ready,    // Decoded and ready to render
    Failed    // No artwork exists for this album
};

// Atomic slot for album artwork - eliminates mutex contention during render
// Memory ordering: state acts as publication flag
// Writer: fill data, then state.store(Ready, release)
// Reader: if (state.load(acquire) == Ready) read data
struct AlbumBrowserSlot {
    std::atomic<SlotState> state{SlotState::Empty};
    std::atomic<uint64_t> generation{0};  // Bumped on reassignment, rejects stale results

    // Decoded artwork data - only written during Empty->Ready transition
    // Read-only when state is Ready (no mutex needed for reads)
    std::vector<uint8_t> decoded_pixels;
    int width = 0;
    int height = 0;
    CachedFormat format = CachedFormat::RGB;
    std::string hash;
    std::string album_dir;  // Which album this slot represents

    // Terminal display state
    uint32_t image_id = 0;      // Currently rendered image ID
    int display_x = 0;          // Desired position
    int display_y = 0;
    int display_cols = 0;
    int display_rows = 0;

    // Track where image was actually rendered (for position-change detection)
    int rendered_x = -1;
    int rendered_y = -1;
};

struct AlbumGroup {
    std::string title;
    std::string artist;
    std::string year;
    std::vector<int> track_indices; // Indices into the main library vector
    std::string representative_track_path; // First track path for artwork lookup via ArtworkWindow
    std::string album_directory; // Directory containing the album's tracks

    // Pre-computed normalized strings for fast searching (computed once in refresh_cache)
    std::string normalized_title;
    std::string normalized_artist;
};

class AlbumBrowser : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused);

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    void refresh_cache(const model::Snapshot& snap);

    // Render album artwork after Canvas flush (similar to NowPlaying)
    void render_images_if_needed(const LayoutRect& rect, bool force_render);

    // Global Search Interface
    void set_filter(const std::string& query);

    // Clear all album images (call when closing album view)
    void clear_all_images();

private:
    void update_filtered_albums();

    std::vector<AlbumGroup> albums_;
    std::vector<size_t> filtered_album_indices_; // Indices into albums_
    int cols_ = 1;
    int scroll_offset_ = 0;
    int selected_index_ = 0;
    int last_scroll_offset_ = -1;  // Dirty tracking for scroll changes
    int scroll_start_offset_ = 0;  // Where scroll session started (for big jump detection)
    bool was_scrolling_ = false;  // Track if user was actively scrolling (for state transitions)
    std::chrono::steady_clock::time_point scroll_start_time_{};  // When scroll session started

    // Smart prefetch timing: Debounce requests and delay prefetch until scroll idle
    std::chrono::steady_clock::time_point last_scroll_time_{};
    std::chrono::steady_clock::time_point last_request_time_{};
    static constexpr auto SCROLL_DEBOUNCE_MS = std::chrono::milliseconds(35);
    static constexpr auto PREFETCH_DELAY_MS = std::chrono::milliseconds(150);
    static constexpr int BIG_JUMP_ROWS = 10;  // Velocity-based: 10+ rows in under 2s
    static constexpr int HUGE_JUMP_ROWS = 25; // Distance-based: 25+ rows always triggers
    static constexpr auto BIG_JUMP_TIME_LIMIT = std::chrono::milliseconds(2000);

    std::string filter_query_;
    bool filter_dirty_ = false;
    bool content_changed_ = false; // Flag to force clear images on filter change
    bool prefetch_completed_ = false; // Skip redundant prefetch when viewport hasn't changed

    // Atomic slots for flicker-free rendering (holds decoded pixel data)
    // Slots are indexed by visible position: (row - scroll_offset) * cols + col
    static constexpr size_t MAX_VISIBLE_SLOTS = 64;
    std::array<AlbumBrowserSlot, MAX_VISIBLE_SLOTS> slots_;

    // Slot helper methods
    size_t get_slot_index(int visible_row, int col) const;
    AlbumBrowserSlot* get_slot(int visible_row, int col);
    void assign_slot(size_t slot_idx, const std::string& album_dir, int x, int y, int cols, int rows);
    void clear_all_slots();

    // Track rendered images for cleanup (key: "x,y,hash", enables delete-after-render)
    struct DisplayedImageInfo {
        std::string hash;
        uint32_t image_id;
    };
    std::unordered_map<std::string, DisplayedImageInfo> displayed_images_;

    // Last scroll offset for detecting slot reassignment
    int last_atomic_scroll_offset_ = -1;
};

}  // namespace ouroboros::ui::widgets

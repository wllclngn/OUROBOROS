#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <chrono>

namespace ouroboros::ui::widgets {

struct AlbumGroup {
    std::string title;
    std::string artist;
    std::string year;
    std::vector<int> track_indices; // Indices into the main library vector
    std::string representative_track_path; // First track path for artwork lookup via ArtworkLoader
    std::string album_directory; // Directory containing the album's tracks
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

private:
    void update_filtered_albums();

    std::vector<AlbumGroup> albums_;
    std::vector<size_t> filtered_album_indices_; // Indices into albums_
    int cols_ = 1;
    int scroll_offset_ = 0;
    int selected_index_ = 0;
    int last_scroll_offset_ = -1;  // Dirty tracking for scroll changes

    // Smart prefetch timing: Debounce requests and delay prefetch until scroll idle
    std::chrono::steady_clock::time_point last_scroll_time_{};
    std::chrono::steady_clock::time_point last_request_time_{};
    static constexpr auto SCROLL_DEBOUNCE_MS = std::chrono::milliseconds(35);
    static constexpr auto PREFETCH_DELAY_MS = std::chrono::milliseconds(150);

    std::string filter_query_;
    bool filter_dirty_ = false;
    bool content_changed_ = false; // Flag to force clear images on filter change

    // Track which images are currently displayed (for upload deduplication)
    // Key: "x,y,hash_snippet" (unique per position and content)
    struct DisplayedImageInfo {
        std::string hash;
        uint32_t image_id;
    };
    std::unordered_map<std::string, DisplayedImageInfo> displayed_images_;  // Changed to unordered_map for O(1) lookups
};

}  // namespace ouroboros::ui::widgets

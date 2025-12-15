#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

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

private:
    std::vector<AlbumGroup> albums_;
    int cols_ = 1;
    int scroll_offset_ = 0;
    int selected_index_ = 0;
    int last_scroll_offset_ = -1;  // Dirty tracking for scroll changes

    // Track which images are currently displayed (for upload deduplication)
    // Key: "x,y,image_id" Value: SHA-256 hash (for verification)
    std::map<std::string, std::string> displayed_images_;
};

}  // namespace ouroboros::ui::widgets

#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ouroboros::ui::widgets {

class NowPlaying : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    void render_image_if_needed(const LayoutRect& widget_rect, bool force_render = false);

    // Delete the current Kitty placement and reset render tracking.
    // Used when the widget's area is taken over (help overlay, view toggle).
    void clear_image();

    // Full view (Now Playing layout): artwork fills panel height on the left,
    // data column (spectrogram slot, volume, transport, metadata) on the right.
    void set_full_view(bool full) { full_view_ = full; }
    [[nodiscard]] bool full_view() const { return full_view_; }

    // Spectrogram slot reserved at the top of the data column (full view only).
    // Zero-sized until the first full-view render.
    [[nodiscard]] const LayoutRect& spectrogram_rect() const { return spectro_rect_; }

private:
    std::string cached_path_;
    std::string pending_render_path_;  // Track path waiting for decode

    int last_art_x_ = 0;
    int last_art_y_ = 0;
    int last_art_width_ = 0;
    int last_art_height_ = 0;

    // Track the Kitty image ID for selective deletion
    uint32_t last_art_image_id_ = 0;

    // SHA256 hash of last rendered artwork (skip re-render if same album)
    std::string last_rendered_hash_;

    // Force re-render on next frame (set when track changes)
    bool force_next_render_ = false;

    // Cache the actual widget rect for dynamic calculations
    LayoutRect cached_rect_ = {0, 0, 0, 0};

    std::vector<std::string> make_art_box(int width, int height);
    std::vector<std::string> combine_horizontal(
        const std::vector<std::string>& left,
        const std::vector<std::string>& right,
        int left_width,
        int right_width
    );

    // Full view state
    bool full_view_ = false;
    LayoutRect spectro_rect_ = {0, 0, 0, 0};
    int cached_album_total_ = 0;  // track count of current album (computed on track change)

    void render_full(Canvas& canvas, const LayoutRect& content_rect,
                     const model::Snapshot& snap, const model::Track& track);
    // Artwork geometry for the full view: fills content height minus the
    // bottom spectrogram strip, left-anchored, clamped so the data column
    // keeps at least MIN_DATA_COLS columns.
    static void full_art_geometry(int content_width, int content_height,
                                  int& art_cols, int& art_rows);
    static int spectro_strip_height(int content_height);
};

}  // namespace ouroboros::ui::widgets

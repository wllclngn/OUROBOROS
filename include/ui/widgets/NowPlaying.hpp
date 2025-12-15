#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace ouroboros::ui::widgets {

class NowPlaying : public Component {
public:
    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    void render_image_if_needed(const LayoutRect& widget_rect, bool force_render = false);

private:
    std::string cached_path_;

    int last_art_x_ = 0;
    int last_art_y_ = 0;
    int last_art_width_ = 0;
    int last_art_height_ = 0;

    // Cache the actual widget rect for dynamic calculations
    LayoutRect cached_rect_ = {0, 0, 0, 0};

    std::vector<std::string> make_art_box(int width, int height);
    std::vector<std::string> combine_horizontal(
        const std::vector<std::string>& left,
        const std::vector<std::string>& right,
        int left_width,
        int right_width
    );
};

}  // namespace ouroboros::ui::widgets

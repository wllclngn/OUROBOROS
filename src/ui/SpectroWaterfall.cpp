#include "ui/SpectroWaterfall.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/Terminal.hpp"
#include "config/UIConfig.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>

namespace ouroboros::ui {

namespace {
    // Gradient stops: black, deep blue, cyan, amber, white
    struct Stop { float v; uint8_t r, g, b; };
    constexpr Stop STOPS[] = {
        {0.00f,   0,   0,   0},
        {0.25f,  16,  24,  96},
        {0.50f,  32, 160, 192},
        {0.75f, 240, 160,  32},
        {1.00f, 255, 255, 255},
    };

    // Lower-eighth blocks for the no-graphics fallback
    const char* const EIGHTHS[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
}

void SpectroWaterfall::color_for(float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    v = std::clamp(v, 0.0f, 1.0f);
    for (size_t i = 1; i < std::size(STOPS); ++i) {
        if (v <= STOPS[i].v) {
            float t = (v - STOPS[i - 1].v) / (STOPS[i].v - STOPS[i - 1].v);
            r = static_cast<uint8_t>(STOPS[i - 1].r + t * (STOPS[i].r - STOPS[i - 1].r));
            g = static_cast<uint8_t>(STOPS[i - 1].g + t * (STOPS[i].g - STOPS[i - 1].g));
            b = static_cast<uint8_t>(STOPS[i - 1].b + t * (STOPS[i].b - STOPS[i - 1].b));
            return;
        }
    }
    r = g = b = 255;
}

void SpectroWaterfall::resize(int width_px, int height_px) {
    if (width_px == w_ && height_px == h_) return;
    w_ = std::max(width_px, 0);
    h_ = std::max(height_px, 0);
    rgba_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
    // Opaque black baseline
    for (size_t i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
}

void SpectroWaterfall::clear() {
    std::fill(rgba_.begin(), rgba_.end(), 0);
    for (size_t i = 3; i < rgba_.size(); i += 4) rgba_[i] = 255;
    std::fill(last_bins_.begin(), last_bins_.end(), 0.0f);
}

void SpectroWaterfall::push_column(const float* bins, size_t nbins) {
    if (w_ <= 0 || h_ <= 0 || !bins || nbins == 0) return;

    last_bins_.assign(bins, bins + nbins);

    // Scroll left one pixel, row by row
    size_t row_bytes = static_cast<size_t>(w_) * 4;
    for (int y = 0; y < h_; ++y) {
        uint8_t* row = rgba_.data() + static_cast<size_t>(y) * row_bytes;
        std::memmove(row, row + 4, row_bytes - 4);
    }

    // Newest column on the right edge; low frequency at the bottom
    for (int y = 0; y < h_; ++y) {
        size_t bin = (static_cast<size_t>(h_ - 1 - y) * nbins) / static_cast<size_t>(h_);
        if (bin >= nbins) bin = nbins - 1;

        uint8_t r, g, b;
        color_for(bins[bin], r, g, b);

        uint8_t* px = rgba_.data() + static_cast<size_t>(y) * row_bytes + (w_ - 1) * 4;
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = 255;
    }
    ++frame_;
}

uint32_t SpectroWaterfall::emit(int cell_x, int cell_y, int cell_w, int cell_h) {
    if (w_ <= 0 || h_ <= 0) return 0;

    auto& img = ImageRenderer::instance();
    if (!img.images_supported()) return 0;

    if (img.get_protocol() == ImageProtocol::Kitty) {
        // montauk chart pattern: re-transmit under ONE fixed id every frame.
        // Kitty replaces the image data and placement in place — flicker-free,
        // no id churn, no stranded placements, and hide() is a single delete.
        std::string b64_path = img.write_to_temp_file(rgba_.data(), rgba_.size());
        if (b64_path.empty()) return 0;

        static bool in_tmux = (std::getenv("TMUX") != nullptr);

        auto& term = Terminal::instance();
        term.move_cursor(cell_x, cell_y);

        std::ostringstream ss;
        if (in_tmux) ss << "\033Ptmux;\033\033_G";
        else         ss << "\033_G";
        ss << "a=T,t=t,f=32,i=" << KITTY_IMAGE_ID
           << ",s=" << w_ << ",v=" << h_
           << ",c=" << cell_w << ",r=" << cell_h
           << ",q=2,z=1,C=1;" << b64_path;
        if (in_tmux) ss << "\033\033\\\033\\";
        else         ss << "\033\\";
        term.write_raw(ss.str());

        placed_ = true;
        return KITTY_IMAGE_ID;
    }

    // Sixel/iTerm2: no id concept; full re-emit each frame, text overdraws
    // on view exit. Rotating hash defeats ImageRenderer's position dedupe.
    std::string hash = std::format("fe{:014x}", frame_ & 0xFFFFFFFFFFFFF);
    return img.render_image(
        rgba_.data(), rgba_.size(),
        w_, h_,
        CachedFormat::RGBA,
        cell_x, cell_y,
        cell_w, cell_h,
        hash
    );
}

void SpectroWaterfall::hide() {
    if (!placed_) return;
    placed_ = false;

    // Uppercase d=I: drop the placement AND free the stored image data
    // (lowercase would keep the pixels in Kitty's store; at one transmit per
    // frame that store grows unbounded)
    static bool in_tmux = (std::getenv("TMUX") != nullptr);
    std::ostringstream ss;
    if (in_tmux) ss << "\033Ptmux;\033\033_G";
    else         ss << "\033_G";
    ss << "a=d,d=I,i=" << KITTY_IMAGE_ID << ",q=2";
    if (in_tmux) ss << "\033\033\\\033\\";
    else         ss << "\033\\";
    Terminal::instance().write_raw(ss.str());

    util::Logger::debug("SpectroWaterfall: Hidden (fixed id deleted, data freed)");
}

void SpectroWaterfall::render_blocks(Canvas& canvas, const LayoutRect& rect) const {
    if (rect.width <= 0 || rect.height <= 0 || last_bins_.empty()) return;

    const auto& uc = config::ui_config();
    size_t nbins = last_bins_.size();

    // Current spectrum as vertical bars: one column per cell, low freq left
    for (int col = 0; col < rect.width; ++col) {
        size_t bin = (static_cast<size_t>(col) * nbins) / static_cast<size_t>(rect.width);
        float v = std::clamp(last_bins_[bin], 0.0f, 1.0f);

        // Total bar height in eighths over rect.height rows
        int eighths = static_cast<int>(v * static_cast<float>(rect.height) * 8.0f + 0.5f);
        for (int row = 0; row < rect.height; ++row) {
            int y = rect.y + rect.height - 1 - row;
            int row_eighths = std::clamp(eighths - row * 8, 0, 8);
            canvas.draw_text(rect.x + col, y, EIGHTHS[row_eighths], uc.nowplaying_info);
        }
    }
}

}  // namespace ouroboros::ui

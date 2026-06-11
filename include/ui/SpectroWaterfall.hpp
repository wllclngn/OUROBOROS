#pragma once

#include "ui/Canvas.hpp"
#include "ui/LayoutConstraints.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ouroboros::ui {

// Scrolling waterfall spectrogram rasterizer (montauk chart pattern: persistent
// RGBA buffer, full emit per frame via the /dev/shm path transport).
// Time scrolls left; the newest FFT column lands on the right edge. Frequency
// runs vertically (low at bottom), magnitude [0,1] maps to a fixed gradient.
//
// Emission rides ImageRenderer (Kitty f=32 + /dev/shm, Sixel/iTerm2 fallback).
// Per-frame updates rotate the content hash and delete the prior placement
// right before rendering the new one (NowPlaying's flash-free discipline) —
// ImageRenderer's position dedupe would otherwise skip same-hash re-renders.
// No graphics protocol: render_blocks() draws the current spectrum as
// block-character bars instead.
class SpectroWaterfall {
public:
    // Resize the pixel buffer (clears history). No-op if unchanged.
    void resize(int width_px, int height_px);

    // Scroll left one pixel and write bins (normalized [0,1], low frequency
    // first) into the rightmost column.
    void push_column(const float* bins, size_t nbins);

    // Emit the buffer at the given cell rect. Returns the image id (0 when
    // no graphics protocol is active or emission failed).
    uint32_t emit(int cell_x, int cell_y, int cell_w, int cell_h);

    // Delete the current placement (view toggled off, help overlay, stop).
    void hide();

    // Clear pixel history (track change / stop).
    void clear();

    // Block-character fallback: draw the latest column's spectrum as bars.
    void render_blocks(Canvas& canvas, const LayoutRect& rect) const;

    [[nodiscard]] int width_px() const { return w_; }
    [[nodiscard]] int height_px() const { return h_; }

private:
    std::vector<uint8_t> rgba_;     // row-major, w_ * h_ * 4
    std::vector<float> last_bins_;  // latest column for the block fallback
    int w_ = 0;
    int h_ = 0;
    bool placed_ = false;           // a Kitty placement currently exists
    uint64_t frame_ = 0;

    // Fixed Kitty image id (montauk chart pattern): every emit re-transmits
    // under this id, replacing data and placement in place — exactly one
    // image ever exists. Outside the 32-bit hash-derived artwork id space
    // by convention ("fe" namespace).
    static constexpr uint32_t KITTY_IMAGE_ID = 0xFE000001;

    static void color_for(float v, uint8_t& r, uint8_t& g, uint8_t& b);
};

}  // namespace ouroboros::ui

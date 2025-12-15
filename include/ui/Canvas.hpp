#pragma once

#include "ui/Color.hpp"
#include <vector>
#include <string>
#include <string_view>

namespace ouroboros::ui {

/**
 * A single cell on the terminal grid.
 * Represents what is visually displayed at one coordinate.
 */
struct Cell {
    std::string content = " "; // UTF-8 grapheme (usually 1-4 bytes)
    Style style;

    bool operator==(const Cell& other) const {
        return content == other.content && style == other.style;
    }
    
    bool operator!=(const Cell& other) const {
        return !(*this == other);
    }
};

/**
 * A 2D grid of Cells representing a rendering surface.
 * Origin (0,0) is top-left.
 */
class Canvas {
public:
    Canvas(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    // Direct access (O(1))
    Cell& at(int x, int y);
    const Cell& at(int x, int y) const;

    // Drawing primitives
    void clear(const Cell& fill_cell = Cell{" ", {}});
    void put(int x, int y, const std::string& grapheme, Style style = {});
    
    // Advanced drawing
    // returns the x-coordinate after the last character drawn
    int draw_text(int x, int y, std::string_view text, Style style = {});
    
    // Draw a box/rect
    void draw_rect(int x, int y, int w, int h, Style style = {});
    
    // Fill a rect
    void fill_rect(int x, int y, int w, int h, const Cell& cell);

    // Blit (copy) another canvas onto this one
    void blit(const Canvas& source, int dest_x, int dest_y);
    void blit(const Canvas& source, int dest_x, int dest_y, int src_x, int src_y, int w, int h);

    // Resize the canvas (clears content)
    void resize(int width, int height);

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<Cell> buffer_;

    bool is_in_bounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }
};

} // namespace ouroboros::ui

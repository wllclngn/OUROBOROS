#include "ui/Canvas.hpp"
#include <algorithm>
#include <stdexcept>
#include <wchar.h>

namespace ouroboros::ui {

// Helper: decode UTF-8 character to wchar_t, return bytes consumed (0 on error)
static int utf8_to_wchar(const char* s, size_t len, wchar_t* out) {
    if (len == 0 || !s) return 0;
    unsigned char c = s[0];

    if ((c & 0x80) == 0) {
        *out = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0 && len >= 2) {
        *out = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    } else if ((c & 0xF0) == 0xE0 && len >= 3) {
        *out = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    } else if ((c & 0xF8) == 0xF0 && len >= 4) {
        *out = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;
}

Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    buffer_.resize(width * height);
}

Cell& Canvas::at(int x, int y) {
    if (!is_in_bounds(x, y)) {
        static Cell dummy;
        return dummy; 
    }
    return buffer_[y * width_ + x];
}

const Cell& Canvas::at(int x, int y) const {
    if (!is_in_bounds(x, y)) {
        static Cell dummy;
        return dummy;
    }
    return buffer_[y * width_ + x];
}

void Canvas::clear(const Cell& fill_cell) {
    std::fill(buffer_.begin(), buffer_.end(), fill_cell);
}

void Canvas::put(int x, int y, const std::string& grapheme, Style style) {
    if (is_in_bounds(x, y)) {
        buffer_[y * width_ + x] = Cell{grapheme, style};
    }
}

void Canvas::resize(int width, int height) {
    if (width_ == width && height_ == height) return;
    width_ = width;
    height_ = height;
    buffer_.clear();
    buffer_.resize(width * height);
}

// Simple UTF-8 decoding helper to iterate characters
// This is a naive implementation; a full grapheme cluster library is ideal 
// but we stick to standard C++23 constraints and simple UTF-8.
static size_t utf8_char_len(char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // Invalid or continuation, treat as 1 to advance
}

// Helper to parse integer from string
static int parse_int(const char*& p) {
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

int Canvas::draw_text(int x, int y, std::string_view text, Style initial_style) {
    if (y < 0 || y >= height_) return x;

    int current_x = x;
    Style current_style = initial_style;
    
    size_t i = 0;
    while (i < text.length()) {
        if (current_x >= width_) break;

        // Check for ANSI escape sequence
        if (text[i] == '\033' && i + 1 < text.length() && text[i+1] == '[') {
            const char* p = text.data() + i + 2; // Skip \033[
            
            // Parse parameters
            while (true) {
                int code = parse_int(p);
                
                // Apply SGR code to current_style
                switch (code) {
                    case 0: // Reset
                        current_style = initial_style; // Or default? Let's reset to passed-in base
                        current_style = Style{}; // Reset to truly default
                        break;
                    case 1: // Bold
                        current_style.attr = static_cast<Attribute>(static_cast<uint8_t>(current_style.attr) | static_cast<uint8_t>(Attribute::Bold));
                        break;
                    case 2: // Dim
                        current_style.attr = static_cast<Attribute>(static_cast<uint8_t>(current_style.attr) | static_cast<uint8_t>(Attribute::Dim));
                        break;
                    case 30: current_style.fg = Color::Black; break;
                    case 31: current_style.fg = Color::Red; break;
                    case 32: current_style.fg = Color::Green; break;
                    case 33: current_style.fg = Color::Yellow; break;
                    case 34: current_style.fg = Color::Blue; break;
                    case 35: current_style.fg = Color::Magenta; break;
                    case 36: current_style.fg = Color::Cyan; break;
                    case 37: current_style.fg = Color::White; break;
                    case 39: current_style.fg = Color::Default; break;
                }
                
                if (*p == ';') {
                    p++; // Continue to next param
                } else {
                    break; // End of sequence (usually 'm')
                }
            }
            
            // Skip until 'm' or end
            while (*p && *p != 'm') p++;
            if (*p == 'm') p++;
            
            // Update index
            i = p - text.data();
            continue; // Don't draw anything for the escape code
        }

        size_t len = utf8_char_len(text[i]);
        if (i + len > text.length()) break; // Incomplete char

        std::string grapheme(text.substr(i, len));

        // Get display width using wcwidth
        wchar_t wc;
        int char_width = 1;
        if (utf8_to_wchar(text.data() + i, len, &wc) > 0) {
            int w = wcwidth(wc);
            if (w > 0) char_width = w;
        }

        if (current_x >= 0 && current_x < width_) {
            put(current_x, y, grapheme, current_style);

            // For double-width characters, mark the next cell as continuation
            if (char_width == 2 && current_x + 1 < width_) {
                put(current_x + 1, y, "", current_style);  // Empty continuation cell
            }
        }

        current_x += char_width;
        i += len;
    }
    return current_x;
}

void Canvas::draw_rect(int x, int y, int w, int h, Style style) {
    if (w <= 0 || h <= 0) return;

    // Corners
    put(x, y, "┌", style);
    put(x + w - 1, y, "┐", style);
    put(x, y + h - 1, "└", style);
    put(x + w - 1, y + h - 1, "┘", style);

    // Horizontal
    for (int i = 1; i < w - 1; ++i) {
        put(x + i, y, "─", style);
        put(x + i, y + h - 1, "─", style);
    }

    // Vertical
    for (int i = 1; i < h - 1; ++i) {
        put(x, y + i, "│", style);
        put(x + w - 1, y + i, "│", style);
    }
}

void Canvas::fill_rect(int x, int y, int w, int h, const Cell& cell) {
    for (int cy = y; cy < y + h; ++cy) {
        for (int cx = x; cx < x + w; ++cx) {
            if (is_in_bounds(cx, cy)) {
                buffer_[cy * width_ + cx] = cell;
            }
        }
    }
}

void Canvas::blit(const Canvas& source, int dest_x, int dest_y) {
    blit(source, dest_x, dest_y, 0, 0, source.width(), source.height());
}

void Canvas::blit(const Canvas& source, int dest_x, int dest_y, int src_x, int src_y, int w, int h) {
    for (int dy = 0; dy < h; ++dy) {
        for (int dx = 0; dx < w; ++dx) {
            int sx = src_x + dx;
            int sy = src_y + dy;
            int tx = dest_x + dx;
            int ty = dest_y + dy;

            if (is_in_bounds(tx, ty)) {
                // In C++23 we can access source safely if we trust caller or add bounds check
                // For performance in inner loop, we do minimal checks
                if (sx >= 0 && sx < source.width() && sy >= 0 && sy < source.height()) {
                    buffer_[ty * width_ + tx] = source.at(sx, sy);
                }
            }
        }
    }
}

} // namespace ouroboros::ui

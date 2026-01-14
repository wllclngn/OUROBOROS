#include "ui/Formatting.hpp"
#include <algorithm>
#include <iostream>
#include <wchar.h>

namespace ouroboros::ui {

// Helper: decode UTF-8 character to wchar_t, return bytes consumed (0 on error)
static int utf8_to_wchar(const char* s, size_t len, wchar_t* out) {
    if (len == 0 || !s) return 0;
    unsigned char c = s[0];

    if ((c & 0x80) == 0) {
        // ASCII
        *out = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0 && len >= 2) {
        // 2-byte
        *out = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    } else if ((c & 0xF0) == 0xE0 && len >= 3) {
        // 3-byte (includes CJK)
        *out = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    } else if ((c & 0xF8) == 0xF0 && len >= 4) {
        // 4-byte
        *out = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    return 0;  // Invalid
}

int display_cols(const std::string& s) {
    int cols = 0;
    for (size_t i = 0; i < s.size(); ) {
        // ANSI Escape Sequence Handling
        if (s[i] == '\x1B') {
            // Case 1: CSI sequences (e.g., colors) - \x1B[...m
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && (s[i] < '@' || s[i] > '~')) {
                    i++;
                }
                if (i < s.size()) i++; // Skip final byte
                continue;
            }
            // Case 2: APC/Kitty sequences - \x1B_ ... \x1B\ (ESC + backslash)
            else if (i + 1 < s.size() && s[i + 1] == '_') {
                i += 2;
                // Skip until ST (String Terminator): \x1B\ (0x1B 0x5C)
                while (i + 1 < s.size()) {
                    if (s[i] == '\x1B' && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }

        // Decode UTF-8 and use wcwidth for proper display width
        wchar_t wc;
        int len = utf8_to_wchar(s.c_str() + i, s.size() - i, &wc);
        if (len > 0) {
            int w = wcwidth(wc);
            cols += (w > 0) ? w : 1;  // wcwidth returns -1 for non-printable, use 1
            i += len;
        } else {
            // Invalid UTF-8, skip byte
            cols++;
            i++;
        }
    }
    return cols;
}

std::string take_cols(const std::string& s, int cols) {
    if (cols <= 0) return "";

    std::string out;
    out.reserve(s.size());
    int seen = 0;
    size_t i = 0;

    while (i < s.size() && seen < cols) {
        // ANSI Escape Sequence Handling
        if (s[i] == '\x1B') {
            size_t start = i;
            bool handled = false;

            // CSI
            if (i + 1 < s.size() && s[i + 1] == '[') {
                i += 2;
                while (i < s.size() && (s[i] < '@' || s[i] > '~')) {
                    i++;
                }
                if (i < s.size()) i++;
                out.append(s, start, i - start);
                handled = true;
            }
            // APC/Kitty
            else if (i + 1 < s.size() && s[i + 1] == '_') {
                i += 2;
                while (i + 1 < s.size()) {
                    if (s[i] == '\x1B' && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                out.append(s, start, i - start);
                handled = true;
            }

            if (handled) continue;
        }

        // Decode UTF-8 and check display width
        wchar_t wc;
        int len = utf8_to_wchar(s.c_str() + i, s.size() - i, &wc);
        if (len > 0) {
            int w = wcwidth(wc);
            if (w < 0) w = 1;  // Non-printable, assume 1

            // Don't exceed requested columns (important for wide chars)
            if (seen + w > cols) break;

            out.append(s, i, len);
            i += len;
            seen += w;
        } else {
            // Invalid UTF-8, skip byte
            out += s[i];
            i++;
            seen++;
        }
    }

    return out;
}

std::string trunc_pad(const std::string& s, int w) {
    if (w <= 0) return "";
    
    int cols = display_cols(s);
    
    if (cols == w) {
        return s; // Perfect fit
    }
    
    if (cols < w) {
        // Pad with spaces
        return s + std::string(w - cols, ' ');
    }
    
    // Truncate with ellipsis
    if (w <= 1) {
        return take_cols(s, w);
    }
    
    return take_cols(s, w - 1) + "…";
}

std::string rpad_trunc(const std::string& s, int w) {
    if (w <= 0) return "";
    
    int cols = display_cols(s);
    
    if (cols == w) return s;
    if (cols < w) return std::string(w - cols, ' ') + s;
    
    return take_cols(s, w);
}

std::string lr_align(int width, const std::string& left, const std::string& right) {
    if (width <= 0) return "";
    
    int rvis = display_cols(right);
    int left_max = width - rvis - 1; // Leave space for at least 1 space between
    if (left_max < 0) left_max = 0;
    
    std::string l = trunc_pad(left, left_max);
    int lvis = display_cols(l);
    
    int space = width - lvis - rvis;
    if (space < 0) space = 0;
    
    return l + std::string(space, ' ') + right;
}

std::vector<std::string> make_box(
    const std::string& title,
    const std::vector<std::string>& lines,
    int width,
    int min_height
) {
    std::vector<std::string> out;
    
    if (width < 3) {
        // Too narrow for a box
        return std::vector<std::string>(std::max(1, min_height), "");
    }
    
    int inner_width = width - 2; // Minus left and right border
    
    // Top border with title
    std::string top = "┌─ " + title + " ";
    int title_len = display_cols(top);
    int fill = width - title_len - 1;
    if (fill > 0) {
        for (int i = 0; i < fill; ++i) {
            top += "─";
        }
    }
    top += "┐";
    out.push_back(top);
    
    // Content lines
    int content_lines = std::max((int)lines.size(), min_height);
    for (int i = 0; i < content_lines; ++i) {
        std::string content = (i < (int)lines.size()) ? lines[i] : "";
        out.push_back("│" + trunc_pad(content, inner_width) + "│");
    }
    
    // Bottom border
    std::string bottom = "└";
    for (int i = 0; i < inner_width; ++i) {
        bottom += "─";
    }
    bottom += "┘";
    out.push_back(bottom);
    
    return out;
}

} // namespace ouroboros::ui
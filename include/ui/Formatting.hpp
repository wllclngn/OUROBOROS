#pragma once

#include <string>
#include <vector>

namespace ouroboros::ui {

/**
 * Calculate the display width of a string, accounting for:
 * - ANSI escape sequences (which don't take visual space)
 * - UTF-8 multi-byte characters (each counts as 1 display column)
 */
int display_cols(const std::string& s);

/**
 * Truncate a string to fit exactly `width` display columns.
 * Handles ANSI codes and UTF-8 correctly.
 */
std::string take_cols(const std::string& s, int width);

/**
 * Truncate string if too long, pad with spaces if too short.
 * Result will be exactly `width` display columns.
 */
std::string trunc_pad(const std::string& s, int width);

/**
 * Right-pad a string (spaces on left side).
 */
std::string rpad_trunc(const std::string& s, int width);

/**
 * Align left text and right text with space between.
 * Example: lr_align(40, "CPU", "75%") -> "CPU                                 75%"
 */
std::string lr_align(int width, const std::string& left, const std::string& right);

/**
 * Create a box with title and content lines.
 * Content will be padded/truncated to fill exactly `min_height` lines.
 * 
 * Example:
 *   ╭─[ PROCESSOR ]────────╮
 *   │ CPU [████░░░░] 40%   │
 *   ╰──────────────────────╯
 */
std::vector<std::string> make_box(
    const std::string& title,
    const std::vector<std::string>& lines,
    int width,
    int min_height = 0
);

} // namespace ouroboros::ui

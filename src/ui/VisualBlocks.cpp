#include "ui/VisualBlocks.hpp"
#include <algorithm>

namespace ouroboros::ui::blocks {

std::string bar_chart(int pct, int width) {
    pct = std::clamp(pct, 0, 100);
    if (width <= 0) return "";

    // Horizontal fill in eighths of a cell: left-eighth blocks for the
    // fractional cell (sub-cell progress stays visible between cell jumps)
    int total_eighths = pct * width * 8 / 100;
    int filled = total_eighths / 8;
    int partial = total_eighths % 8;
    const char* lefts[] = {"▏", "▎", "▍", "▌", "▋", "▊", "▉"};

    std::string result;
    for (int i = 0; i < filled; i++) result += "█";
    if (filled < width) {
        result += (partial > 0) ? lefts[partial - 1] : "░";
    }
    for (int i = filled + 1; i < width; i++) result += "░";
    return result;
}

}  // namespace ouroboros::ui::blocks

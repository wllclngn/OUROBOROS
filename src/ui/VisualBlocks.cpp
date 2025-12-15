#include "ui/VisualBlocks.hpp"
#include <algorithm>
#include <vector>

namespace ouroboros::ui::blocks {

std::string percentage_to_block_8(int pct) {
    const char* blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int level = std::clamp(pct * 8 / 100, 0, 7);
    return blocks[level];
}

std::string bar_chart(int pct, int width) {
    int filled = pct * width / 100;
    int partial = (pct * width % 100) * 8 / 100;
    const char* blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

    std::string result;
    for (int i = 0; i < filled; i++) result += "█";
    if (filled < width) result += blocks[partial];
    for (int i = filled + 1; i < width; i++) result += "░";
    return result;
}

std::string sparkline(const std::vector<int>& values, int width) {
    const char* blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    std::string result;
    int max_val = 0;
    for (auto v : values) max_val = std::max(max_val, v);

    int i = 0;
    for (auto v : values) {
        if (i >= width) break;
        int level = max_val > 0 ? v * 8 / max_val : 0;
        result += blocks[level];
        i++;
    }
    return result;
}

std::string cycle_animation(int frame, int width) {
    const char* shades[] = {"░", "▒", "▓", "█"};
    std::string result;
    for (int i = 0; i < width; ++i) {
        int block_idx = (i + frame) % 4;
        result += shades[block_idx];
    }
    return result;
}

}  // namespace ouroboros::ui::blocks

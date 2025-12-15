#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ouroboros::ui::blocks {

constexpr std::string_view BLOCKS_8 = "▁▂▃▄▅▆▇█";
constexpr std::string_view SHADES = "░▒▓█";

std::string percentage_to_block_8(int pct);
std::string bar_chart(int pct, int width);
std::string sparkline(const std::vector<int>& values, int width);
std::string cycle_animation(int frame, int width);

}  // namespace ouroboros::ui::blocks

#pragma once

#include <string>

namespace ouroboros::ui::blocks {

// Horizontal fill bar with eighth-cell resolution. pct in [0,100], width in cells.
std::string bar_chart(int pct, int width);

}  // namespace ouroboros::ui::blocks

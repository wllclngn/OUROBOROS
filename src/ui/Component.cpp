#include "ui/Component.hpp"
#include "util/Platform.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace ouroboros::ui {

// Helper method implementations moved from inline to reduce duplication

std::string Component::format_duration(int total_seconds) const {
    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    std::ostringstream oss;
    if (hours > 0) {
        oss << hours << ":"
            << std::setw(2) << std::setfill('0') << minutes << ":"
            << std::setw(2) << std::setfill('0') << seconds;
    } else {
        oss << minutes << ":"
            << std::setw(2) << std::setfill('0') << seconds;
    }
    return oss.str();
}

}  // namespace ouroboros::ui

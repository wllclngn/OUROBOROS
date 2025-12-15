#include "ui/Component.hpp"
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

std::string Component::format_track_display(
    const model::Track& track,
    int max_width,
    bool show_duration
) const {
    // Format: "Artist - Title"
    std::string display;

    if (!track.artist.empty() && !track.title.empty()) {
        display = track.artist + " - " + track.title;
    } else if (!track.title.empty()) {
        display = track.title;
    } else if (!track.artist.empty()) {
        display = track.artist;
    } else {
        // Fallback to filename
        size_t last_slash = track.path.find_last_of('/');
        display = (last_slash != std::string::npos)
            ? track.path.substr(last_slash + 1)
            : track.path;
    }

    // Add duration if requested and space available
    if (show_duration && track.duration_ms > 0) {
        int duration_seconds = track.duration_ms / 1000;
        std::string duration_str = " [" + format_duration(duration_seconds) + "]";
        int available_width = max_width - static_cast<int>(duration_str.length());

        if (available_width > 10) {  // Ensure minimum space for track info
            std::string truncated = truncate_text(display, available_width);
            return truncated + duration_str;
        }
    }

    return truncate_text(display, max_width);
}

std::string Component::format_track_metadata_line(
    const model::Track& track,
    bool include_album
) const {
    std::ostringstream oss;

    if (!track.artist.empty()) {
        oss << track.artist;
    }

    if (include_album && !track.album.empty()) {
        if (oss.tellp() > 0) oss << " - ";
        oss << track.album;
    }

    if (!track.date.empty()) {
        if (oss.tellp() > 0) oss << " ";
        oss << "(" << track.date << ")";
    }

    return oss.str();
}

std::string Component::center_text(const std::string& text, int width) const {
    if (static_cast<int>(text.length()) >= width) {
        return text.substr(0, width);
    }

    int padding = (width - static_cast<int>(text.length())) / 2;
    return std::string(padding, ' ') + text;
}

std::string Component::pad_right(const std::string& text, int width) const {
    if (static_cast<int>(text.length()) >= width) {
        return text.substr(0, width);
    }

    return text + std::string(width - text.length(), ' ');
}

std::string Component::format_filesize(size_t bytes) const {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 3) {
        size /= 1024.0;
        unit_index++;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << size << " " << units[unit_index];
    return oss.str();
}

}  // namespace ouroboros::ui

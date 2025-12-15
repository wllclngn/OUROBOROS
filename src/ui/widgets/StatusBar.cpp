#include "ui/widgets/StatusBar.hpp"
#include "ui/Formatting.hpp"
#include "ui/VisualBlocks.hpp"
#include "config/Theme.hpp"
#include <chrono>
#include <sstream>
#include <iomanip>

namespace ouroboros::ui::widgets {

void StatusBar::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    auto theme = config::ThemeManager::get_theme("terminal");

    // Check for recent alerts
    if (!snap.alerts.empty()) {
        const auto& last_alert = snap.alerts.back();
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - last_alert.timestamp).count();

        if (age < 5) {
            Style alert_style;
            if (last_alert.level == "error") {
                alert_style = Style{Color::Red, Color::Default, Attribute::Bold};
            } else {
                alert_style = Style{Color::Yellow, Color::Default, Attribute::Bold};
            }
            std::string msg = "ALERT: " + last_alert.message;
            canvas.draw_text(rect.x, rect.y, truncate_text(msg, rect.width), alert_style);
            return;
        }
    }

    // BUILD STATUS LINE: LEFT SIDE (status) + RIGHT SIDE (help)

    // LEFT SIDE: Status info
    std::ostringstream left;

    left << "[" << snap.library->tracks.size() << " tracks] │ ";

    // Playback state
    const char* state_icon = "■";
    if (snap.player.state == model::PlaybackState::Playing) {
        state_icon = "▶";
    } else if (snap.player.state == model::PlaybackState::Paused) {
        state_icon = "⏸";
    }
    left << state_icon << " ";

    // Progress bar and time
    int position_pct = 0;
    std::string time_str = "0:00 / 0:00";
    if (snap.player.current_track_index.has_value()) {
        // Resolve track index to actual Track via Library
        int track_idx = snap.player.current_track_index.value();
        if (track_idx >= 0 && track_idx < static_cast<int>(snap.library->tracks.size())) {
            const auto& track = snap.library->tracks[track_idx];
            if (track.duration_ms > 0) {
                position_pct = (snap.player.playback_position_ms * 100) / track.duration_ms;
            }
            int pos_sec = snap.player.playback_position_ms / 1000;
            int dur_sec = track.duration_ms / 1000;
            time_str = format_duration(pos_sec) + " / " + format_duration(dur_sec);
        }
    }

    auto progress_bar = ui::blocks::bar_chart(position_pct, 18);
    left << progress_bar << " " << time_str;

    // Volume
    left << " │ Vol: ";
    auto volume_bar = ui::blocks::bar_chart(snap.player.volume_percent, 10);
    left << volume_bar << " " << snap.player.volume_percent << "%";

    // Repeat mode
    left << " │ REPEAT: ";
    switch (snap.player.repeat_mode) {
        case model::RepeatMode::Off:  left << "OFF"; break;
        case model::RepeatMode::One:  left << "ONE"; break;
        case model::RepeatMode::All:  left << "ALL"; break;
    }

    // Draw left side (contains ANSI codes from bar charts)
    canvas.draw_text(rect.x, rect.y, left.str(), Style{});

    // RIGHT SIDE: Help indicator
    std::string help_indicator = "HELP: ?";
    int help_x = rect.x + rect.width - static_cast<int>(help_indicator.length());
    if (help_x > rect.x) {  // Only draw if there's space
        canvas.draw_text(help_x, rect.y, help_indicator,
                        Style{Color::BrightWhite, Color::Default, Attribute::Bold});
    }
}

void StatusBar::handle_input(const InputEvent&) {
}

SizeConstraints StatusBar::get_constraints() const {
    // StatusBar is always exactly 1 line tall
    SizeConstraints constraints;
    constraints.min_height = 1;
    constraints.max_height = 1;
    return constraints;
}

}  // namespace ouroboros::ui::widgets

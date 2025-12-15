#include "ui/widgets/NowPlaying.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/ArtworkLoader.hpp"
#include "ui/VisualBlocks.hpp"
#include "config/Theme.hpp"
#include <sstream>
#include <fstream>
#include <iomanip>
#include "util/Logger.hpp"

namespace ouroboros::ui::widgets {

void NowPlaying::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    auto theme = config::ThemeManager::get_theme("terminal");
    // Cache the actual rect for dynamic calculations
    cached_rect_ = rect;

    // Draw border with title
    auto content_rect = draw_box_border(canvas, rect, "NOW PLAYING");

    // Check if we have a current track
    if (!snap.player.current_track_index.has_value()) {
        ouroboros::util::Logger::debug("NowPlaying: No track currently playing");
        return;  // Keep UI clean - no placeholder text
    }

    // Resolve track index to actual Track via Library
    int track_idx = snap.player.current_track_index.value();
    if (track_idx < 0 || track_idx >= static_cast<int>(snap.library->tracks.size())) {
        canvas.draw_text(content_rect.x + 2, content_rect.y + 2, "(invalid track index)",
                        Style{Color::Default, Color::Default, Attribute::Dim});
        return;
    }

    const auto& track = snap.library->tracks[track_idx];

    // Check if track changed
    bool track_changed = (cached_path_ != track.path);

    if (track_changed) {
        cached_path_ = track.path;

        auto& img_renderer = ImageRenderer::instance();
        if (img_renderer.images_supported()) {
            // Clear previous artwork
            // if (last_art_width_ > 0 && last_art_height_ > 0) {
            //    img_renderer.clear_image(last_art_x_, last_art_y_, last_art_width_, last_art_height_);
            // }

            // Request async artwork loading (non-blocking)
            auto& loader = ArtworkLoader::instance();
            loader.request_artwork(track.path);
        }
    }

    // Artwork loading happens in background - ArtworkLoader owns the cache
    // We just query it during render_image_if_needed()

    // FIX: Don't draw placeholder - it causes flush_canvas to output spaces
    // that clear Kitty graphics when the placeholder disappears
    // Just leave the area blank if no image is available

    // Calculate layout: artwork takes most space, metadata + statusline at bottom
    // Reserve 3 lines at bottom for track info, format, and statusline
    int metadata_lines = 3;

    // Render track metadata at bottom of content area
    int y = content_rect.y + content_rect.height - metadata_lines;

    // Track info line: ARTIST ALBUM YEAR TRACK_NUMBER SONG (multi-color like Browser)
    int x = content_rect.x + 1;
    int line_y = y++;
    int remaining_w = content_rect.width - 1;

    // Helper to draw and advance
    auto draw_part = [&](const std::string& text, Style s) {
        if (remaining_w <= 0) return;
        std::string t = truncate_text(text, remaining_w);
        canvas.draw_text(x, line_y, t, s);
        int len = display_cols(t);
        x += len;
        remaining_w -= len;
    };

    // Artist (Cyan)
    draw_part(!track.artist.empty() ? track.artist : "Unknown Artist",
             Style{Color::Cyan, Color::Default, Attribute::None});

    // Separator bullet
    draw_part(" • ", Style{Color::Cyan, Color::Default, Attribute::Dim});

    // Album (Default)
    if (!track.album.empty()) {
        draw_part(track.album, Style{Color::Default, Color::Default, Attribute::None});
        draw_part(" • ", Style{Color::Cyan, Color::Default, Attribute::Dim});
    }

    // Year (Default)
    if (!track.date.empty()) {
        draw_part(track.date, Style{Color::Default, Color::Default, Attribute::None});
        draw_part(" • ", Style{Color::Cyan, Color::Default, Attribute::Dim});
    }

    // Track number (Dim)
    if (track.track_number > 0) {
        std::ostringstream num_oss;
        num_oss << std::setfill('0') << std::setw(2) << track.track_number;
        draw_part(num_oss.str(), Style{Color::Default, Color::Default, Attribute::Dim});
        draw_part(" • ", Style{Color::Cyan, Color::Default, Attribute::Dim});
    }

    // Title (BrightWhite)
    draw_part(!track.title.empty() ? track.title : "Untitled",
             Style{Color::BrightWhite, Color::Default, Attribute::None});

    // Format info
    std::ostringstream format_line;

    // Format type
    switch (track.format) {
        case model::AudioFormat::MP3:  format_line << "MP3"; break;
        case model::AudioFormat::FLAC: format_line << "FLAC"; break;
        case model::AudioFormat::OGG:  format_line << "OGG"; break;
        case model::AudioFormat::WAV:  format_line << "WAV"; break;
        default: format_line << "Unknown"; break;
    }

    // Sample rate
    if (track.sample_rate > 0) {
        format_line << " • " << (track.sample_rate / 1000) << "kHz";
    }

    // Bitrate
    if (track.bitrate > 0) {
        format_line << " • " << track.bitrate << "kbps";
    }

    // Bit depth
    if (track.bit_depth > 0) {
        format_line << " " << track.bit_depth << "bit";
    }

    // Channels
    if (track.channels == 2) {
        format_line << " Stereo";
    } else if (track.channels == 1) {
        format_line << " Mono";
    }

    std::string format_str = format_line.str();
    if (!format_str.empty()) {
        canvas.draw_text(content_rect.x + 1, y++, truncate_text(format_str, content_rect.width - 1),
                        Style{Color::Cyan, Color::Default, Attribute::Dim});
    }

    // STATUSLINE: Playback info + progress + volume + repeat
    int statusline_x = content_rect.x + 1;
    int statusline_y = y++;
    int statusline_remaining = content_rect.width - 1;

    auto draw_status_part = [&](const std::string& text, Style s) {
        if (statusline_remaining <= 0) return;
        std::string t = truncate_text(text, statusline_remaining);
        canvas.draw_text(statusline_x, statusline_y, t, s);
        int len = display_cols(t);
        statusline_x += len;
        statusline_remaining -= len;
    };

    // Playback state icon
    const char* state_icon = "■";
    if (snap.player.state == model::PlaybackState::Playing) {
        state_icon = "▶";
    } else if (snap.player.state == model::PlaybackState::Paused) {
        state_icon = "⏸";
    }
    draw_status_part(std::string(state_icon) + " ", Style{});

    // Progress bar and time
    int position_pct = 0;
    if (track.duration_ms > 0) {
        position_pct = (snap.player.playback_position_ms * 100) / track.duration_ms;
    }
    int pos_sec = snap.player.playback_position_ms / 1000;
    int dur_sec = track.duration_ms / 1000;
    std::string time_str = format_duration(pos_sec) + " / " + format_duration(dur_sec);

    auto progress_bar = ui::blocks::bar_chart(position_pct, 12);
    draw_status_part(progress_bar + " " + time_str, Style{});

    // Separator bullet
    draw_status_part(" • ", Style{Color::Cyan, Color::Default, Attribute::Dim});

    // Volume
    draw_status_part("Vol. ", Style{});
    auto volume_bar = ui::blocks::bar_chart(snap.player.volume_percent, 8);
    draw_status_part(volume_bar, Style{});

    // Separator bullet
    draw_status_part(" • ", Style{Color::Cyan, Color::Default, Attribute::Dim});

    // Repeat mode
    draw_status_part("REPEAT: ", Style{});
    std::string repeat_str;
    switch (snap.player.repeat_mode) {
        case model::RepeatMode::Off:  repeat_str = "OFF"; break;
        case model::RepeatMode::One:  repeat_str = "ONE"; break;
        case model::RepeatMode::All:  repeat_str = "ALL"; break;
    }
    draw_status_part(repeat_str, Style{});
}

void NowPlaying::render_image_if_needed(const LayoutRect& widget_rect, bool force_render) {
    ouroboros::util::Logger::debug("NowPlaying: render_image_if_needed called for: " + cached_path_);

    // Query ArtworkLoader directly (zero-copy)
    auto& loader = ArtworkLoader::instance();
    const auto* artwork = loader.get_artwork_ref(cached_path_);

    if (!artwork) {
        ouroboros::util::Logger::debug("NowPlaying: No artwork ref for " + cached_path_);
        return;
    }
    
    if (!artwork->loaded) {
        ouroboros::util::Logger::debug("NowPlaying: Artwork not loaded for " + cached_path_);
        return;
    }

    // Skip if we already rendered this image (unless forcing)
    static std::string last_rendered_path;
    if (!force_render && cached_path_ == last_rendered_path) {
        return;  // Already rendered
    }

    ouroboros::util::Logger::debug("NowPlaying: Attempting render. Path=" + cached_path_ + " DataSize=" + std::to_string(artwork->jpeg_data.size()));

    auto& img_renderer = ImageRenderer::instance();
    if (!img_renderer.images_supported()) {
        ouroboros::util::Logger::debug("NowPlaying: Images not supported");
        return;
    }

    // DYNAMIC CALCULATION: Use actual widget dimensions from container
    // draw_box_border uses 1 cell for border on each side
    int content_x = widget_rect.x + 1;
    int content_y = widget_rect.y + 1;
    int content_width = widget_rect.width - 2;   // Subtract left+right borders
    int content_height = widget_rect.height - 2;  // Subtract top+bottom borders

    // Reserve 3 lines at bottom for metadata (track info, format, statusline)
    int metadata_lines = 3;
    int available_artwork_height = content_height - metadata_lines;

    // Calculate artwork dimensions - balanced padding on both sides
    int art_cols = content_width - 2;  // Reserve 1 cell padding each side
    int art_rows = art_cols / 2;       // Maintain aspect

    // Constrain by available height if needed
    if (art_rows > available_artwork_height) {
        art_rows = available_artwork_height;
        // Don't recalculate art_cols - keep balanced padding
    }

    // Ensure even columns for proper image scaling
    if (art_cols % 2 != 0) art_cols--;

    // Position artwork - 1 cell left/right padding, no top padding
    int art_x = content_x + 1;
    int art_y = content_y;

    // Safety bounds (minimum viable display)
    if (art_cols < 4) art_cols = 4;
    if (art_rows < 2) art_rows = 2;

    ouroboros::util::Logger::debug("NowPlaying: Calling render_image. X=" + std::to_string(art_x) + " Y=" + std::to_string(art_y) + " Cols=" + std::to_string(art_cols) + " Rows=" + std::to_string(art_rows));

    bool success = img_renderer.render_image(
        artwork->jpeg_data,
        art_x,
        art_y,
        art_cols,
        art_rows
    );
    
    ouroboros::util::Logger::debug("NowPlaying: render_image returned " + std::string(success ? "TRUE" : "FALSE"));

    if (success) {
        last_art_x_ = art_x;
        last_art_y_ = art_y;
        last_art_width_ = art_cols;
        last_art_height_ = art_rows;

        // Track what we rendered (static var declared at top of function)
        last_rendered_path = cached_path_;
    }
}

void NowPlaying::handle_input(const InputEvent&) {
}

SizeConstraints NowPlaying::get_constraints() const {
    // NowPlaying needs minimum space for artwork + metadata + statusline
    // Minimum: 20 rows for artwork (10 cols × 2) + 3 rows (track info + format + statusline) + 2 border = 25 total
    SizeConstraints constraints;
    constraints.min_height = 25;
    return constraints;
}

}  // namespace ouroboros::ui::widgets

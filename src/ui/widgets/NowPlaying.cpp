#include "ui/widgets/NowPlaying.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/ArtworkWindow.hpp"
#include "ui/VisualBlocks.hpp"
#include "config/UIConfig.hpp"
#include "util/Logger.hpp"
#include "util/Platform.hpp"
#include <sstream>
#include <fstream>
#include <iomanip>
#include <utility>

namespace ouroboros::ui::widgets {

namespace {
// Base codec name for a track (DSD encodes its rate multiple, e.g. "DSD2").
// One source for the three places NowPlaying prints the format.
std::string codec_name(const model::Track& track) {
    switch (track.format) {
        case model::AudioFormat::MP3:  return "MP3";
        case model::AudioFormat::FLAC: return "FLAC";
        case model::AudioFormat::OGG:  return "OGG";
        case model::AudioFormat::WAV:  return "WAV";
        case model::AudioFormat::M4A:  return "M4A";
        case model::AudioFormat::DSD:  return "DSD" + std::to_string(track.sample_rate / 44100);
        default:                       return "Unknown";
    }
}
}  // namespace

void NowPlaying::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    const auto& uc = config::ui_config();
    // Cache the actual rect for dynamic calculations
    cached_rect_ = rect;

    // Draw border with title
    auto content_rect = draw_box_border(canvas, rect, "NOW PLAYING");

    // Check if we have a current track
    if (!snap.player.current_track_index.has_value()) {
        ouroboros::util::Logger::debug("NowPlaying: No track currently playing");
        clear_image();
        return;
    }

    // Resolve track index to actual Track via Library
    int track_idx = snap.player.current_track_index.value();
    if (track_idx < 0 || track_idx >= util::narrow_cast<int>(snap.library->tracks.size())) {
        canvas.draw_text(content_rect.x + 2, content_rect.y + 2, "(invalid track index)",
                        uc.muted);
        return;
    }

    const auto& track = snap.library->tracks[track_idx];

    // Check if track changed
    bool track_changed = (cached_path_ != track.path);

    if (track_changed) {
        ouroboros::util::Logger::info("NowPlaying: Track changed - path=" + track.path);
        cached_path_ = track.path;

        // Reset render tracking so new artwork will be displayed
        force_next_render_ = true;

        // NOTE: Don't delete the old image here. Keep it visible until new artwork
        // is ready to render. Deletion happens in render_image_if_needed() right
        // before rendering the new image, preventing the flash of empty space.

        // Album track count for the full view's TRACK row (nn / total)
        cached_album_total_ = 0;
        auto slash = track.path.rfind('/');
        if (slash != std::string::npos && snap.library) {
            std::string track_dir = track.path.substr(0, slash);
            for (const auto& group : snap.library->albums) {
                if (group.album_directory == track_dir) {
                    cached_album_total_ = util::narrow_cast<int>(group.track_indices.size());
                    break;
                }
            }
        }
    }

    if (full_view_) {
        render_full(canvas, content_rect, snap, track);
        return;
    }

    // Prepare Format info early to determine layout
    std::ostringstream format_line;
    format_line << codec_name(track);

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

    // Artwork geometry (shared with render_image_if_needed so they never drift)
    int content_width = content_rect.width;
    int content_height = content_rect.height;
    int art_cols = 0, art_rows = 0, horizontal_padding = 0;
    compact_art_geometry(content_width, content_height, art_cols, art_rows, horizontal_padding);

    // Position text anchored to BOTTOM
    // This ensures consistent bottom padding (0) and keeps statusline fixed
    int lines_needed = 2 + (!format_str.empty() ? 1 : 0);
    int y = content_rect.y + content_rect.height - lines_needed;

    // Track info line: ARTIST ALBUM YEAR TRACK_NUMBER SONG (multi-color like Browser)
    // Align text with artwork left edge for symmetry
    int x = content_rect.x + horizontal_padding;
    int line_y = y++;
    int remaining_w = art_cols;  // Text width matches artwork width

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
             uc.artist);

    // Separator bullet
    draw_part(" • ", uc.separator);

    // Track number (Dim)
    if (track.track_number > 0) {
        std::ostringstream num_oss;
        num_oss << std::setfill('0') << std::setw(2) << track.track_number;
        draw_part(num_oss.str(), uc.muted);
        draw_part(" • ", uc.separator);
    }

    // Title with quotes (BrightWhite)
    draw_part("\"" + (!track.title.empty() ? track.title : "Untitled") + "\"",
             uc.title);

    // Separator bullet
    draw_part(" • ", uc.separator);

    // Year with parentheses (Default)
    if (!track.date.empty()) {
        draw_part("(" + track.date + ")", uc.album);
        draw_part(" • ", uc.separator);
    }

    // Album (Default)
    if (!track.album.empty()) {
        draw_part(track.album, uc.album);
    }

    // Format info (multi-color: data in nowplaying_info, bullets in separator)
    if (!format_str.empty()) {
        int fmt_x = content_rect.x + horizontal_padding;
        int fmt_y = y++;
        int fmt_remaining = art_cols;

        auto draw_fmt = [&](const std::string& text, Style s) {
            if (fmt_remaining <= 0) return;
            std::string t = truncate_text(text, fmt_remaining);
            canvas.draw_text(fmt_x, fmt_y, t, s);
            int len = display_cols(t);
            fmt_x += len;
            fmt_remaining -= len;
        };

        // Codec
        draw_fmt(codec_name(track), uc.nowplaying_info);
        if (track.sample_rate > 0) {
            draw_fmt(" • ", uc.separator);
            draw_fmt(std::to_string(track.sample_rate / 1000) + "kHz", uc.nowplaying_info);
        }
        if (track.bitrate > 0) {
            draw_fmt(" • ", uc.separator);
            draw_fmt(std::to_string(track.bitrate) + "kbps", uc.nowplaying_info);
        }
        if (track.bit_depth > 0) {
            draw_fmt(" " + std::to_string(track.bit_depth) + "bit", uc.nowplaying_info);
        }
        if (track.channels == 2) {
            draw_fmt(" Stereo", uc.nowplaying_info);
        } else if (track.channels == 1) {
            draw_fmt(" Mono", uc.nowplaying_info);
        }
    }

    // STATUSLINE: Playback info + progress + volume + repeat
    int statusline_x = content_rect.x + horizontal_padding;
    int statusline_y = y++;
    int statusline_remaining = art_cols;

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
    draw_status_part(std::string(state_icon) + " ", uc.nowplaying_info);

    // Progress bar and time
    int position_pct = 0;
    if (track.duration_ms > 0) {
        position_pct = (snap.player.playback_position_ms * 100) / track.duration_ms;
    }
    int pos_sec = snap.player.playback_position_ms / 1000;
    int dur_sec = track.duration_ms / 1000;
    std::string time_str = format_duration(pos_sec) + " / " + format_duration(dur_sec);

    auto progress_bar = ui::blocks::bar_chart(position_pct, 12);
    draw_status_part(progress_bar + " " + time_str, uc.nowplaying_info);

    // Separator bullet
    draw_status_part(" • ", uc.separator);

    // Volume
    draw_status_part("Vol. ", uc.nowplaying_info);
    auto volume_bar = ui::blocks::bar_chart(snap.player.volume_percent, 8);
    draw_status_part(volume_bar, uc.nowplaying_info);

    // Separator bullet
    draw_status_part(" • ", uc.separator);

    // Repeat mode
    draw_status_part("REPEAT: ", uc.nowplaying_info);
    std::string repeat_str;
    switch (snap.player.repeat_mode) {
        case model::RepeatMode::Off:  repeat_str = "OFF"; break;
        case model::RepeatMode::One:  repeat_str = "ONE"; break;
        case model::RepeatMode::All:  repeat_str = "ALL"; break;
        default: std::unreachable();
    }
    draw_status_part(repeat_str, uc.nowplaying_info);

    // Separator bullet
    draw_status_part(" • ", uc.separator);

    // Shuffle mode
    draw_status_part("SHUFFLE: ", uc.nowplaying_info);
    draw_status_part(snap.player.shuffle_enabled ? "ON" : "OFF", uc.nowplaying_info);
}

void NowPlaying::compact_art_geometry(int content_width, int content_height,
                                      int& art_cols, int& art_rows,
                                      int& horizontal_padding) {
    // Reserve 3 lines at the bottom for track info, format and statusline.
    constexpr int metadata_lines = 3;
    int available_artwork_height = content_height - metadata_lines;

    // Start from the width constraint (1 col padding each side), square it,
    // then cap by height; keep (width - cols) even so padding stays symmetric.
    art_cols = content_width - 2;
    if (art_cols < 0) art_cols = 0;
    art_rows = art_cols / 2;

    if (art_rows > available_artwork_height) {
        art_rows = available_artwork_height;
        art_cols = art_rows * 2;
        if ((content_width - art_cols) % 2 != 0) {
            if (art_cols + 1 <= content_width - 2) {
                art_cols++;
            } else {
                art_cols--;
            }
        }
    }

    if (art_cols < 4) art_cols = 4;
    art_rows = art_cols / 2;

    horizontal_padding = (content_width - art_cols) / 2;
}

void NowPlaying::full_art_geometry(int content_width, int content_height,
                                   int& art_cols, int& art_rows) {
    // Artwork fills the panel height minus the spectrogram strip reserved
    // along the bottom; width-clamped so the data column keeps room to breathe
    constexpr int MIN_DATA_COLS = 26;
    constexpr int ART_LEFT_PAD = 1;
    constexpr int COLUMN_GUTTER = 2;

    art_rows = content_height - spectro_strip_height(content_height);
    art_cols = art_rows * 2;

    int max_art = content_width - MIN_DATA_COLS - ART_LEFT_PAD - COLUMN_GUTTER;
    if (art_cols > max_art) art_cols = max_art;
    if (art_cols < 4) art_cols = 4;
    if (art_cols % 2 != 0) art_cols--;
    art_rows = art_cols / 2;
}

int NowPlaying::spectro_strip_height(int content_height) {
    // Bottom strip below the artwork: a quarter of the panel, at least 4 rows.
    // Enough for the full-width waterfall to have presence without starving the
    // album artwork of vertical space.
    return std::max(4, content_height / 4);
}

void NowPlaying::render_full(Canvas& canvas, const LayoutRect& content_rect,
                             const model::Snapshot& snap, const model::Track& track) {
    const auto& uc = config::ui_config();

    int art_cols = 0, art_rows = 0;
    full_art_geometry(content_rect.width, content_rect.height, art_cols, art_rows);

    // Data column right of artwork: 1 col left pad + art + 2 col gutter
    int col_x = content_rect.x + 1 + art_cols + 2;
    int col_w = content_rect.x + content_rect.width - col_x - 1;
    if (col_w < 10) return;  // degenerate terminal; artwork alone

    int y = content_rect.y;

    auto draw_row = [&](int row_y, const std::string& text, Style s) {
        canvas.draw_text(col_x, row_y, truncate_text(text, col_w), s);
    };

    // Stacked metadata: label on its own line, value flush beneath it
    auto meta_row = [&](const std::string& label, const std::string& value, Style value_style) {
        if (y + 1 >= content_rect.y + content_rect.height) return;
        canvas.draw_text(col_x, y++, truncate_text(label + ":", col_w), uc.muted);
        canvas.draw_text(col_x, y++, truncate_text(value, col_w), value_style);
    };

    meta_row("SONG", !track.title.empty() ? track.title : "Untitled", uc.title);

    {
        std::ostringstream trk;
        trk << std::setfill('0') << std::setw(2) << track.track_number;
        if (cached_album_total_ > 0) {
            trk << " / " << std::setfill('0') << std::setw(2) << cached_album_total_;
        }
        if (track.track_number > 0) {
            meta_row("TRACK", trk.str(), uc.nowplaying_info);
        }
    }

    if (!track.album.empty())  meta_row("ALBUM", track.album, uc.album);
    meta_row("ARTIST", !track.artist.empty() ? track.artist : "Unknown Artist", uc.artist);
    if (!track.date.empty())   meta_row("YEAR", track.date, uc.album);
    if (!track.genre.empty())  meta_row("GENRE", track.genre, uc.nowplaying_info);

    // Format: CODEC (format, sample rate, bit depth, channels), BITRATE
    {
        std::ostringstream codec;
        codec << codec_name(track);
        if (track.sample_rate > 0) codec << " " << (track.sample_rate / 1000) << "kHz";
        if (track.bit_depth > 0)   codec << " " << track.bit_depth << "bit";
        if (track.channels == 2)   codec << " Stereo";
        else if (track.channels == 1) codec << " Mono";
        meta_row("CODEC", codec.str(), uc.nowplaying_info);
    }

    if (track.bitrate > 0) {
        meta_row("BITRATE", std::to_string(track.bitrate) + "kbps", uc.nowplaying_info);
    }

    y++;  // blank before the playback block

    // Playback modes, one compact line just above the progress bar (playback state,
    // not track metadata). Clamped to col_w so it never bleeds into the queue.
    if (y < content_rect.y + content_rect.height) {
        const char* repeat_str = "OFF";
        switch (snap.player.repeat_mode) {
            case model::RepeatMode::Off:  repeat_str = "OFF"; break;
            case model::RepeatMode::One:  repeat_str = "ONE"; break;
            case model::RepeatMode::All:  repeat_str = "ALL"; break;
            default: std::unreachable();
        }
        std::string modes = std::string("REPEAT: ") + repeat_str +
                            "   SHUFFLE: " + (snap.player.shuffle_enabled ? "ON" : "OFF");
        draw_row(y++, modes, uc.nowplaying_info);
    }

    // PROGRESS: bar, then state icon with times
    if (y + 2 < content_rect.y + content_rect.height) {
        draw_row(y++, "PROGRESS:", uc.muted);

        int position_pct = 0;
        if (track.duration_ms > 0) {
            position_pct = (snap.player.playback_position_ms * 100) / track.duration_ms;
        }
        draw_row(y++, ui::blocks::bar_chart(position_pct, col_w), uc.nowplaying_info);

        const char* state_icon = "■";
        if (snap.player.state == model::PlaybackState::Playing) {
            state_icon = "▶";
        } else if (snap.player.state == model::PlaybackState::Paused) {
            state_icon = "⏸";
        }
        int pos_sec = snap.player.playback_position_ms / 1000;
        int dur_sec = track.duration_ms / 1000;
        draw_row(y++, std::string(state_icon) + " " + format_duration(pos_sec) +
                          " / " + format_duration(dur_sec),
                 uc.nowplaying_info);
    }

    // VOLUME: bar
    if (y + 1 < content_rect.y + content_rect.height) {
        draw_row(y++, "VOLUME:", uc.muted);
        draw_row(y++, ui::blocks::bar_chart(snap.player.volume_percent, col_w),
                 uc.nowplaying_info);
    }

    // Spectrogram strip: full panel width, from the artwork's bottom edge to
    // the panel's end. Pushed down if the metadata column runs longer than
    // the artwork. Rendered by the spectrogram pipeline; nothing drawn here.
    int art_bottom = content_rect.y + art_rows;
    int spectro_top = std::max(art_bottom + 1, y + 1);  // 1-row gap below artwork, matching panel padding
    int spectro_h = content_rect.y + content_rect.height - spectro_top;
    if (spectro_h >= 3) {
        spectro_rect_ = {content_rect.x + 1, spectro_top,
                         content_rect.width - 2, spectro_h};
    } else {
        spectro_rect_ = {0, 0, 0, 0};  // panel too short; pipeline skips
    }
}

void NowPlaying::render_image_if_needed(const LayoutRect& widget_rect, bool /*force_render*/) {
    ouroboros::util::Logger::debug("NowPlaying: render_image_if_needed called for: " + cached_path_);

    if (cached_path_.empty()) {
        ouroboros::util::Logger::debug("NowPlaying: No cached path");
        return;
    }

    auto& img_renderer = ImageRenderer::instance();
    if (!img_renderer.images_supported()) {
        ouroboros::util::Logger::debug("NowPlaying: Images not supported");
        return;
    }

    // DYNAMIC CALCULATION: Use actual widget dimensions from container
    // draw_box_border uses 1 cell for border on each side
    int content_x = widget_rect.x + 1;
    int content_y = widget_rect.y + 1;
    int content_width = widget_rect.width - 2;
    int content_height = widget_rect.height - 2;

    int art_cols = 0;
    int art_rows = 0;
    int art_x = 0;
    int art_y = content_y;
    int total_padding = 0;
    int horizontal_padding = 0;

    if (full_view_) {
        // FULL VIEW: artwork fills panel height, left-anchored
        full_art_geometry(content_width, content_height, art_cols, art_rows);
        art_x = content_x + 1;
    } else {
        compact_art_geometry(content_width, content_height, art_cols, art_rows, horizontal_padding);
        total_padding = content_width - art_cols;
        art_x = content_x + horizontal_padding;
    }

    // Request artwork from ArtworkWindow with priority 0 (currently playing track)
    // Use force_extract=true to get per-track artwork (important for podcasts/mixes)
    auto& artwork_window = ArtworkWindow::instance();
    artwork_window.request(cached_path_, 0, art_cols, art_rows, true, true);

    // Query ArtworkWindow for decoded pixels
    const auto* artwork = artwork_window.get_decoded(cached_path_, art_cols, art_rows);

    if (!artwork) {
        ouroboros::util::Logger::debug("NowPlaying: Artwork not ready for " + cached_path_);
        pending_render_path_ = cached_path_;
        return;
    }

    // Skip if we already rendered this exact image at this exact position/size
    // Compare by SHA256 hash, not path - tracks with identical artwork skip re-render
    bool same_image = (artwork->hash == last_rendered_hash_);
    bool same_position = (art_x == last_art_x_ && art_y == last_art_y_);
    bool same_size = (art_cols == last_art_width_ && art_rows == last_art_height_);

    if (same_image && same_position && same_size && last_art_image_id_ != 0) {
        // Image is already correctly displayed, no action needed
        ouroboros::util::Logger::debug("NowPlaying: Skipping re-render (same SHA256 hash)");
        force_next_render_ = false;
        return;
    }

    // Clear the force flag now that we're rendering
    force_next_render_ = false;

    ouroboros::util::Logger::debug("NowPlaying: Attempting render. Path=" + cached_path_ +
                                  " DataSize=" + std::to_string(artwork->data_size));

    ouroboros::util::Logger::debug("NowPlaying::render_image_if_needed - content_width=" + std::to_string(content_width) +
                                   " art_cols=" + std::to_string(art_cols) +
                                   " total_padding=" + std::to_string(total_padding) +
                                   " horizontal_padding=" + std::to_string(horizontal_padding) +
                                   " LEFT=" + std::to_string(horizontal_padding) +
                                   " RIGHT=" + std::to_string(total_padding - horizontal_padding));
    ouroboros::util::Logger::debug("NowPlaying: Calling render_image. X=" + std::to_string(art_x) +
                                  " Y=" + std::to_string(art_y) +
                                  " Cols=" + std::to_string(art_cols) +
                                  " Rows=" + std::to_string(art_rows));

    // Delete old image RIGHT BEFORE rendering new one (not earlier)
    // This prevents the flash of empty space when track changes
    if (last_art_image_id_ != 0) {
        img_renderer.delete_image_by_id(last_art_image_id_);
        last_art_image_id_ = 0;
    }

    // Use modified hash to get unique image ID for NowPlaying
    // This prevents ID collision with AlbumBrowser showing the same artwork
    // ImageRenderer uses first 16 hex chars for ID, so prepend "ff" to shift it
    std::string nowplaying_hash = "ff" + artwork->hash;

    uint32_t image_id = img_renderer.render_image(
        artwork->data,
        artwork->data_size,
        artwork->width,
        artwork->height,
        artwork->format,
        art_x,
        art_y,
        art_cols,
        art_rows,
        nowplaying_hash
    );
    bool success = (image_id != 0);

    ouroboros::util::Logger::debug("NowPlaying: render_image returned " +
                                  std::string(success ? "TRUE" : "FALSE") +
                                  " image_id=" + std::to_string(image_id));

    if (success) {
        last_art_x_ = art_x;
        last_art_y_ = art_y;
        last_art_width_ = art_cols;
        last_art_height_ = art_rows;
        last_art_image_id_ = image_id;
        last_rendered_hash_ = artwork->hash;
        pending_render_path_.clear();
    } else {
        pending_render_path_ = cached_path_;
    }
}

void NowPlaying::clear_image() {
    if (last_art_image_id_ != 0) {
        auto& img_renderer = ImageRenderer::instance();
        img_renderer.delete_image_by_id(last_art_image_id_);
        last_art_image_id_ = 0;
        cached_path_.clear();
        last_rendered_hash_.clear();
        ouroboros::util::Logger::debug("NowPlaying: Cleared artwork");
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

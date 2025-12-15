#include "ui/Renderer.hpp"
#include "ui/Terminal.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/FlexLayout.hpp"
#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include <algorithm>
#include <fstream>

namespace ouroboros::ui {

// Layout constants
namespace {
    constexpr int MIN_TERMINAL_COLS = 60;
    constexpr int MIN_TERMINAL_ROWS = 15;
    constexpr int MIN_CONTENT_HEIGHT = 10;
    constexpr int MIN_BROWSER_WIDTH = 40;
    constexpr int MIN_RIGHT_COLUMN_WIDTH = 30;
    constexpr int COLUMN_GUTTER = 1;

    // FlexLayout growth ratios for right column (NowPlaying vs Queue)
    constexpr float NOWPLAYING_FLEX_RATIO = 0.6f;  // ~60% of right column
    constexpr float QUEUE_FLEX_RATIO = 0.4f;       // ~40% of right column
}

Renderer::Renderer(std::shared_ptr<backend::SnapshotPublisher> publisher)
    : publisher_(publisher),
      canvas_(1, 1),
      prev_canvas_(1, 1) {
    // Create widgets directly
    header_ = std::make_unique<widgets::NowPlaying>();
    browser_ = std::make_unique<widgets::Browser>();
    queue_ = std::make_unique<widgets::Queue>();
    controls_ = std::make_unique<widgets::Controls>();
    status_bar_ = std::make_unique<widgets::StatusBar>();
    album_browser_ = std::make_unique<widgets::AlbumBrowser>();
    help_overlay_ = std::make_unique<widgets::HelpOverlay>();

    // Initialize image renderer
    auto& img = ImageRenderer::instance();
    img.detect_protocol();
}

Renderer::~Renderer() = default;

void Renderer::compute_layout(int cols, int rows) {
    // FLEXLAYOUT-BASED: Declarative, constraint-driven layout

    // Content area now fills the entire screen (statusline moved to NowPlaying widget)
    LayoutRect content_area = {0, 0, cols, rows};

    // 2. HORIZONTAL SPLIT: [Browser] + [Right Column]
    FlexLayout horizontal_layout;
    horizontal_layout.set_direction(FlexDirection::Row);
    horizontal_layout.set_spacing(COLUMN_GUTTER);

    // Browser: flexible, prefers more space
    LayoutConstraints browser_constraints;
    if (show_album_view_) {
        browser_constraints.size = album_browser_->get_constraints();
    } else {
        browser_constraints.size = browser_->get_constraints();
    }
    browser_constraints.size.min_width = MIN_BROWSER_WIDTH;
    browser_constraints.flex.flex_grow = 1.0f;

    // Right column: flexible
    LayoutConstraints right_constraints;
    right_constraints.size.min_width = MIN_RIGHT_COLUMN_WIDTH;
    right_constraints.flex.flex_grow = 1.0f;

    horizontal_layout.add_item(nullptr, browser_constraints);  // Placeholder for browser
    horizontal_layout.add_item(nullptr, right_constraints);     // Placeholder for right column

    auto horiz_rects = horizontal_layout.compute_layout(content_area.width, content_area.height);

    browser_rect_.x = content_area.x + horiz_rects[0].x;
    browser_rect_.y = content_area.y + horiz_rects[0].y;
    browser_rect_.width = horiz_rects[0].width;
    browser_rect_.height = horiz_rects[0].height;

    LayoutRect right_area;
    right_area.x = content_area.x + horiz_rects[1].x;
    right_area.y = content_area.y + horiz_rects[1].y;
    right_area.width = horiz_rects[1].width;
    right_area.height = horiz_rects[1].height;

    // 3. VERTICAL SPLIT (Right Column): [NowPlaying] + [Queue]
    // ARTWORK-CENTRIC LAYOUT:
    
    int right_width = right_area.width;
    int right_height = right_area.height;
    
    // NowPlaying internal layout constants
    constexpr int NP_BORDER_H = 2; // Top + Bottom border
    constexpr int NP_BORDER_W = 2; // Left + Right border
    constexpr int NP_METADATA_H = 5; // Artist, Album, Title, Format, Statusline
    
    // Reserve space for Queue
    constexpr int MIN_QUEUE_H = 5;
    int max_art_height_avail = right_height - NP_METADATA_H - NP_BORDER_H - MIN_QUEUE_H;
    
    // Calculate optimal dimensions (Cols = 2 * Rows)
    int avail_width_cells = right_width - NP_BORDER_W;
    
    int art_cols = std::min(avail_width_cells, max_art_height_avail * 2);
    
    // Ensure even width for cleaner centering
    if (art_cols % 2 != 0) art_cols--;
    
    // Ensure minimum size
    if (art_cols < 10) art_cols = 10; // Minimum 10x5 artwork
    
    int art_rows = art_cols / 2;
    
    // Calculate total NowPlaying height
    int np_height = art_rows + NP_METADATA_H + NP_BORDER_H;
    
    // Safety clamp
    if (np_height > right_height - MIN_QUEUE_H) {
        np_height = right_height - MIN_QUEUE_H;
    }
    
    header_rect_.x = right_area.x;
    header_rect_.y = right_area.y;
    header_rect_.width = right_width;
    header_rect_.height = np_height;

    queue_rect_.x = right_area.x;
    queue_rect_.y = right_area.y + np_height;
    queue_rect_.width = right_width;
    queue_rect_.height = right_height - np_height;
}

void Renderer::flush_canvas() {
    auto& terminal = Terminal::instance();

    // OPTIMIZATION: Only update changed cells
    for (int y = 0; y < canvas_.height(); ++y) {
        for (int x = 0; x < canvas_.width(); ++x) {
            const auto& cell = canvas_.at(x, y);
            const auto& prev = prev_canvas_.at(x, y);

            if (cell != prev) {
                terminal.move_cursor(x, y);

                // Convert Style to ANSI codes and print
                std::string output;

                // Foreground color
                if (cell.style.fg != Color::Default) {
                    int fg_code = 30 + (static_cast<int>(cell.style.fg) - 1) % 8;
                    if (static_cast<int>(cell.style.fg) > 8) {
                        fg_code += 60;  // Bright colors
                    }
                    output += "\033[" + std::to_string(fg_code) + "m";
                }

                // Attributes
                if (has_attribute(cell.style.attr, Attribute::Bold)) {
                    output += "\033[1m";
                }
                if (has_attribute(cell.style.attr, Attribute::Dim)) {
                    output += "\033[2m";
                }
                if (has_attribute(cell.style.attr, Attribute::Underline)) {
                    output += "\033[4m";
                }

                output += cell.content;
                output += "\033[0m";  // Reset

                terminal.print(x, y, output);
            }
        }
    }

    terminal.flush();

    // Save for next diff
    prev_canvas_ = canvas_;
}

void Renderer::render(bool force_redraw) {
    if (!publisher_) return;

    auto snap = publisher_->get_current();
    if (!snap) return;

    auto& terminal = Terminal::instance();

    // Get terminal size EVERY FRAME for dynamic layout
    int cols = terminal.get_terminal_width();
    int rows = terminal.get_terminal_height();

    // Safety: ensure minimum viable size
    if (cols < MIN_TERMINAL_COLS) cols = MIN_TERMINAL_COLS;
    if (rows < MIN_TERMINAL_ROWS) rows = MIN_TERMINAL_ROWS;

    // Resize canvas if terminal size changed
    static int last_cols = 0, last_rows = 0;
    bool size_changed = (cols != last_cols || rows != last_rows);

    if (size_changed) {
        canvas_.resize(cols, rows);
        prev_canvas_.resize(cols, rows);
        terminal.clear_screen();
        last_cols = cols;
        last_rows = rows;
    }

    // Clear canvas
    canvas_.clear();

    // Compute layout
    compute_layout(cols, rows);

    // RENDER WIDGETS TO CANVAS
    // Toggle between track list (Browser) and album grid (AlbumBrowser) with Ctrl+a
    if (show_album_view_) {
        album_browser_->render(canvas_, browser_rect_, *snap, focus_ == Focus::Browser);
    } else {
        browser_->render(canvas_, browser_rect_, *snap, focus_ == Focus::Browser);
    }

    header_->render(canvas_, header_rect_, *snap);
    queue_->render(canvas_, queue_rect_, *snap, focus_ == Focus::Queue);

    // Render help overlay (if visible)
    if (help_overlay_->is_visible()) {
        LayoutRect fullscreen_rect{0, 0, cols, rows};
        help_overlay_->render(canvas_, fullscreen_rect, *snap);
    }

    // FLUSH CANVAS TO TERMINAL
    flush_canvas();

    // Render album art using actual widget dimensions
    // Force render if track changed (not just terminal resize)
    static std::optional<int> last_track_idx;
    bool track_changed = (snap->player.current_track_index != last_track_idx);
    if (track_changed) {
        last_track_idx = snap->player.current_track_index;
    }
    header_->render_image_if_needed(header_rect_, size_changed || track_changed || force_redraw);

    // Render album grid artwork if in album view mode
    static bool last_album_view_state = false;
    bool album_view_activated = (show_album_view_ && !last_album_view_state);
    if (album_view_activated) {
        ouroboros::util::Logger::info("Renderer: Album view activated - forcing artwork re-render");
    }
    last_album_view_state = show_album_view_;

    if (show_album_view_) {
        album_browser_->render_images_if_needed(browser_rect_, size_changed || album_view_activated || force_redraw);
    }
}

void Renderer::handle_input() {
    auto& terminal = Terminal::instance();
    auto event = terminal.read_input();

    if (event.key_name.empty() && event.key == 0) {
        ouroboros::util::Logger::debug("handle_input: No key");
        return;
    }

    ouroboros::util::Logger::debug("handle_input: Got key=" + std::to_string(event.key) + " name=" + event.key_name);

    // Check if current widget is capturing text input
    bool input_captured = false;
    if (focus_ == Focus::Browser && browser_->is_searching()) {
        input_captured = true;
    }

    // Global quit - check both 'q' and 'Q'
    if (!input_captured && (event.key == 'q' || event.key == 'Q')) {
        ouroboros::util::Logger::info("=== QUIT KEY PRESSED ===");
        should_quit_ = true;
        return;
    }

    ouroboros::util::Logger::debug("handle_input: Publishing event...");
    auto& bus = events::EventBus::instance();

    // Play/Pause
    if (!input_captured && (event.key_name == "space" || event.key == ' ')) {
        events::Event evt;
        evt.type = events::Event::Type::PlayPause;
        bus.publish(evt);
        return;
    }

    // Next/Previous track
    if (!input_captured && event.key == 'n') {
        events::Event evt;
        evt.type = events::Event::Type::NextTrack;
        bus.publish(evt);
        return;
    }

    if (!input_captured && event.key == 'p') {
        events::Event evt;
        evt.type = events::Event::Type::PrevTrack;
        bus.publish(evt);
        return;
    }

    // Seeking
    if (!input_captured && event.key_name == "right") {
        events::Event evt;
        evt.type = events::Event::Type::SeekForward;
        evt.seek_seconds = 5;
        bus.publish(evt);
        return;
    }

    if (!input_captured && event.key_name == "left") {
        events::Event evt;
        evt.type = events::Event::Type::SeekBackward;
        evt.seek_seconds = 5;
        bus.publish(evt);
        return;
    }

    // Volume: +/- keys
    if (!input_captured && (event.key == '+' || event.key == '=')) {
        events::Event evt;
        evt.type = events::Event::Type::VolumeUp;
        evt.volume_delta = 5;
        bus.publish(evt);
        return;
    }

    if (!input_captured && (event.key == '-' || event.key == '_')) {
        events::Event evt;
        evt.type = events::Event::Type::VolumeDown;
        evt.volume_delta = 5;
        bus.publish(evt);
        return;
    }

    // Repeat: 'r' key to toggle repeat mode
    if (!input_captured && (event.key == 'r' || event.key == 'R')) {
        events::Event evt;
        evt.type = events::Event::Type::RepeatToggle;
        bus.publish(evt);
        return;
    }

    // Ctrl+a: Toggle album view (Ctrl+a = ASCII 1)
    if (event.key == 1) {
        // If closing album view, clear all Kitty graphics
        if (show_album_view_) {
            auto& img_renderer = ImageRenderer::instance();
            img_renderer.clear_image(0, 0, 0, 0);  // Params unused for Kitty (deletes ALL)
            ouroboros::util::Logger::info("Renderer: Clearing album artwork before closing view");
        }

        show_album_view_ = !show_album_view_;
        return;
    }

    // Ctrl+d: Clear queue (Ctrl+d = ASCII 4)
    if (event.key == 4) {
        events::Event evt;
        evt.type = events::Event::Type::ClearQueue;
        bus.publish(evt);
        return;
    }

    // Ctrl+f: Start Search (Ctrl+f = ASCII 6)
    if (event.key == 6) {
        // Focus Browser
        focus_ = Focus::Browser;
        
        // Switch to List View if in Album View (since AlbumBrowser has no search yet)
        if (show_album_view_) {
            show_album_view_ = false;
            // Clear graphics
            auto& img_renderer = ImageRenderer::instance();
            img_renderer.clear_image(0, 0, 0, 0);
        }
        
        // Start Search
        browser_->start_search();
        return;
    }

    // ?: Toggle help overlay
    if (!input_captured && event.key == '?') {
        help_overlay_->set_visible(!help_overlay_->is_visible());
        return;
    }

    // Tab: Switch focus between Browser and Queue
    if (!input_captured && (event.key_name == "tab" || event.key == '\t' || event.key == 9)) {
        focus_ = (focus_ == Focus::Browser) ? Focus::Queue : Focus::Browser;
        return;
    }

    // If help overlay is visible, close it on any key press except '?'
    if (help_overlay_->is_visible() && event.key != '?') {
        help_overlay_->set_visible(false);
        return;
    }

    // Route input based on focus
    if (focus_ == Focus::Browser) {
        // Browser/AlbumBrowser has focus
        if (show_album_view_) {
            album_browser_->handle_input(event);
        } else {
            browser_->handle_input(event);
        }
    } else if (focus_ == Focus::Queue) {
        // Queue has focus
        queue_->handle_input(event);
    }

    // These widgets don't handle navigation, so always pass events
    controls_->handle_input(event);
    status_bar_->handle_input(event);
}

bool Renderer::should_quit() const {
    return should_quit_;
}

}  // namespace ouroboros::ui
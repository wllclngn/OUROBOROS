#include "ui/Renderer.hpp"
#include "ui/Terminal.hpp"
#include "ui/Formatting.hpp"
#include "ui/ImageRenderer.hpp"
#include "ui/FlexLayout.hpp"
#include "ui/InputEvent.hpp"
#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include <algorithm>
#include <fstream>

namespace ouroboros::ui {

// Layout constants
namespace {
    constexpr int COLUMN_GUTTER = 1;

    // Absolute minimums (terminal must be at least this size)
    constexpr int MIN_TERMINAL_COLS = 40;  // Absolute minimum width
    constexpr int MIN_TERMINAL_ROWS = 10;  // Absolute minimum height

    // RESPONSIVE BREAKPOINTS (like mobile/desktop web design)
    constexpr int BREAKPOINT_SMALL = 80;   // < 80 cols = compact mode
    constexpr int BREAKPOINT_LARGE = 120;  // > 120 cols = expanded mode

    // Layout mode based on terminal width
    enum class LayoutMode {
        COMPACT,   // < 80 cols: Hide Queue, maximize NowPlaying
        NORMAL,    // 80-120 cols: Standard layout
        EXPANDED   // > 120 cols: More space for Browser
    };

    // Determine layout mode from terminal width
    LayoutMode get_layout_mode(int terminal_width) {
        if (terminal_width < BREAKPOINT_SMALL) return LayoutMode::COMPACT;
        if (terminal_width >= BREAKPOINT_LARGE) return LayoutMode::EXPANDED;
        return LayoutMode::NORMAL;
    }

    // Get minimum widths based on layout mode
    struct ResponsiveConstraints {
        int min_browser_width;
        int min_right_width;
        bool show_queue;
        float browser_flex;
        float right_flex;
    };

    ResponsiveConstraints get_constraints(LayoutMode mode) {
        switch (mode) {
            case LayoutMode::COMPACT:
                return {20, 25, false, 1.0f, 1.0f};  // Hide Queue, minimal widths
            case LayoutMode::NORMAL:
                return {40, 30, true, 1.0f, 1.0f};   // Standard 50/50 split
            case LayoutMode::EXPANDED:
                return {50, 35, true, 1.5f, 1.0f};   // More Browser space (60/40)
        }
        return {40, 30, true, 1.0f, 1.0f};
    }
}

Renderer::Renderer(std::shared_ptr<backend::SnapshotPublisher> publisher)
    : publisher_(publisher),
      canvas_(1, 1),
      prev_canvas_(1, 1) {
    // Create widgets directly
    header_ = std::make_unique<widgets::NowPlaying>();
    browser_ = std::make_unique<widgets::Browser>();
    queue_ = std::make_unique<widgets::Queue>();
    album_browser_ = std::make_unique<widgets::AlbumBrowser>();
    help_overlay_ = std::make_unique<widgets::HelpOverlay>();
    global_search_box_ = std::make_unique<widgets::SearchBox>();

    // Initialize image renderer
    auto& img = ImageRenderer::instance();
    img.detect_protocol();
}

Renderer::~Renderer() = default;

void Renderer::compute_layout(int cols, int rows) {
    // RESPONSIVE LAYOUT: Adapts to terminal size like mobile/desktop web

    // Content area fills entire screen
    LayoutRect content_area = {0, 0, cols, rows};

    // Determine layout mode based on terminal width
    LayoutMode mode = get_layout_mode(cols);
    ResponsiveConstraints constraints = get_constraints(mode);

    // HORIZONTAL SPLIT: [Browser] + [Right Column]
    FlexLayout horizontal_layout;
    horizontal_layout.set_direction(FlexDirection::Row);
    horizontal_layout.set_spacing(COLUMN_GUTTER);

    // Browser: flexible, adapts to screen size
    LayoutConstraints browser_constraints;
    if (show_album_view_) {
        browser_constraints.size = album_browser_->get_constraints();
    } else {
        browser_constraints.size = browser_->get_constraints();
    }
    browser_constraints.size.min_width = constraints.min_browser_width;
    browser_constraints.flex.flex_grow = constraints.browser_flex;

    // Right column: flexible, adapts to screen size
    LayoutConstraints right_constraints;
    right_constraints.size.min_width = constraints.min_right_width;
    right_constraints.flex.flex_grow = constraints.right_flex;

    horizontal_layout.add_item(nullptr, browser_constraints);
    horizontal_layout.add_item(nullptr, right_constraints);

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

    // VERTICAL SPLIT (Right Column): [NowPlaying] + [Queue]
    // RESPONSIVE: Queue hidden in compact mode

    int right_width = right_area.width;
    int right_height = right_area.height;

    // NowPlaying constants
    constexpr int NP_BORDER_H = 2;
    constexpr int NP_BORDER_W = 2;
    constexpr int NP_METADATA_H = 3;

    int np_height;
    int queue_height;

    if (constraints.show_queue) {
        // NORMAL/EXPANDED: Show both NowPlaying and Queue
        constexpr int MIN_QUEUE_H = 5;
        int max_art_height = right_height - NP_METADATA_H - NP_BORDER_H - MIN_QUEUE_H;

        int avail_width = right_width - NP_BORDER_W;
        int art_cols = std::min(avail_width, max_art_height * 2);
        if (art_cols % 2 != 0) art_cols--;
        if (art_cols < 10) art_cols = 10;

        int art_rows = art_cols / 2;
        np_height = art_rows + NP_METADATA_H + NP_BORDER_H;

        if (np_height > right_height - MIN_QUEUE_H) {
            np_height = right_height - MIN_QUEUE_H;
        }

        queue_height = right_height - np_height;
    } else {
        // COMPACT: Hide Queue, NowPlaying takes full height
        np_height = right_height;
        queue_height = 0;
    }

    header_rect_.x = right_area.x;
    header_rect_.y = right_area.y;
    header_rect_.width = right_width;
    header_rect_.height = np_height;

    queue_rect_.x = right_area.x;
    queue_rect_.y = right_area.y + np_height;
    queue_rect_.width = right_width;
    queue_rect_.height = queue_height;
}

void Renderer::render_search_overlay(const LayoutRect& rect, const model::Snapshot& snap) {
    (void)rect;
    if (focus_ != Focus::Search) return;

    // Draw box over Browser area
    LayoutRect search_rect = {browser_rect_.x, browser_rect_.y, browser_rect_.width, 3};

    // Clear area behind it
    canvas_.fill_rect(search_rect.x, search_rect.y, search_rect.width, search_rect.height, Cell{" ", Style{}});

    global_search_box_->set_visible(true);
    global_search_box_->render(canvas_, search_rect, snap);
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

    // Only render Queue if visible (hidden in compact mode)
    if (queue_rect_.height > 0) {
        queue_->render(canvas_, queue_rect_, *snap, focus_ == Focus::Queue);
    }

    // Render help overlay (if visible)
    if (help_overlay_->is_visible()) {
        LayoutRect fullscreen_rect{0, 0, cols, rows};
        help_overlay_->render(canvas_, fullscreen_rect, *snap);
    }

    // Global Search Overlay
    render_search_overlay({0, 0, canvas_.width(), canvas_.height()}, *snap);

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

    handle_input_event(event);
}

void Renderer::handle_input_event(const InputEvent& event) {
    ouroboros::util::Logger::debug("handle_input_event: key=" + std::to_string(event.key) +
        " (char='" + std::string(1, static_cast<char>(event.key)) + "') name=" + event.key_name);

    // Check if current widget is capturing text input
    bool input_captured = (focus_ == Focus::Search);

    // Global quit (from TOML: quit)
    if (!input_captured && matches_keybind(event, "quit")) {
        ouroboros::util::Logger::info("=== QUIT KEY PRESSED ===");
        should_quit_ = true;
        return;
    }
    auto& bus = events::EventBus::instance();

    // Play/Pause (from TOML: play)
    if (!input_captured && matches_keybind(event, "play")) {
        events::Event evt;
        evt.type = events::Event::Type::PlayPause;
        bus.publish(evt);
        return;
    }

    // Next track (from TOML: next)
    if (!input_captured && matches_keybind(event, "next")) {
        ouroboros::util::Logger::info("Renderer: NextTrack keybind matched");
        events::Event evt;
        evt.type = events::Event::Type::NextTrack;
        bus.publish(evt);
        return;
    }

    // Previous track (from TOML: prev)
    if (!input_captured && matches_keybind(event, "prev")) {
        ouroboros::util::Logger::info("Renderer: PrevTrack keybind matched");
        events::Event evt;
        evt.type = events::Event::Type::PrevTrack;
        bus.publish(evt);
        return;
    }

    // Seek forward (from TOML: seek_forward)
    if (!input_captured && matches_keybind(event, "seek_forward")) {
        events::Event evt;
        evt.type = events::Event::Type::SeekForward;
        evt.seek_seconds = 5;
        bus.publish(evt);
        return;
    }

    // Seek backward (from TOML: seek_backward)
    if (!input_captured && matches_keybind(event, "seek_backward")) {
        events::Event evt;
        evt.type = events::Event::Type::SeekBackward;
        evt.seek_seconds = 5;
        bus.publish(evt);
        return;
    }

    // Volume up (from TOML: volume_up)
    if (!input_captured && matches_keybind(event, "volume_up")) {
        events::Event evt;
        evt.type = events::Event::Type::VolumeUp;
        evt.volume_delta = 5;
        bus.publish(evt);
        return;
    }

    // Volume down (from TOML: volume_down)
    if (!input_captured && matches_keybind(event, "volume_down")) {
        events::Event evt;
        evt.type = events::Event::Type::VolumeDown;
        evt.volume_delta = 5;
        bus.publish(evt);
        return;
    }

    // Repeat cycle (from TOML: repeat_cycle)
    if (!input_captured && matches_keybind(event, "repeat_cycle")) {
        events::Event evt;
        evt.type = events::Event::Type::RepeatToggle;
        bus.publish(evt);
        return;
    }

    // Shuffle toggle (from TOML: shuffle_toggle)
    if (!input_captured && matches_keybind(event, "shuffle_toggle")) {
        events::Event evt;
        evt.type = events::Event::Type::ShuffleToggle;
        bus.publish(evt);
        return;
    }

    // Toggle album view (from TOML: toggle_album_view)
    if (matches_keybind(event, "toggle_album_view")) {
        // If closing album view, clear all Kitty graphics
        if (show_album_view_) {
            auto& img_renderer = ImageRenderer::instance();
            img_renderer.clear_image(0, 0, 0, 0);  // Params unused for Kitty (deletes ALL)
            ouroboros::util::Logger::info("Renderer: Clearing album artwork before closing view");
        }

        show_album_view_ = !show_album_view_;
        return;
    }

    // Clear queue (from TOML: clear_queue)
    if (matches_keybind(event, "clear_queue")) {
        events::Event evt;
        evt.type = events::Event::Type::ClearQueue;
        bus.publish(evt);
        return;
    }

    // Search (from TOML: search)
    if (matches_keybind(event, "search")) {
        focus_ = Focus::Search;
        global_search_box_->set_visible(true);
        return;
    }

    // Help overlay (from TOML: help)
    if (!input_captured && matches_keybind(event, "help")) {
        help_overlay_->set_visible(!help_overlay_->is_visible());
        return;
    }

    // Tab: Switch focus between Browser and Queue (from TOML: tab)
    if (!input_captured && matches_keybind(event, "tab")) {
        focus_ = (focus_ == Focus::Browser) ? Focus::Queue : Focus::Browser;
        return;
    }

    // If help overlay is visible, close it on any key press except help key
    if (help_overlay_->is_visible() && !matches_keybind(event, "help")) {
        help_overlay_->set_visible(false);
        return;
    }

    // Route input based on focus
    if (focus_ == Focus::Search) {
        auto result = global_search_box_->handle_search_input(event);
        std::string query = global_search_box_->get_query();
        
        // Live update filter
        ouroboros::util::Logger::debug("GlobalSearch: Query='" + query + "' -> " + (show_album_view_ ? "AlbumBrowser" : "Browser"));
        if (show_album_view_) {
            album_browser_->set_filter(query);
        } else {
            browser_->set_filter(query);
        }

        if (result == widgets::SearchBox::Result::Submit) {
            focus_ = Focus::Browser;
            global_search_box_->set_visible(false);
        } else if (result == widgets::SearchBox::Result::Cancel) {
            focus_ = Focus::Browser;
            global_search_box_->set_visible(false);
        }
    } else if (focus_ == Focus::Browser) {
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
}

bool Renderer::should_quit() const {
    return should_quit_;
}

}  // namespace ouroboros::ui
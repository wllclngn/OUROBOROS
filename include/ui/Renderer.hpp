#pragma once

#include "backend/SnapshotPublisher.hpp"
#include "ui/Canvas.hpp"
#include "ui/widgets/NowPlaying.hpp"
#include "ui/widgets/Browser.hpp"
#include "ui/widgets/Queue.hpp"
#include "ui/widgets/StatusBar.hpp"
#include "ui/widgets/SearchBox.hpp"
#include "ui/widgets/AlbumBrowser.hpp"
#include "ui/widgets/HelpOverlay.hpp"

namespace ouroboros::ui {

class Renderer {
public:
    Renderer(std::shared_ptr<backend::SnapshotPublisher> publisher);
    ~Renderer();

    void render(bool force_redraw = false);
    void handle_input();
    void handle_input_event(const InputEvent& event);
    bool should_quit() const;
    bool is_album_view_active() const { return show_album_view_; }

private:
    std::shared_ptr<backend::SnapshotPublisher> publisher_;
    bool should_quit_ = false;

    // Canvas for rendering
    Canvas canvas_;
    Canvas prev_canvas_;  // For diffing (reduces flicker)

    // Widget rectangles (computed each frame based on terminal size)
    LayoutRect browser_rect_;
    LayoutRect header_rect_;
    LayoutRect queue_rect_;
    LayoutRect status_rect_;

    // Widgets
    std::unique_ptr<widgets::NowPlaying> header_;
    std::unique_ptr<widgets::Browser> browser_;
    std::unique_ptr<widgets::Queue> queue_;
    std::unique_ptr<widgets::StatusBar> status_bar_;
    std::unique_ptr<widgets::AlbumBrowser> album_browser_;
    std::unique_ptr<widgets::HelpOverlay> help_overlay_;

    bool show_album_view_ = false;

    // Layout computation
    void compute_layout(int width, int height);

    // Canvas → Terminal rendering
    void flush_canvas();

    // Focus Management
    enum class Focus {
        Browser,
        Queue,
        Search
    };
    Focus focus_ = Focus::Browser;

    // Global Search
    std::unique_ptr<widgets::SearchBox> global_search_box_;
    void render_search_overlay(const LayoutRect& rect, const model::Snapshot& snap);
};

}  // namespace ouroboros::ui

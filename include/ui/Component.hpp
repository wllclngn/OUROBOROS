#pragma once

#include "ui/Canvas.hpp"
#include "ui/LayoutConstraints.hpp"
#include "ui/InputEvent.hpp"
#include "model/Snapshot.hpp"

namespace ouroboros::ui {

/**
 * Base class for all UI components in the OUROBOROS player.
 *
 * Components render to a Canvas (not strings!) and participate in the
 * FlexLayout system for automatic, dynamic positioning.
 *
 * This is the MODERN, production-ready interface.
 */
class Component {
public:
    virtual ~Component() = default;

    /**
     * Render this component to the canvas.
     *
     * The layout system has already computed your position and size -
     * just draw to the canvas at the specified rectangle.
     *
     * NO manual width calculations. NO string padding. Just draw.
     *
     * @param canvas    The canvas to draw on
     * @param rect      Your allocated screen space (x, y, width, height)
     * @param snap      Current application state snapshot
     */
    virtual void render(
        Canvas& canvas,
        const LayoutRect& rect,
        const model::Snapshot& snap
    ) = 0;

    /**
     * Handle input event.
     *
     * Called by input routing system when this component should process input.
     * Only called if this widget is focused or for global events.
     *
     * @param event     Input event to handle
     */
    virtual void handle_input(const InputEvent& event) {
        // Default: do nothing
        (void)event;
    }

    /**
     * Get size constraints for layout computation.
     *
     * Return your preferred/min/max size hints.
     * The FlexLayout engine uses this to compute optimal layout.
     *
     * @return Size constraints for this component
     */
    virtual SizeConstraints get_constraints() const {
        // Default: fully flexible (fill available space)
        return SizeConstraints{};
    }

protected:
    /**
     * Helper: Draw a bordered box with title.
     *
     * Draws box border and title at the top of the rectangle.
     * Returns the inner content rectangle (area inside border).
     */
    LayoutRect draw_box_border(
        Canvas& canvas,
        const LayoutRect& rect,
        const std::string& title,
        Style border_style = Style{},
        bool title_highlighted = false
    ) {
        // Draw border
        canvas.draw_rect(rect.x, rect.y, rect.width, rect.height, border_style);

        // Draw title (top-left of border)
        if (!title.empty()) {
            // Highlight title when widget has focus (same color as track selection)
            Style title_style = title_highlighted ?
                Style{Color::BrightYellow, Color::Default, Attribute::Bold} :
                Style{Color::BrightWhite, Color::Default, Attribute::Bold};

            canvas.draw_text(
                rect.x + 2,
                rect.y,
                " " + title + " ",
                title_style
            );
        }

        // Return inner content rect (inside border)
        return LayoutRect{
            rect.x + 1,
            rect.y + 1,
            rect.width - 2,
            rect.height - 2
        };
    }

    /**
     * Helper: Truncate text to fit within max_width.
     *
     * Adds "..." if truncated.
     */
    std::string truncate_text(const std::string& text, int max_width) const {
        if (static_cast<int>(text.length()) <= max_width) {
            return text;
        }
        if (max_width <= 3) {
            return text.substr(0, max_width);
        }
        return text.substr(0, max_width - 3) + "...";
    }

    /**
     * Helper: Format duration in seconds as "MM:SS" or "H:MM:SS".
     */
    std::string format_duration(int total_seconds) const;

    /**
     * Helper: Format track for display (e.g., "Artist - Title [3:45]").
     *
     * @param track         The track to format
     * @param max_width     Maximum width for display
     * @param show_duration Whether to include duration in brackets
     * @return Formatted track string
     */
    std::string format_track_display(
        const model::Track& track,
        int max_width,
        bool show_duration = false
    ) const;

    /**
     * Helper: Format track metadata line (e.g., "Artist - Album (2023)").
     *
     * @param track         The track to format
     * @param include_album Whether to include album name
     * @return Formatted metadata string
     */
    std::string format_track_metadata_line(
        const model::Track& track,
        bool include_album = true
    ) const;

    /**
     * Helper: Center text within given width.
     */
    std::string center_text(const std::string& text, int width) const;

    /**
     * Helper: Pad text to width (right-aligned).
     */
    std::string pad_right(const std::string& text, int width) const;

    /**
     * Helper: Format file size (e.g., "3.5 MB").
     */
    std::string format_filesize(size_t bytes) const;
};

}  // namespace ouroboros::ui

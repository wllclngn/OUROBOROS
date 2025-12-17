#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include "backend/HierarchicalCache.hpp"
#include <string>
#include <vector>
#include <filesystem>
#include <memory>

namespace ouroboros::ui::widgets {

/**
 * DirectoryBrowser Widget
 *
 * Displays top-level music directories for hierarchical navigation.
 * Allows users to browse by filesystem directory instead of by metadata.
 */
class DirectoryBrowser : public Component {
public:
    DirectoryBrowser();

    // Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap, bool is_focused);

    void handle_input(const InputEvent& event) override;

    SizeConstraints get_constraints() const override;

    // Directory navigation
    void set_directories(const std::vector<backend::DirectoryMetadata>& directories);
    std::optional<std::string> get_selected_directory() const;
    int get_selected_index() const { return selected_index_; }

private:
    // Render directory list
    void render_directory_list(Canvas& canvas, const LayoutRect& content_rect, bool is_focused);

    // Directory state
    std::vector<backend::DirectoryMetadata> directories_;
    int selected_index_ = 0;
    int scroll_offset_ = 0;

    // UI state
    bool needs_refresh_ = true;
};

}  // namespace ouroboros::ui::widgets

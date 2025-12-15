#pragma once

#include "ui/Component.hpp"
#include "model/Snapshot.hpp"
#include <string>
#include <vector>

namespace ouroboros::ui::widgets {

class SearchBox : public Component {
public:
    enum class Result {
        None,
        Submit,
        Cancel
    };

    // NEW INTERFACE: Canvas-based rendering
    void render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) override;

    void handle_input(const InputEvent& event) override; // Generic interface
    Result handle_search_input(const InputEvent& event); // Specialized interface

    SizeConstraints get_constraints() const override;

    bool is_visible() const { return visible_; }
    void set_visible(bool v);

    std::string get_query() const { return query_; }
    void clear();

private:
    bool visible_ = false;
    std::string query_;
    int cursor_pos_ = 0;
};

}  // namespace ouroboros::ui::widgets

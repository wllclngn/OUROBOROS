#include "ui/widgets/Controls.hpp"
#include "ui/Formatting.hpp"
#include "ui/VisualBlocks.hpp"
#include "config/Theme.hpp"
#include <string>

namespace ouroboros::ui::widgets {

// Controls widget is currently unused - StatusBar handles this now
// Keeping as stub for future use

void Controls::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    (void)canvas;
    (void)rect;
    (void)snap;
    // Not currently used in layout
}

void Controls::handle_input(const InputEvent&) {
}

}  // namespace ouroboros::ui::widgets

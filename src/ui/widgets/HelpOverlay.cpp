#include "ui/widgets/HelpOverlay.hpp"
#include "config/UIConfig.hpp"
#include "ui/InputEvent.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

namespace ouroboros::ui::widgets {

HelpOverlay::HelpOverlay() {
    load_from_man();
}

bool HelpOverlay::read_lines(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return false;

    std::vector<std::string> collected;
    std::array<char, 1024> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        std::string line(buf.data());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        collected.push_back(std::move(line));
    }
    const int status = pclose(pipe);
    if (status != 0 || collected.empty()) return false;

    // man pads every page with leading and trailing blank lines; drop them so the
    // overlay opens on content rather than on whitespace.
    auto first = std::find_if(collected.begin(), collected.end(),
                              [](const std::string& l) { return !l.empty(); });
    auto last = std::find_if(collected.rbegin(), collected.rend(),
                             [](const std::string& l) { return !l.empty(); }).base();
    if (first >= last) return false;

    lines_.assign(first, last);
    return true;
}

void HelpOverlay::load_from_man() {
    lines_.clear();

    if (read_lines("man -P cat ouroboros 2>/dev/null | col -b")) {
        util::Logger::debug("HelpOverlay: loaded " + std::to_string(lines_.size()) +
                            " lines from the installed manpage");
        return;
    }

    // Dev tree: format the page in the repository directly. Rendering the roff
    // source raw would be unreadable, so man still does the formatting.
    for (const char* candidate : {"ouroboros.1", "../ouroboros.1"}) {
        if (!std::filesystem::exists(candidate)) continue;
        if (read_lines(std::string("man -P cat ./") + candidate + " 2>/dev/null | col -b")) {
            util::Logger::debug("HelpOverlay: loaded " + std::to_string(lines_.size()) +
                                " lines from " + candidate);
            return;
        }
    }

    lines_.push_back("Help unavailable: the ouroboros(1) manpage could not be found.");
    lines_.push_back("");
    lines_.push_back("Install it with ./install.py, or run from the source tree so");
    lines_.push_back("ouroboros.1 sits beside the binary.");
    util::Logger::warn("HelpOverlay: no manpage found; showing the fallback notice");
}

void HelpOverlay::render(Canvas& canvas, const LayoutRect& rect, const model::Snapshot& snap) {
    (void)snap;
    if (!visible_) return;

    const auto& uc = config::ui_config();
    auto inner = draw_box_border(canvas, rect, "HELP  (? or ESC to close)", true);

    int available = inner.height;
    if (available < 1) return;

    int total = static_cast<int>(lines_.size());
    int max_scroll = std::max(0, total - available);
    if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
    if (scroll_offset_ < 0) scroll_offset_ = 0;

    int end = std::min(total, scroll_offset_ + available);
    int y = inner.y;
    int max_w = inner.width - 2;

    for (int i = scroll_offset_; i < end; ++i, ++y) {
        const std::string& line = lines_[i];
        if (line.empty()) continue;
        // man's own layout carries the structure: section headings sit flush left,
        // everything else is indented. That is enough to style from without
        // reparsing the roff.
        const bool heading = line[0] != ' ' && line[0] != '\t';
        canvas.draw_text(inner.x + 1, y, truncate_text(line, max_w),
                         heading ? uc.selection : uc.title);
    }

    if (total > available) {
        int pct = (max_scroll > 0) ? (scroll_offset_ * 100) / max_scroll : 0;
        std::string indicator = std::to_string(pct) + "%";
        canvas.draw_text(inner.x + inner.width - static_cast<int>(indicator.size()) - 1,
                        rect.y, " " + indicator + " ", uc.muted);
    }
}

void HelpOverlay::handle_input(const InputEvent& event) {
    if (matches_keybind(event, "nav_down")) {
        scroll_offset_++;
    } else if (matches_keybind(event, "nav_up")) {
        scroll_offset_--;
    } else if (event.key == 'd') {
        scroll_offset_ += 20;
    } else if (event.key == 'u') {
        scroll_offset_ -= 20;
    } else if (event.key == 'g') {
        scroll_offset_ = 0;
    } else if (event.key == 'G') {
        scroll_offset_ = static_cast<int>(lines_.size());  // clamped in render
    }
}

SizeConstraints HelpOverlay::get_constraints() const {
    return SizeConstraints{};
}

}  // namespace ouroboros::ui::widgets

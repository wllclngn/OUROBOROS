#include "model/Snapshot.hpp"

#include <vector>

// Two Stacks queue operations split out so they have no heavy dependencies and
// can be unit-tested with synthetic QueueState values.

namespace ouroboros::model {

bool jump_queue_to_index(QueueState& q, int display_index) {
    const int hist_size = static_cast<int>(q.history.size());
    const bool has_current = q.current.has_value();
    const int total = hist_size + (has_current ? 1 : 0) + static_cast<int>(q.future.size());

    if (display_index < 0 || display_index >= total) return false;
    if (has_current && display_index == hist_size) return false;  // already playing this track

    // Flatten to display order, then re-partition around the target index.
    std::vector<int> flat;
    flat.reserve(static_cast<size_t>(total));
    flat.insert(flat.end(), q.history.begin(), q.history.end());
    if (has_current) flat.push_back(*q.current);
    flat.insert(flat.end(), q.future.begin(), q.future.end());

    q.history.assign(flat.begin(), flat.begin() + display_index);
    q.current = flat[static_cast<size_t>(display_index)];
    q.future.assign(flat.begin() + display_index + 1, flat.end());
    return true;
}

}  // namespace ouroboros::model

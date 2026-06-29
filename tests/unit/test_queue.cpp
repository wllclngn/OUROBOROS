#include "SimpleTest.hpp"
#include "model/Snapshot.hpp"

#include <optional>
#include <vector>

using ouroboros::model::QueueState;
using ouroboros::model::jump_queue_to_index;

namespace {
QueueState make_queue(std::vector<int> history, std::optional<int> current,
                      std::vector<int> future) {
    QueueState q;
    q.history = std::move(history);
    q.current = current;
    q.future = std::move(future);
    return q;
}
}  // namespace

// Display order is history ++ [current] ++ future.
// history=[1,2] current=3 future=[4,5]  ->  display indices 0..4 = {1,2,3,4,5}

TEST_CASE(test_jump_into_future_repartitions) {
    QueueState q = make_queue({1, 2}, 3, {4, 5});
    // Jump to display index 3 -> track 4 becomes current.
    ASSERT_TRUE(jump_queue_to_index(q, 3));
    ASSERT_TRUE(q.history == (std::vector<int>{1, 2, 3}));
    ASSERT_TRUE(q.current.has_value() && *q.current == 4);
    ASSERT_TRUE(q.future == (std::vector<int>{5}));
}

TEST_CASE(test_jump_into_history_repartitions) {
    QueueState q = make_queue({1, 2}, 3, {4, 5});
    // Jump back to display index 0 -> track 1 becomes current; 2,3,4,5 become future.
    ASSERT_TRUE(jump_queue_to_index(q, 0));
    ASSERT_TRUE(q.history.empty());
    ASSERT_TRUE(q.current.has_value() && *q.current == 1);
    ASSERT_TRUE(q.future == (std::vector<int>{2, 3, 4, 5}));
}

TEST_CASE(test_jump_to_current_is_noop) {
    QueueState q = make_queue({1, 2}, 3, {4, 5});
    // Display index 2 is the current track -> no change, returns false.
    ASSERT_FALSE(jump_queue_to_index(q, 2));
    ASSERT_TRUE(q.history == (std::vector<int>{1, 2}));
    ASSERT_TRUE(q.current.has_value() && *q.current == 3);
    ASSERT_TRUE(q.future == (std::vector<int>{4, 5}));
}

TEST_CASE(test_jump_last_index) {
    QueueState q = make_queue({1, 2}, 3, {4, 5});
    // Display index 4 (last) -> track 5 current, future empty.
    ASSERT_TRUE(jump_queue_to_index(q, 4));
    ASSERT_TRUE(q.history == (std::vector<int>{1, 2, 3, 4}));
    ASSERT_TRUE(q.current.has_value() && *q.current == 5);
    ASSERT_TRUE(q.future.empty());
}

TEST_CASE(test_jump_out_of_bounds_rejected) {
    QueueState q = make_queue({1, 2}, 3, {4, 5});  // total = 5
    ASSERT_FALSE(jump_queue_to_index(q, 5));   // == total
    ASSERT_FALSE(jump_queue_to_index(q, 99));
    ASSERT_FALSE(jump_queue_to_index(q, -1));
    // Unchanged
    ASSERT_TRUE(q.history == (std::vector<int>{1, 2}));
    ASSERT_TRUE(q.current.has_value() && *q.current == 3);
    ASSERT_TRUE(q.future == (std::vector<int>{4, 5}));
}

TEST_CASE(test_jump_no_current_track) {
    // No current (stopped), only future. Display order = future.
    QueueState q = make_queue({}, std::nullopt, {10, 11, 12});
    ASSERT_TRUE(jump_queue_to_index(q, 1));  // -> track 11 current
    ASSERT_TRUE(q.history == (std::vector<int>{10}));
    ASSERT_TRUE(q.current.has_value() && *q.current == 11);
    ASSERT_TRUE(q.future == (std::vector<int>{12}));
}

TEST_CASE(test_jump_preserves_skipped_for_previous) {
    // Jumping forward must leave skipped tracks in history so Previous can walk back.
    QueueState q = make_queue({}, 1, {2, 3, 4});  // playing 1
    ASSERT_TRUE(jump_queue_to_index(q, 3));        // jump to track 4
    // 1,2,3 are now history in play order -> Previous returns 3, then 2, then 1.
    ASSERT_TRUE(q.history == (std::vector<int>{1, 2, 3}));
    ASSERT_TRUE(q.current.has_value() && *q.current == 4);
}

int main() {
    return ouroboros::test::TestRunner::instance().run_all();
}

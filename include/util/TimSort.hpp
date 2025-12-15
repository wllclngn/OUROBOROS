#pragma once

#include <vector>
#include <functional>
#include <algorithm>
#include <cstdint>
#include <iterator>

namespace ouroboros::util {

// Forward declaration of helper implemented in .cpp
size_t compute_min_run_length(size_t n);

namespace detail {

// Internal structure to track runs
struct Run {
    size_t base;      // Starting index (offset from first)
    size_t length;    // Length of the run
};

// Binary insertion sort: O(n log n) comparisons, O(n^2) moves
// Efficient for small arrays (like TimSort runs)
template<typename RandomIt, typename Compare>
void binary_insertion_sort(RandomIt first, RandomIt last, Compare comp) {
    for (auto it = first + 1; it != last; ++it) {
        // Move element to temp to avoid overwrites
        auto value = std::move(*it);
        
        // Find position where value should be inserted
        // upper_bound returns first element > value
        // We want to insert *before* that element
        auto left = first;
        auto right = it;
        
        // Custom binary search to handle strict weak ordering with 'comp'
        while (left < right) {
            auto mid = left + (std::distance(left, right) / 2);
            if (comp(value, *mid)) {
                right = mid;
            } else {
                left = mid + 1;
            }
        }
        
        // Shift elements right
        std::move_backward(left, it, it + 1);
        
        // Place value
        *left = std::move(value);
    }
}

// Identifies a run (monotonic sequence) and extends it if necessary.
// Returns {run_length, was_descending}
template<typename RandomIt, typename Compare>
std::pair<size_t, bool> count_run_and_make_ascending(RandomIt first, RandomIt last, Compare comp) {
    if (std::distance(first, last) <= 1) {
        return {static_cast<size_t>(std::distance(first, last)), false};
    }

    auto run_end = first + 1;

    // Check if run is descending
    if (comp(*run_end, *first)) {
        // Strictly descending run: a > b
        while (run_end != last && comp(*run_end, *(run_end - 1))) {
            ++run_end;
        }
        // Reverse to make ascending
        std::reverse(first, run_end);
        return {static_cast<size_t>(std::distance(first, run_end)), true};
    } else {
        // Non-descending (ascending) run: a <= b
        while (run_end != last && !comp(*run_end, *(run_end - 1))) {
            ++run_end;
        }
        return {static_cast<size_t>(std::distance(first, run_end)), false};
    }
}

// Merges two adjacent runs
template<typename RandomIt, typename Compare>
void merge_at(
    RandomIt first,
    std::vector<Run>& runs,
    size_t i,
    Compare comp
) {
    const auto& run1 = runs[i];
    const auto& run2 = runs[i + 1];
    
    auto base1 = first + run1.base;
    auto len1 = run1.length;
    auto base2 = first + run2.base;
    auto len2 = run2.length;
    
    // Create temporary buffer for the left (smaller) run
    // (TimSort optimization: usually merge smaller into larger, but here we simplify to left merge)
    using ValueType = typename std::iterator_traits<RandomIt>::value_type;
    std::vector<ValueType> temp_left(len1);
    std::move(base1, base1 + len1, temp_left.begin());
    
    auto cursor1 = temp_left.begin();
    auto cursor2 = base2;
    auto dest = base1;
    auto end1 = temp_left.end();
    auto end2 = base2 + len2;
    
    // Merge
    while (cursor1 != end1 && cursor2 != end2) {
        if (comp(*cursor2, *cursor1)) {
            *dest++ = std::move(*cursor2++);
        } else {
            *dest++ = std::move(*cursor1++);
        }
    }
    
    // Copy remaining elements from temp_left (if any)
    // (Remaining elements from run2 are already in place)
    if (cursor1 != end1) {
        std::move(cursor1, end1, dest);
    }
    
    // Update run info (merge run2 into run1)
    runs[i].length = len1 + len2;
    runs.erase(runs.begin() + i + 1);
}

// Collapses the stack of runs to maintain invariants
template<typename RandomIt, typename Compare>
void merge_collapse(RandomIt first, std::vector<Run>& runs, Compare comp) {
    while (runs.size() > 1) {
        size_t n = runs.size() - 2;
        
        if (n > 0 && runs[n - 1].length <= runs[n].length + runs[n + 1].length) {
            if (runs[n - 1].length < runs[n + 1].length) {
                --n;
            }
            merge_at(first, runs, n, comp);
        } else if (runs[n].length <= runs[n + 1].length) {
            merge_at(first, runs, n, comp);
        } else {
            break;
        }
    }
}

// Forces all runs to merge at the end
template<typename RandomIt, typename Compare>
void merge_force_collapse(RandomIt first, std::vector<Run>& runs, Compare comp) {
    while (runs.size() > 1) {
        size_t n = runs.size() - 2;
        if (n > 0 && runs[n - 1].length < runs[n + 1].length) {
            --n;
        }
        merge_at(first, runs, n, comp);
    }
}

} // namespace detail

/**
 * TimSort implementation.
 * Stable, adaptive, iterative mergesort variant.
 * O(n) for sorted data, O(n log n) worst case.
 */
template<typename RandomIt, typename Compare>
void timsort(RandomIt first, RandomIt last, Compare comp) {
    const size_t n = std::distance(first, last);
    if (n < 2) return;
    
    // Determine minimum run length
    size_t min_run = compute_min_run_length(n);
    
    // Stack of pending runs
    std::vector<detail::Run> runs;
    runs.reserve(40); // Standard TimSort stack depth
    
    size_t n_remaining = n;
    auto cur = first;
    
    while (n_remaining > 0) {
        // Find natural run
        auto [run_len, _] = detail::count_run_and_make_ascending(cur, last, comp);
        
        // If short, extend to min_run
        if (run_len < min_run) {
            size_t force = std::min(n_remaining, min_run);
            detail::binary_insertion_sort(cur, cur + force, comp);
            run_len = force;
        }
        
        // Push run
        runs.push_back({
            static_cast<size_t>(std::distance(first, cur)),
            run_len
        });
        
        // Merge if invariants violated
        detail::merge_collapse(first, runs, comp);
        
        cur += run_len;
        n_remaining -= run_len;
    }
    
    // Final cleanup
    detail::merge_force_collapse(first, runs, comp);
}

// Convenience overload for containers
template<typename Container, typename Compare>
void timsort(Container& c, Compare comp) {
    timsort(c.begin(), c.end(), comp);
}

} // namespace ouroboros::util
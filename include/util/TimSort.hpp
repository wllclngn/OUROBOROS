#pragma once

#include <vector>
#include <algorithm>
#include <iterator>
#include <thread>
#include <chrono>
#include "Logger.hpp"

namespace ouroboros::util {

// Forward declaration
size_t compute_min_run_length(size_t n);

namespace detail {

struct Run {
    size_t base;
    size_t length;
};

// Binary insertion sort for small runs
template<typename RandomIt, typename Compare>
void binary_insertion_sort(RandomIt first, RandomIt last, Compare comp) {
    for (auto it = first + 1; it != last; ++it) {
        auto value = std::move(*it);
        auto left = first;
        auto right = it;

        while (left < right) {
            auto mid = left + (std::distance(left, right) / 2);
            if (comp(value, *mid)) {
                right = mid;
            } else {
                left = mid + 1;
            }
        }

        std::move_backward(left, it, it + 1);
        *left = std::move(value);
    }
}

// Count natural run and make ascending
template<typename RandomIt, typename Compare>
std::pair<size_t, bool> count_run_and_make_ascending(RandomIt first, RandomIt last, Compare comp) {
    if (std::distance(first, last) <= 1) {
        return {static_cast<size_t>(std::distance(first, last)), false};
    }

    auto run_end = first + 1;

    if (comp(*run_end, *first)) {
        while (run_end != last && comp(*run_end, *(run_end - 1))) {
            ++run_end;
        }
        std::reverse(first, run_end);
        return {static_cast<size_t>(std::distance(first, run_end)), true};
    } else {
        while (run_end != last && !comp(*run_end, *(run_end - 1))) {
            ++run_end;
        }
        return {static_cast<size_t>(std::distance(first, run_end)), false};
    }
}

// Merge two adjacent sorted ranges
template<typename RandomIt, typename Compare>
void merge_ranges(RandomIt first, size_t left_len, size_t right_len, Compare comp) {
    using ValueType = typename std::iterator_traits<RandomIt>::value_type;

    // Copy left side to temp buffer
    std::vector<ValueType> temp(left_len);
    std::move(first, first + left_len, temp.begin());

    auto left = temp.begin();
    auto left_end = temp.end();
    auto right = first + left_len;
    auto right_end = first + left_len + right_len;
    auto dest = first;

    while (left != left_end && right != right_end) {
        if (comp(*right, *left)) {
            *dest++ = std::move(*right++);
        } else {
            *dest++ = std::move(*left++);
        }
    }

    while (left != left_end) {
        *dest++ = std::move(*left++);
    }
    // Right side elements are already in place
}

// Sequential merge collapse - maintains TimSort invariants
template<typename RandomIt, typename Compare>
void merge_collapse(RandomIt first, std::vector<Run>& runs, Compare comp) {
    while (runs.size() > 1) {
        size_t n = runs.size() - 2;

        // Check invariants and merge if violated
        if (n > 0 && runs[n - 1].length <= runs[n].length + runs[n + 1].length) {
            // Merge smaller of runs[n-1] and runs[n+1] with runs[n]
            if (runs[n - 1].length < runs[n + 1].length) {
                --n;
            }
        } else if (runs[n].length <= runs[n + 1].length) {
            // Merge runs[n] with runs[n+1]
        } else {
            break;  // Invariants satisfied
        }

        // Merge runs[n] and runs[n+1]
        merge_ranges(first + runs[n].base, runs[n].length, runs[n + 1].length, comp);
        runs[n].length += runs[n + 1].length;
        runs.erase(runs.begin() + n + 1);
    }
}

// Force collapse all remaining runs sequentially
template<typename RandomIt, typename Compare>
void force_collapse(RandomIt first, std::vector<Run>& runs, Compare comp) {
    while (runs.size() > 1) {
        size_t n = runs.size() - 2;

        // Merge smaller neighbor
        if (n > 0 && runs[n - 1].length < runs[n + 1].length) {
            --n;
        }

        merge_ranges(first + runs[n].base, runs[n].length, runs[n + 1].length, comp);
        runs[n].length += runs[n + 1].length;
        runs.erase(runs.begin() + n + 1);
    }
}

} // namespace detail

/**
 * Sequential TimSort implementation.
 * Exploits natural runs in data for O(n) best case on sorted/nearly-sorted data.
 * O(n log n) worst case. Stable sort.
 */
template<typename RandomIt, typename Compare>
void timsort(RandomIt first, RandomIt last, Compare comp) {
    const size_t n = std::distance(first, last);
    if (n < 2) return;

    size_t min_run = compute_min_run_length(n);
    std::vector<detail::Run> runs;
    runs.reserve(40);

    size_t n_remaining = n;
    auto cur = first;

    // Phase 1: Identify and extend runs
    while (n_remaining > 0) {
        auto [run_len, _] = detail::count_run_and_make_ascending(cur, last, comp);

        if (run_len < min_run) {
            size_t force = std::min(n_remaining, min_run);
            detail::binary_insertion_sort(cur, cur + force, comp);
            run_len = force;
        }

        runs.push_back({
            static_cast<size_t>(std::distance(first, cur)),
            run_len
        });

        // Merge when invariants violated
        detail::merge_collapse(first, runs, comp);

        cur += run_len;
        n_remaining -= run_len;
    }

    // Phase 2: Final collapse
    detail::force_collapse(first, runs, comp);
}

// Convenience overload for containers
template<typename Container, typename Compare>
void timsort(Container& c, Compare comp) {
    timsort(c.begin(), c.end(), comp);
}

/**
 * Parallel TimSort implementation.
 * Divides data into chunks, sorts each chunk in parallel with timsort,
 * then merges the sorted chunks using parallel tree reduction.
 */
template<typename RandomIt, typename Compare>
void parallel_timsort(RandomIt first, RandomIt last, Compare comp) {
    const size_t n = std::distance(first, last);
    if (n < 2) return;

    auto t0 = std::chrono::steady_clock::now();

    const size_t num_threads = std::max(std::thread::hardware_concurrency(), 1u);
    const size_t chunk_size = std::max((n + num_threads - 1) / num_threads, size_t{1});

    Logger::info("parallel_timsort: n=" + std::to_string(n) + " threads=" + std::to_string(num_threads) +
                 " chunk_size=" + std::to_string(chunk_size));

    // Phase 1: Sort chunks in parallel
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (size_t t = 0; t < num_threads; ++t) {
        size_t start = t * chunk_size;
        if (start >= n) break;
        size_t end = std::min(start + chunk_size, n);

        workers.emplace_back([first, start, end, &comp]() {
            timsort(first + start, first + end, comp);
        });
    }

    Logger::info("Phase 1: launched " + std::to_string(workers.size()) + " workers");

    for (auto& w : workers) {
        w.join();
    }

    auto t1 = std::chrono::steady_clock::now();
    Logger::info("Phase 1 done: " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()) + " ms");

    // Phase 2: Parallel tree merge of sorted chunks
    using ValueType = typename std::iterator_traits<RandomIt>::value_type;
    std::vector<ValueType> buffer(n);

    size_t merge_size = chunk_size;
    while (merge_size < n) {
        std::vector<std::thread> merge_workers;
        size_t next_merge_size = merge_size * 2;

        for (size_t start = 0; start < n; start += next_merge_size) {
            size_t mid = std::min(start + merge_size, n);
            size_t end = std::min(start + next_merge_size, n);

            if (mid >= end) continue;

            merge_workers.emplace_back([first, start, mid, end, &buffer, &comp]() {
                auto left = first + start;
                auto left_end = first + mid;
                auto right = first + mid;
                auto right_end = first + end;
                auto dest = buffer.begin() + start;

                while (left != left_end && right != right_end) {
                    if (comp(*right, *left)) {
                        *dest++ = std::move(*right++);
                    } else {
                        *dest++ = std::move(*left++);
                    }
                }
                while (left != left_end) {
                    *dest++ = std::move(*left++);
                }
                while (right != right_end) {
                    *dest++ = std::move(*right++);
                }
            });
        }

        for (auto& w : merge_workers) {
            w.join();
        }

        // Copy merged results back
        for (size_t start = 0; start < n; start += next_merge_size) {
            size_t mid = std::min(start + merge_size, n);
            size_t end = std::min(start + next_merge_size, n);
            if (mid < end) {
                std::move(buffer.begin() + start, buffer.begin() + end, first + start);
            }
        }

        merge_size = next_merge_size;
    }

    auto t2 = std::chrono::steady_clock::now();
    Logger::info("Phase 2 done: " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()) + " ms (total: " +
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count()) + " ms)");
}

// Convenience overload for containers
template<typename Container, typename Compare>
void parallel_timsort(Container& c, Compare comp) {
    parallel_timsort(c.begin(), c.end(), comp);
}

} // namespace ouroboros::util

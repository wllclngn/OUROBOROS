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

constexpr size_t MIN_GALLOP = 7;

struct Run {
    size_t base;
    size_t length;
    int power;    // node level in PowerSort merge tree
};

// PowerSort node power computation (from CPython 3.11+).
// Computes the depth in a conceptual nearly-optimal binary merge tree
// for the boundary between two adjacent runs.
// s1: start index of left run, n1: length of left run,
// n2: length of right run, n: total list length.
inline int powerloop(size_t s1, size_t n1, size_t n2, size_t n) {
    int result = 0;
    // Midpoints a and b, doubled to avoid fractions:
    //   a = 2*(s1 + n1/2) = 2*s1 + n1
    //   b = 2*(s1 + n1 + n2/2) = a + n1 + n2
    size_t a = 2 * s1 + n1;
    size_t b = a + n1 + n2;
    // Emulate a/n and b/n one bit at a time, until bits differ
    for (;;) {
        ++result;
        if (a >= n) {
            a -= n;
            b -= n;
        } else if (b >= n) {
            break;
        }
        a <<= 1;
        b <<= 1;
    }
    return result;
}

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

// Gallop left: find leftmost insertion point via exponential search.
// Returns k such that base[k-1] < key <= base[k].
template<typename RandomIt, typename Compare>
size_t gallop_left(const typename std::iterator_traits<RandomIt>::value_type& key,
                   RandomIt base, size_t len, size_t hint, Compare comp) {
    size_t last_ofs = 0;
    size_t ofs = 1;

    if (comp(*(base + hint), key)) {
        size_t max_ofs = len - hint;
        while (ofs < max_ofs && comp(*(base + hint + ofs), key)) {
            last_ofs = ofs;
            ofs = (ofs << 1) + 1;
            if (ofs <= last_ofs) ofs = max_ofs;
        }
        if (ofs > max_ofs) ofs = max_ofs;
        last_ofs += hint;
        ofs += hint;
    } else {
        size_t max_ofs = hint + 1;
        while (ofs < max_ofs && !comp(*(base + hint - ofs), key)) {
            last_ofs = ofs;
            ofs = (ofs << 1) + 1;
            if (ofs <= last_ofs) ofs = max_ofs;
        }
        if (ofs > max_ofs) ofs = max_ofs;
        size_t tmp = last_ofs;
        last_ofs = (ofs > hint) ? 0 : hint - ofs;
        ofs = hint - tmp;
    }

    while (last_ofs < ofs) {
        size_t mid = last_ofs + ((ofs - last_ofs) >> 1);
        if (comp(*(base + mid), key)) {
            last_ofs = mid + 1;
        } else {
            ofs = mid;
        }
    }
    return ofs;
}

// Gallop right: find rightmost insertion point via exponential search.
// Returns k such that base[k-1] <= key < base[k].
template<typename RandomIt, typename Compare>
size_t gallop_right(const typename std::iterator_traits<RandomIt>::value_type& key,
                    RandomIt base, size_t len, size_t hint, Compare comp) {
    size_t last_ofs = 0;
    size_t ofs = 1;

    if (comp(key, *(base + hint))) {
        size_t max_ofs = hint + 1;
        while (ofs < max_ofs && comp(key, *(base + hint - ofs))) {
            last_ofs = ofs;
            ofs = (ofs << 1) + 1;
            if (ofs <= last_ofs) ofs = max_ofs;
        }
        if (ofs > max_ofs) ofs = max_ofs;
        size_t tmp = last_ofs;
        last_ofs = (ofs > hint) ? 0 : hint - ofs;
        ofs = hint - tmp;
    } else {
        size_t max_ofs = len - hint;
        while (ofs < max_ofs && !comp(key, *(base + hint + ofs))) {
            last_ofs = ofs;
            ofs = (ofs << 1) + 1;
            if (ofs <= last_ofs) ofs = max_ofs;
        }
        if (ofs > max_ofs) ofs = max_ofs;
        last_ofs += hint;
        ofs += hint;
    }

    while (last_ofs < ofs) {
        size_t mid = last_ofs + ((ofs - last_ofs) >> 1);
        if (comp(key, *(base + mid))) {
            ofs = mid;
        } else {
            last_ofs = mid + 1;
        }
    }
    return ofs;
}

// Merge lo: left run is smaller, copy left to temp, merge left-to-right.
template<typename RandomIt, typename Compare>
void merge_lo(RandomIt base, size_t left_len, size_t right_len, Compare comp,
              std::vector<typename std::iterator_traits<RandomIt>::value_type>& tmp,
              size_t& min_gallop) {
    tmp.resize(left_len);
    std::move(base, base + left_len, tmp.begin());

    auto cursor1 = tmp.begin();
    auto cursor2 = base + left_len;
    auto dest = base;

    size_t left_remaining = left_len;
    size_t right_remaining = right_len;

    while (left_remaining > 1 && right_remaining > 0) {
        size_t count1 = 0;
        size_t count2 = 0;

        // One-at-a-time mode
        do {
            if (comp(*cursor2, *cursor1)) {
                *dest++ = std::move(*cursor2++);
                --right_remaining;
                ++count2;
                count1 = 0;
                if (right_remaining == 0) goto lo_epilogue;
            } else {
                *dest++ = std::move(*cursor1++);
                --left_remaining;
                ++count1;
                count2 = 0;
                if (left_remaining == 1) goto lo_epilogue;
            }
        } while ((count1 | count2) < min_gallop);

        // Gallop mode
        do {
            count1 = gallop_right<decltype(cursor1), Compare>(*cursor2, cursor1, left_remaining, 0, comp);
            if (count1 > 0) {
                std::move(cursor1, cursor1 + count1, dest);
                dest += count1;
                cursor1 += count1;
                left_remaining -= count1;
                if (left_remaining <= 1) goto lo_epilogue;
            }
            *dest++ = std::move(*cursor2++);
            --right_remaining;
            if (right_remaining == 0) goto lo_epilogue;

            count2 = gallop_left<RandomIt, Compare>(*cursor1, cursor2, right_remaining, 0, comp);
            if (count2 > 0) {
                std::move(cursor2, cursor2 + count2, dest);
                dest += count2;
                cursor2 += count2;
                right_remaining -= count2;
                if (right_remaining == 0) goto lo_epilogue;
            }
            *dest++ = std::move(*cursor1++);
            --left_remaining;
            if (left_remaining == 1) goto lo_epilogue;

            if (count1 >= MIN_GALLOP || count2 >= MIN_GALLOP) {
                if (min_gallop > 1) --min_gallop;
            } else {
                ++min_gallop;
            }
        } while (count1 >= MIN_GALLOP || count2 >= MIN_GALLOP);

        ++min_gallop;
    }

lo_epilogue:
    if (left_remaining == 1) {
        std::move(cursor2, cursor2 + right_remaining, dest);
        *(dest + right_remaining) = std::move(*cursor1);
    } else if (left_remaining > 0) {
        std::move(cursor1, cursor1 + left_remaining, dest);
    }
}

// Merge hi: right run is smaller, copy right to temp, merge right-to-left.
template<typename RandomIt, typename Compare>
void merge_hi(RandomIt base, size_t left_len, size_t right_len, Compare comp,
              std::vector<typename std::iterator_traits<RandomIt>::value_type>& tmp,
              size_t& min_gallop) {
    tmp.resize(right_len);
    std::move(base + left_len, base + left_len + right_len, tmp.begin());

    auto cursor1 = base + left_len - 1;
    auto cursor2 = tmp.begin() + right_len - 1;
    auto dest = base + left_len + right_len - 1;

    size_t left_remaining = left_len;
    size_t right_remaining = right_len;

    while (left_remaining > 0 && right_remaining > 1) {
        size_t count1 = 0;
        size_t count2 = 0;

        // One-at-a-time mode
        do {
            if (comp(*cursor2, *cursor1)) {
                *dest-- = std::move(*cursor1--);
                --left_remaining;
                ++count1;
                count2 = 0;
                if (left_remaining == 0) goto hi_epilogue;
            } else {
                *dest-- = std::move(*cursor2--);
                --right_remaining;
                ++count2;
                count1 = 0;
                if (right_remaining == 1) goto hi_epilogue;
            }
        } while ((count1 | count2) < min_gallop);

        // Gallop mode (reversed direction)
        do {
            count1 = left_remaining - gallop_right<RandomIt, Compare>(
                *cursor2, base, left_remaining, left_remaining - 1, comp);
            if (count1 > 0) {
                dest -= count1;
                cursor1 -= count1;
                left_remaining -= count1;
                std::move_backward(cursor1 + 1, cursor1 + 1 + count1, dest + 1 + count1);
                if (left_remaining == 0) goto hi_epilogue;
            }
            *dest-- = std::move(*cursor2--);
            --right_remaining;
            if (right_remaining == 1) goto hi_epilogue;

            count2 = right_remaining - gallop_left<decltype(cursor2), Compare>(
                *cursor1, tmp.begin(), right_remaining, right_remaining - 1, comp);
            if (count2 > 0) {
                dest -= count2;
                cursor2 -= count2;
                right_remaining -= count2;
                std::move(cursor2 + 1, cursor2 + 1 + count2, dest + 1);
                if (right_remaining <= 1) goto hi_epilogue;
            }
            *dest-- = std::move(*cursor1--);
            --left_remaining;
            if (left_remaining == 0) goto hi_epilogue;

            if (count1 >= MIN_GALLOP || count2 >= MIN_GALLOP) {
                if (min_gallop > 1) --min_gallop;
            } else {
                ++min_gallop;
            }
        } while (count1 >= MIN_GALLOP || count2 >= MIN_GALLOP);

        ++min_gallop;
    }

hi_epilogue:
    if (right_remaining == 1) {
        dest -= left_remaining;
        cursor1 -= left_remaining;
        std::move_backward(cursor1 + 1, cursor1 + 1 + left_remaining, dest + 1 + left_remaining);
        *dest = std::move(*cursor2);
    } else if (right_remaining > 0) {
        std::move(tmp.begin(), tmp.begin() + right_remaining, dest - right_remaining + 1);
    }
}

// Merge two adjacent runs with pre-merge trimming and galloping.
template<typename RandomIt, typename Compare>
void merge_with_gallop(RandomIt first, std::vector<Run>& runs, size_t i, Compare comp,
                       std::vector<typename std::iterator_traits<RandomIt>::value_type>& tmp,
                       size_t& min_gallop) {
    auto base1 = first + runs[i].base;
    size_t len1 = runs[i].length;
    auto base2 = first + runs[i + 1].base;
    size_t len2 = runs[i + 1].length;

    // Pre-merge trimming: skip elements already in position
    size_t k = gallop_right<RandomIt, Compare>(*base2, base1, len1, 0, comp);
    base1 += k;
    len1 -= k;

    if (len1 == 0) {
        runs[i].length += runs[i + 1].length;
        runs.erase(runs.begin() + i + 1);
        return;
    }

    len2 = gallop_left<RandomIt, Compare>(*(base1 + len1 - 1), base2, len2, len2 - 1, comp);

    if (len2 == 0) {
        runs[i].length += runs[i + 1].length;
        runs.erase(runs.begin() + i + 1);
        return;
    }

    if (len1 <= len2) {
        merge_lo(base1, len1, len2, comp, tmp, min_gallop);
    } else {
        merge_hi(base1, len1, len2, comp, tmp, min_gallop);
    }

    runs[i].length += runs[i + 1].length;
    runs.erase(runs.begin() + i + 1);
}

// PowerSort merge policy: merge runs whose power >= new run's power.
// Replaces classic TimSort's stack invariant heuristic with provably
// near-optimal merge ordering (CPython 3.11+, Munro & Wild 2018).
template<typename RandomIt, typename Compare>
void powersort_found_new_run(RandomIt first, std::vector<Run>& runs, size_t n2, size_t total_n,
                             Compare comp,
                             std::vector<typename std::iterator_traits<RandomIt>::value_type>& tmp,
                             size_t& min_gallop) {
    if (!runs.empty()) {
        size_t s1 = runs.back().base;
        size_t n1 = runs.back().length;
        int power = powerloop(s1, n1, n2, total_n);
        while (runs.size() > 1 && runs[runs.size() - 2].power > power) {
            merge_with_gallop(first, runs, runs.size() - 2, comp, tmp, min_gallop);
        }
        runs.back().power = power;
    }
}

// Force collapse all remaining runs
template<typename RandomIt, typename Compare>
void force_collapse(RandomIt first, std::vector<Run>& runs, Compare comp,
                    std::vector<typename std::iterator_traits<RandomIt>::value_type>& tmp,
                    size_t& min_gallop) {
    while (runs.size() > 1) {
        size_t n = runs.size() - 2;
        if (n > 0 && runs[n - 1].length < runs[n + 1].length) {
            --n;
        }
        merge_with_gallop(first, runs, n, comp, tmp, min_gallop);
    }
}

} // namespace detail

/**
 * PowerSort implementation (TimSort with optimal merge policy + galloping).
 * Exploits natural runs in data for O(n) best case on sorted/nearly-sorted data.
 * O(n log n) worst case. Stable sort.
 *
 * Merge policy: PowerSort (Munro & Wild 2018, CPython 3.11+)
 * Merge operation: galloping mode with exponential search
 */
template<typename RandomIt, typename Compare>
void timsort(RandomIt first, RandomIt last, Compare comp) {
    using ValueType = typename std::iterator_traits<RandomIt>::value_type;
    const size_t n = std::distance(first, last);
    if (n < 2) return;

    size_t min_run = compute_min_run_length(n);
    std::vector<detail::Run> runs;
    runs.reserve(40);

    std::vector<ValueType> tmp;
    tmp.reserve(n / 2);
    size_t min_gallop = detail::MIN_GALLOP;

    size_t n_remaining = n;
    auto cur = first;

    while (n_remaining > 0) {
        auto [run_len, _] = detail::count_run_and_make_ascending(cur, last, comp);

        if (run_len < min_run) {
            size_t force = std::min(n_remaining, min_run);
            detail::binary_insertion_sort(cur, cur + force, comp);
            run_len = force;
        }

        // PowerSort merge policy: before pushing, merge runs with >= power
        detail::powersort_found_new_run(first, runs, run_len, n, comp, tmp, min_gallop);

        runs.push_back({
            static_cast<size_t>(std::distance(first, cur)),
            run_len,
            0
        });

        cur += run_len;
        n_remaining -= run_len;
    }

    // Final collapse
    detail::force_collapse(first, runs, comp, tmp, min_gallop);
}

// Convenience overload for containers
template<typename Container, typename Compare>
void timsort(Container& c, Compare comp) {
    timsort(c.begin(), c.end(), comp);
}

/**
 * Parallel PowerSort implementation.
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

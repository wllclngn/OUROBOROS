#include "util/TimSort.hpp"
#include <cstddef>

namespace ouroboros::util {

// Computes the minimum run length for TimSort merging strategy.
// Goal: Choose a minrun such that N/minrun is a power of 2 or close to it.
size_t compute_min_run_length(size_t n) {
    size_t r = 0;
    while (n >= 32) {
        r |= (n & 1);
        n >>= 1;
    }
    return n + r;
}

} // namespace ouroboros::util

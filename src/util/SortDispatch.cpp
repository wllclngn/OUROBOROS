#include "util/SortDispatch.hpp"

#include <vector>

#include "sublimation_pack.h"
#include "sublimation_strings.h"

namespace ouroboros::util {

// The plain pack sorts malloc an n-slot scratch per call, and these run on the
// render path (the album grid re-sorts its visible slots every frame). A
// thread-local buffer grown to n reuses that capacity across frames and call
// sites; each thread gets its own, so no locking and no cross-thread aliasing.
// Threads that never sort never allocate it.
static uint64_t* frame_scratch(size_t n) {
    thread_local std::vector<uint64_t> buf;
    if (buf.size() < n) buf.resize(n);
    return buf.data();
}

void sort_by_key_f32(std::span<const float> keys, std::span<uint32_t> indices, bool descending) {
    if (indices.empty()) return;
    sublimation_pack_sort_f32_with_scratch(keys.data(), indices.data(), indices.size(),
                                           descending, frame_scratch(indices.size()));
}

void sort_by_key_u32(std::span<const uint32_t> keys, std::span<uint32_t> indices, bool descending) {
    if (indices.empty()) return;
    sublimation_pack_sort_u32_with_scratch(keys.data(), indices.data(), indices.size(),
                                           descending, frame_scratch(indices.size()));
}

void sort_by_key_i32(std::span<const int32_t> keys, std::span<uint32_t> indices, bool descending) {
    if (indices.empty()) return;
    sublimation_pack_sort_i32_with_scratch(keys.data(), indices.data(), indices.size(),
                                           descending, frame_scratch(indices.size()));
}

void sort_by_string(std::span<const char* const> arr, std::span<uint32_t> indices) {
    if (indices.empty()) return;
    // sublimation_strings_indices takes (const char**) but is read-only; the
    // const_cast is safe at the call boundary.
    sublimation_strings_indices(const_cast<const char**>(arr.data()),
                                indices.data(), indices.size());
}

}  // namespace ouroboros::util

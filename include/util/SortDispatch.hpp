#pragma once

#include <cstdint>
#include <span>

namespace ouroboros::util {

// Thin typed adapter over sublimation's pack-sort / string-sort C entry points.
// sublimation IS OUROBOROS's sort -- there is no backend choice and no fallback
// (montauk's shipped shape). See montauk/sublimation's README:
// https://github.com/wllclngn/montauk
//
// Index-permutation model: the caller fills `indices` with an identity
// permutation (0, 1, ..., n-1); each call reorders `indices` so that
// keys[indices[0]] <= keys[indices[1]] <= ... (or the reverse when descending).
// The `keys` / `arr` inputs are read-only and never moved. `indices.size()` is
// the element count; `keys`/`arr` must have at least that many entries.

void sort_by_key_f32(std::span<const float> keys, std::span<uint32_t> indices, bool descending);
void sort_by_key_u32(std::span<const uint32_t> keys, std::span<uint32_t> indices, bool descending);
void sort_by_key_i32(std::span<const int32_t> keys, std::span<uint32_t> indices, bool descending);

// Lexicographic (UTF-8 byte-order) sort of a pointer array; reorders `indices`.
// Unstable -- callers needing a total order encode the full key into the strings.
void sort_by_string(std::span<const char* const> arr, std::span<uint32_t> indices);

}  // namespace ouroboros::util

#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

namespace ouroboros::util {

/**
 * Boyer-Moore-Horspool (BMH) literal search - O(1) extra space
 * Fast average-case search using only bad-character table.
 * Based on optimal µEmacs implementation.
 *
 * Average case: O(n/m) sublinear performance
 * Worst case: O(n*m) (extremely rare)
 * Space: O(1) - fixed 256-byte alphabet table
 */
class BoyerMooreSearch {
public:
    /**
     * Initialize Boyer-Moore context with pattern.
     * @param pattern The search pattern
     * @param case_sensitive Whether to perform case-sensitive search
     */
    BoyerMooreSearch(const std::string& pattern, bool case_sensitive = false);

    /**
     * Search for pattern in text using Boyer-Moore algorithm.
     * @param text The text to search in
     * @param start_pos Starting position in text (default 0)
     * @return Position of first match, or -1 if not found
     */
    int search(const std::string& text, int start_pos = 0) const;

private:
    static constexpr int ALPHABET_SIZE = 256;
    static constexpr int MAX_PATTERN = 256;

    // Bad character skip table (no heap allocation)
    int bad_char_[ALPHABET_SIZE];

    // Pattern information
    std::string pattern_;
    int pattern_len_;
    bool case_sensitive_;

    // Fast character normalization
    inline unsigned char normalize_char(unsigned char c) const {
        return case_sensitive_ ? c : static_cast<unsigned char>(tolower(c));
    }

    // Precompute bad character rule (BMH variant)
    void compute_bad_char();
};

} // namespace ouroboros::util

#include "util/BoyerMoore.hpp"
#include <cstring>
#include <cctype>

namespace ouroboros::util {

BoyerMooreSearch::BoyerMooreSearch(const std::string& pattern, bool case_sensitive)
    : pattern_(pattern),
      pattern_len_(static_cast<int>(pattern.length())),
      case_sensitive_(case_sensitive) {

    if (pattern_len_ <= 0 || pattern_len_ > MAX_PATTERN) {
        pattern_len_ = 0;
        return;
    }

    compute_bad_char();
}

void BoyerMooreSearch::compute_bad_char() {
    // Initialize all characters to pattern length (maximum shift)
    for (int i = 0; i < ALPHABET_SIZE; ++i) {
        bad_char_[i] = pattern_len_;
    }

    // Set actual shift distances for characters in pattern (except last)
    // This is the BMH optimization: only use last occurrence
    for (int i = 0; i < pattern_len_ - 1; ++i) {
        unsigned char c = normalize_char(static_cast<unsigned char>(pattern_[i]));
        bad_char_[c] = pattern_len_ - 1 - i;
    }
}

int BoyerMooreSearch::search(const std::string& text, int start_pos) const {
    int text_len = static_cast<int>(text.length());
    int m = pattern_len_;
    int n = text_len;

    // Validation
    if (m == 0 || start_pos < 0 || start_pos >= n || m > n) {
        return -1;
    }

    // Boyer-Moore-Horspool algorithm
    int i = start_pos;
    while (i <= n - m) {
        int j = m - 1;

        // Compare pattern with text from right to left
        while (j >= 0 &&
               normalize_char(static_cast<unsigned char>(text[i + j])) ==
               normalize_char(static_cast<unsigned char>(pattern_[j]))) {
            --j;
        }

        // Match found
        if (j < 0) {
            return i;
        }

        // Mismatch: use bad character rule to skip ahead
        unsigned char bad = normalize_char(static_cast<unsigned char>(text[i + m - 1]));
        int bad_shift = bad_char_[bad];
        int shift = bad_shift > 0 ? bad_shift : 1;
        i += shift;
    }

    return -1;  // Pattern not found
}

} // namespace ouroboros::util

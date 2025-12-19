#pragma once

#include <string>
#include <unicode/unistr.h>
#include <unicode/translit.h>
#include <unicode/normlzr.h>

namespace ouroboros::util {

/// Normalize text for Unicode-aware case-insensitive search
/// Transliterates diacritics to ASCII equivalents (Björk → bjork, José → jose)
/// and converts to lowercase for searching
inline std::string normalize_for_search(const std::string& text) {
    if (text.empty()) {
        return text;
    }

    // Convert UTF-8 string to ICU UnicodeString
    icu::UnicodeString unicode_text = icu::UnicodeString::fromUTF8(text);

    // Transliterate to Latin-ASCII (removes diacritics: ö → o, é → e)
    // NFD: Canonical decomposition (splits ö into o + combining diaeresis)
    // [:Nonspacing Mark:] Remove: Removes combining characters
    // NFC: Canonical composition (normalizes back to standard form)
    // Latin-ASCII: Converts Latin characters to pure ASCII
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::Transliterator> trans(
        icu::Transliterator::createInstance(
            "NFD; [:Nonspacing Mark:] Remove; NFC; Latin-ASCII",
            UTRANS_FORWARD,
            status
        )
    );

    if (U_FAILURE(status) || !trans) {
        // Fallback: just lowercase without transliteration
        std::string result;
        unicode_text.toLower().toUTF8String(result);
        return result;
    }

    // Apply transliteration
    trans->transliterate(unicode_text);

    // Convert to lowercase and back to UTF-8 std::string
    std::string result;
    unicode_text.toLower().toUTF8String(result);

    return result;
}

/// Case-insensitive string comparison using ICU
/// Returns: <0 if a < b, 0 if a == b, >0 if a > b (like strcmp)
inline int case_insensitive_compare(const std::string& a, const std::string& b) {
    icu::UnicodeString ua = icu::UnicodeString::fromUTF8(a);
    icu::UnicodeString ub = icu::UnicodeString::fromUTF8(b);

    // Case-fold both strings (more robust than toLower for comparison)
    ua.foldCase();
    ub.foldCase();

    return ua.compare(ub);
}

} // namespace ouroboros::util

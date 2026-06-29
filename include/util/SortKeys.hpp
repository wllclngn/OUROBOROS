#pragma once

#include <string>

namespace ouroboros::util {

// Field separator for composite byte-order sort keys: below any printable byte,
// so a shorter field sorts before a longer one sharing its prefix -- which
// preserves the per-field comparison order of the original multi-key comparators.
inline constexpr char SORT_KEY_SEP = '\x01';

// Zero-pad a non-negative int to a fixed width so byte order == numeric order
// inside a composite key.
inline std::string zeropad(int value, int width) {
    if (value < 0) value = 0;
    std::string s = std::to_string(value);
    if (static_cast<int>(s.size()) < width) s.insert(0, width - s.size(), '0');
    return s;
}

// Composite track key matching the comparator (artist, date, [dir], track number).
// `folded_artist` must already be fold_case(artist_sort_key(artist)) so byte order
// reproduces case_insensitive_compare; `dir` is only appended when group_by_dir.
inline std::string track_sort_key(const std::string& folded_artist,
                                  const std::string& date,
                                  const std::string& dir,
                                  int track_number,
                                  bool group_by_dir) {
    std::string k = folded_artist;
    k += SORT_KEY_SEP; k += date;
    if (group_by_dir) { k += SORT_KEY_SEP; k += dir; }
    k += SORT_KEY_SEP; k += zeropad(track_number, 6);
    return k;
}

// Composite album key matching the comparator (primary, [year], title). `primary`
// and `folded_title` must already be fold_case'd; `year` is the zero-padded
// year_to_int value or empty when not sorting by year.
inline std::string album_sort_key(const std::string& primary,
                                  const std::string& year_field,
                                  const std::string& folded_title) {
    std::string k = primary;
    k += SORT_KEY_SEP; k += year_field;
    k += SORT_KEY_SEP; k += folded_title;
    return k;
}

}  // namespace ouroboros::util

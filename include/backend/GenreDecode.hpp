#pragma once

#include <string>

namespace ouroboros::backend {

// Resolve a numeric ID3 genre reference to a name. ID3 stores genre either as a
// plain string or as a legacy reference into the ID3v1 genre table:
//   "(52)"            -> "Electronic"
//   "52"              -> "Electronic"
//   "(52)Ambient Dub" -> "Ambient Dub"   (ID3v2.3 refinement string wins)
// Any non-numeric value, or an out-of-range index, passes through unchanged.
[[nodiscard]] std::string decode_id3_genre(const std::string& genre);

}  // namespace ouroboros::backend

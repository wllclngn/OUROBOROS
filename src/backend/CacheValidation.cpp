#include "backend/Library.hpp"

#include <filesystem>
#include <unordered_set>

// Pure TIER 0 cache-validation decision, split out from Library.cpp so it has no
// heavy dependencies (no metadata parser, no decoders) and can be unit-tested with
// synthetic inputs. See Library.hpp for the contract.

namespace ouroboros::backend {

Library::CacheValidationResult Library::classify_tier0(
    const std::vector<std::string>& scanned_files,
    const std::unordered_map<std::string, std::time_t>& scanned_file_mtimes,
    const std::unordered_map<std::string, std::time_t>& scanned_dirs,
    const std::unordered_map<std::string, model::Track>& cached_tracks) {

    // Additions: any on-disk file not present in the cache.
    for (const auto& file_path : scanned_files) {
        if (cached_tracks.find(file_path) == cached_tracks.end()) {
            return CacheValidationResult::CountMismatch;
        }
    }

    // In-place edits: a cached file whose on-disk mtime is newer than what the
    // cache recorded. This is the case path-membership alone cannot see -- a
    // retag leaves the path and count unchanged.
    for (const auto& file_path : scanned_files) {
        auto cit = cached_tracks.find(file_path);
        if (cit == cached_tracks.end()) continue;
        auto mit = scanned_file_mtimes.find(file_path);
        if (mit != scanned_file_mtimes.end() && cit->second.file_mtime > 0 &&
            mit->second > cit->second.file_mtime) {
            return CacheValidationResult::MetadataMismatch;
        }
    }

    // Removals: a cached file that is gone, but only when its directory was
    // actually scanned this pass. A directory absent from scanned_dirs is an
    // unmounted/unavailable drive, not a deletion -- the cumulative cache keeps
    // those entries.
    std::unordered_set<std::string> scanned_set(scanned_files.begin(), scanned_files.end());
    for (const auto& [path, track] : cached_tracks) {
        (void)track;
        std::string dir = std::filesystem::path(path).parent_path().string();
        if (scanned_dirs.find(dir) != scanned_dirs.end() &&
            scanned_set.find(path) == scanned_set.end()) {
            return CacheValidationResult::MetadataMismatch;
        }
    }

    return CacheValidationResult::Valid;
}

}  // namespace ouroboros::backend

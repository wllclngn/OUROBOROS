#pragma once

#include "model/Snapshot.hpp"
#include "util/DirectoryScanner.hpp"
#include <filesystem>
#include <vector>
#include <optional>
#include <unordered_map>
#include <functional>
#include <memory>

namespace ouroboros::backend {

class Library {
public:
    Library();

    [[deprecated("use set_music_directories() instead")]]
    void set_music_directory(const std::filesystem::path& dir);
    void set_music_directories(const std::vector<std::filesystem::path>& dirs);
    void scan_directory(const std::function<void(int scanned, int total)>& progress_callback = nullptr);

    [[nodiscard]] std::vector<model::Track> get_all_tracks() const;
    [[nodiscard]] std::optional<model::Track> get_track_by_path(const std::filesystem::path& path) const;

    [[nodiscard]] size_t get_track_count() const;
    [[nodiscard]] bool is_scanning() const;

    // Persistence (monolithic .bin cache)
    [[nodiscard]] bool save_to_cache(const std::filesystem::path& cache_path) const;
    [[nodiscard]] bool load_from_cache(const std::filesystem::path& cache_path);
    void set_tracks(const std::vector<model::Track>& tracks);

    // Multi-tier cache validation
    enum class CacheValidationResult {
        Valid,
        CountMismatch,
        MetadataMismatch,
        MissingFiles,
        GenericFailure
    };

    [[nodiscard]] CacheValidationResult validate_cache_tier0(const std::filesystem::path& cache_path);
    [[nodiscard]] std::vector<std::string> find_dirty_directories(
        const std::unordered_map<std::string, std::time_t>& current_dir_mtimes,
        const std::unordered_map<std::string, std::time_t>& cached_dir_mtimes
    );
    void scan_for_changes(
        const std::vector<std::string>& changed_files,
        const std::vector<std::string>& deleted_files,
        const std::function<void(int, int)>& progress_callback = nullptr
    );

    // Accessors for optimization fields
    [[nodiscard]] const std::unordered_map<std::string, std::time_t>& get_dir_mtimes() const { return dir_mtimes_; }
    [[nodiscard]] uint64_t get_tree_hash() const { return last_tree_hash_; }

private:
    std::vector<std::filesystem::path> music_dirs_;  // Multiple directories
    // Map path -> Track for O(1) lookup
    std::unordered_map<std::string, model::Track> tracks_;
    bool is_scanning_ = false;

    // Multi-tier optimization (CACHE_VERSION 3)
    std::unordered_map<std::string, std::time_t> dir_mtimes_;  // Directory → mtime
    uint64_t last_tree_hash_ = 0;                               // Tree hash from last scan
    std::time_t cache_timestamp_ = 0;                           // When cache was built

    // Performance: reuse scan results from TIER 0 validation to avoid double scanning
    std::optional<util::DirectoryScanner::ScanResult> cached_scan_result_;
};

}  // namespace ouroboros::backend

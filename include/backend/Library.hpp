#pragma once

#include "model/Snapshot.hpp"
#include "backend/HierarchicalCache.hpp"
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

    void set_music_directory(const std::filesystem::path& dir);
    void scan_directory(const std::function<void(int scanned, int total)>& progress_callback = nullptr);
    
    std::vector<model::Track> get_all_tracks() const;
    std::optional<model::Track> get_track_by_path(const std::filesystem::path& path) const;
    
    size_t get_track_count() const;
    bool is_scanning() const;

    // Persistence
    bool save_to_cache(const std::filesystem::path& cache_path) const;
    bool load_from_cache(const std::filesystem::path& cache_path);
    bool load_from_hierarchical_cache(const std::filesystem::path& cache_root);
    void set_tracks(const std::vector<model::Track>& tracks);

    // Multi-tier cache validation (Phase 3)
    enum class CacheValidationResult {
        Valid,
        CountMismatch,
        MetadataMismatch,
        MissingFiles,
        GenericFailure
    };
    
    CacheValidationResult validate_cache_tier0(const std::filesystem::path& cache_path);
    std::vector<std::string> find_dirty_directories(
        const std::unordered_map<std::string, std::time_t>& current_dir_mtimes,
        const std::unordered_map<std::string, std::time_t>& cached_dir_mtimes
    );
    void scan_for_changes(
        const std::vector<std::string>& changed_files,
        const std::vector<std::string>& deleted_files,
        const std::function<void(int, int)>& progress_callback = nullptr
    );

    // Accessors for optimization fields
    const std::unordered_map<std::string, std::time_t>& get_dir_mtimes() const { return dir_mtimes_; }
    uint64_t get_tree_hash() const { return last_tree_hash_; }

    // Hierarchical Cache Operations
    void generate_hierarchical_caches(const std::filesystem::path& cache_root);
    void set_cache_root(const std::filesystem::path& cache_root);
    HierarchicalCache& get_hierarchical_cache() { return hierarchical_cache_; }
    const HierarchicalCache& get_hierarchical_cache() const { return hierarchical_cache_; }

private:
    std::filesystem::path music_dir_;
    // Map path -> Track for O(1) lookup
    std::unordered_map<std::string, model::Track> tracks_;
    bool is_scanning_ = false;

    // Multi-tier optimization (CACHE_VERSION 3)
    std::unordered_map<std::string, std::time_t> dir_mtimes_;  // Directory → mtime
    uint64_t last_tree_hash_ = 0;                               // Tree hash from last scan
    std::time_t cache_timestamp_ = 0;                           // When cache was built

    // Hierarchical Cache System
    HierarchicalCache hierarchical_cache_;
};

}  // namespace ouroboros::backend

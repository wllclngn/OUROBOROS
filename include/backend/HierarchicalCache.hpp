#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <fstream>
#include <chrono>
#include "model/Snapshot.hpp"

namespace ouroboros::backend {

// Global index entry (lightweight)
struct IndexEntry {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    uint32_t directory_index;
    std::string artwork_hash;
};

// Directory metadata in manifest
struct DirectoryMetadata {
    std::string path;
    uint32_t track_count;
    std::string cache_file;
    std::string artwork_file;
    std::chrono::system_clock::time_point last_scanned;
    std::string tree_hash;
};

// Cache manifest structure
struct CacheManifest {
    uint32_t version = 1;
    std::string music_root;
    std::chrono::system_clock::time_point last_global_scan;
    std::vector<DirectoryMetadata> directories;
};

// Global index structure
struct GlobalIndex {
    std::vector<std::string> directories;
    std::vector<IndexEntry> tracks;
};

/**
 * Hierarchical Cache System
 *
 * Manages per-directory caches instead of monolithic cache.
 * Only loads caches for directories being actively browsed.
 */
class HierarchicalCache {
public:
    HierarchicalCache();
    ~HierarchicalCache() = default;

    // Initialization
    void set_cache_root(const std::filesystem::path& cache_root);
    void set_music_root(const std::filesystem::path& music_root);

    // Cache Generation (called after library scan)
    void generate_hierarchical_caches(
        const std::unordered_map<std::string, model::Track>& all_tracks
    );

    // Cache Loading/Unloading
    std::unordered_map<std::string, model::Track> load_directory(
        const std::filesystem::path& directory
    );

    void unload_directory(const std::filesystem::path& directory);

    void unload_all();

    // Global Index Operations
    GlobalIndex load_global_index();

    std::vector<std::string> search_index(
        const GlobalIndex& index,
        const std::string& query
    );

    // Manifest Operations
    CacheManifest load_manifest();
    void save_manifest(const CacheManifest& manifest);

    // Directory Utilities
    std::vector<std::filesystem::path> get_top_level_directories() const;

    std::filesystem::path get_top_level_directory_for_track(
        const std::string& track_path
    ) const;

    // Cache Validation
    bool is_cache_valid(const std::filesystem::path& directory);

    // Migration from monolithic cache
    bool migrate_from_monolithic(
        const std::filesystem::path& old_library_bin,
        const std::filesystem::path& old_artwork_cache
    );

    // Statistics
    size_t get_loaded_directory_count() const { return loaded_directories_.size(); }
    std::vector<std::string> get_loaded_directory_names() const;

private:
    // Internal helpers
    void generate_directory_cache(
        const std::filesystem::path& directory,
        const std::vector<model::Track>& tracks
    );

    void generate_global_index(
        const std::vector<std::filesystem::path>& directories,
        const std::unordered_map<std::string, model::Track>& all_tracks
    );

    std::string compute_directory_tree_hash(const std::filesystem::path& directory);

    // Binary I/O
    void write_directory_metadata(
        std::ofstream& out,
        const std::vector<model::Track>& tracks
    );

    std::vector<model::Track> read_directory_metadata(std::ifstream& in);

    void write_global_index(const GlobalIndex& index);

    // Member variables
    std::filesystem::path cache_root_;
    std::filesystem::path music_root_;

    // Loaded directories (LRU cache - currently size=1 for aggressive unload)
    std::unordered_map<std::string, std::unordered_map<std::string, model::Track>> loaded_directories_;

    // Magic numbers for binary formats
    static constexpr uint64_t GLOBAL_INDEX_MAGIC = 0x4F55524F49445831; // 'OUROIDX1'
    static constexpr uint64_t DIRECTORY_CACHE_MAGIC = 0x4F55524F44495231; // 'OURODIR1'
    static constexpr uint32_t FORMAT_VERSION = 1;
};

} // namespace ouroboros::backend

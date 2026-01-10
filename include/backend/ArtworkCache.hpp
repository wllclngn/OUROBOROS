#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <filesystem>
#include <cstdint>

namespace ouroboros::backend {

struct ArtworkEntry {
    std::vector<uint8_t> data;  // Raw JPEG/PNG bytes (compressed)
    std::string mime_type;       // "image/jpeg" or "image/png"
    std::string source_dir;      // Album directory this artwork came from
    size_t ref_count = 0;        // Number of tracks referencing this artwork

    ArtworkEntry() = default;
    ArtworkEntry(std::vector<uint8_t> d, std::string mime, std::string dir, size_t refs = 0)
        : data(std::move(d)), mime_type(std::move(mime)), source_dir(std::move(dir)), ref_count(refs) {}
};

/**
 * Global artwork cache with content-addressed storage.
 *
 * Stores artwork indexed by SHA-256 hash for O(1) lookups and automatic deduplication.
 * Thread-safe singleton accessed during library scan and UI rendering.
 *
 * Cache persistence: ~/.cache/ouroboros/artwork.cache
 */
class ArtworkCache {
public:
    static ArtworkCache& instance();

    // Store artwork (called during library scan)
    // Validates image before storing to reject corrupt data
    void store(const std::string& hash, std::vector<uint8_t> data, const std::string& mime_type, const std::string& source_dir);

    // O(1) lookup by hash
    // Returns nullptr if not found
    const ArtworkEntry* get(const std::string& hash) const;

    // O(1) lookup by directory - returns hash or nullptr
    const std::string* get_hash_for_dir(const std::string& dir) const;

    // Increment reference count (called when track references artwork)
    void ref(const std::string& hash);

    // Decrement reference count, evict if 0 (called when track is removed)
    void unref(const std::string& hash);

    // Persist cache to disk (only if dirty)
    bool save(const std::filesystem::path& cache_path);

    // Check if cache has unsaved changes
    bool is_dirty() const;

    // Load cache from disk
    bool load(const std::filesystem::path& cache_path);

    // Statistics
    size_t size() const;
    size_t memory_usage() const;  // Total bytes in cache

    // Clear all entries (used for testing)
    void clear();

private:
    ArtworkCache() = default;
    ~ArtworkCache() = default;

    // Prevent copy/move
    ArtworkCache(const ArtworkCache&) = delete;
    ArtworkCache& operator=(const ArtworkCache&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ArtworkEntry> cache_;  // hash -> artwork
    std::unordered_map<std::string, std::string> dir_to_hash_;  // dir -> hash
    bool dirty_ = false;  // Track if cache needs saving

    // Cache file format magic/version
    static constexpr uint64_t CACHE_MAGIC = 0x4F55524F41525431ULL;  // 'OUROART1'
    static constexpr uint32_t CACHE_VERSION = 2;  // Bumped for source_dir field
};

}  // namespace ouroboros::backend

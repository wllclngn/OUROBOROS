#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <list>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include "ui/ImageRenderer.hpp"  // For CachedFormat

namespace ouroboros::ui {

// Decoded artwork ready for terminal rendering
struct DecodedArtwork {
    const uint8_t* data;
    size_t data_size;
    int width;
    int height;
    CachedFormat format;
    std::string hash;  // Original content hash for image_id generation
};

// Priority-based artwork request
struct WindowRequest {
    std::string path;
    int priority;        // Lower = higher priority (distance from cursor)
    uint64_t timestamp;  // Tie-breaker
    int target_width;    // Decode target dimensions
    int target_height;
};

// Comparator: lower priority value = higher queue priority
struct WindowRequestComparator {
    bool operator()(const WindowRequest& a, const WindowRequest& b) const {
        if (a.priority != b.priority)
            return a.priority > b.priority;  // Lower priority value wins
        return a.timestamp > b.timestamp;    // Earlier timestamp wins
    }
};

class ArtworkWindow {
public:
    static ArtworkWindow& instance();

    // Request artwork loading with priority (lower = higher priority)
    // If notify=false, call flush_requests() after batching all requests
    void request(const std::string& path, int priority, int width_cols, int height_rows, bool notify = true);

    // Notify workers after batching requests (call after multiple request() calls with notify=false)
    void flush_requests();

    // Get decoded pixels (nullptr if not ready)
    const DecodedArtwork* get_decoded(const std::string& path, int width_cols, int height_rows);

    // Clear everything (called on big jump)
    void reset();

    // Check if new artwork is ready (for render trigger)
    bool has_updates();
    void clear_updates();

    // Get memory stats for debugging
    size_t get_total_bytes() const { return total_bytes_.load(); }
    size_t get_entry_count() const;

    ~ArtworkWindow();

private:
    ArtworkWindow();
    ArtworkWindow(const ArtworkWindow&) = delete;
    ArtworkWindow& operator=(const ArtworkWindow&) = delete;

    // Cache key: album directory + dimensions (content-addressed by directory)
    // All tracks in same album share artwork, so key by directory not file path
    struct CacheKey {
        std::string album_dir;  // Parent directory, not individual track path
        int width;
        int height;

        bool operator==(const CacheKey& other) const {
            return album_dir == other.album_dir && width == other.width && height == other.height;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const {
            return std::hash<std::string>()(k.album_dir) ^
                   (std::hash<int>()(k.width) << 1) ^
                   (std::hash<int>()(k.height) << 2);
        }
    };

    // Cache entry: holds both jpeg reference and decoded pixels
    struct Entry {
        std::string hash;                    // For ArtworkCache lookup
        std::vector<uint8_t> decoded_pixels; // RGB or PNG for terminal
        int decoded_width = 0;
        int decoded_height = 0;
        CachedFormat format = CachedFormat::RGB;
        bool ready = false;                  // True when decoded and ready to render
        std::list<CacheKey>::iterator lru_iter;
    };

    // Cache
    std::unordered_map<CacheKey, Entry, CacheKeyHash> cache_;
    std::list<CacheKey> lru_list_;  // Front = newest, back = oldest
    std::mutex cache_mutex_;

    // Memory tracking
    std::atomic<size_t> total_bytes_{0};
    size_t memory_limit_bytes_;

    // Request queue
    std::priority_queue<WindowRequest, std::vector<WindowRequest>, WindowRequestComparator> request_queue_;
    std::unordered_set<std::string> pending_paths_;  // Deduplication
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Worker threads
    std::vector<std::thread> workers_;
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> has_updates_{false};

    // Terminal cell size for pixel calculations
    int cell_width_ = 8;
    int cell_height_ = 16;

    // Worker constants
    static constexpr size_t NUM_WORKERS = 4;
    static constexpr auto WORKER_TIMEOUT = std::chrono::milliseconds(500);

    void worker_thread();
    void evict_until_under_limit();
    void detect_cell_size();

    // Decode helper (moved from ImageRenderer)
    struct DecodeResult {
        std::vector<uint8_t> pixels;
        int width;
        int height;
        CachedFormat format;
        bool valid;
    };
    DecodeResult decode_jpeg(const std::vector<uint8_t>& jpeg_data, int target_w, int target_h);
};

} // namespace ouroboros::ui

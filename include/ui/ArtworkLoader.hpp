#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stack>
#include <queue>
#include <list>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

namespace ouroboros::ui {

struct ArtworkData {
    std::vector<uint8_t> jpeg_data;
    std::string path;
    std::string mime_type;
    std::string hash;  // SHA-256 hash for content-addressed caching
    bool loaded = false;
};

// LRU cache entry with viewport protection
struct CacheEntry {
    ArtworkData data;
    std::chrono::steady_clock::time_point last_access;
    bool in_viewport = false;  // Protected from eviction
};

// Viewport state for dirty flag pattern
struct ViewportState {
    std::unordered_set<std::string> visible_paths;
    int last_scroll_offset = -1;
    int last_selected_index = -1;
    uint64_t state_hash = 0;

    bool has_changed(int scroll, int selected) const {
        return last_scroll_offset != scroll || last_selected_index != selected;
    }

    void update(int scroll, int selected) {
        last_scroll_offset = scroll;
        last_selected_index = selected;
        // Simple hash for change detection
        state_hash = (static_cast<uint64_t>(scroll) << 32) | static_cast<uint64_t>(selected);
    }

    uint64_t hash() const { return state_hash; }
};

// Priority-based artwork request for radial rendering
struct ArtworkRequest {
    std::string track_path;
    int distance;            // Manhattan distance from selection
    int viewport_tier;       // 0=visible, 1=prefetch
    uint64_t timestamp;      // For tie-breaking
    uint64_t viewport_generation;  // Invalidation token
};

// Comparator for priority queue (lower values = higher priority)
struct RequestComparator {
    bool operator()(const ArtworkRequest& a, const ArtworkRequest& b) const {
        // Lower tier = higher priority (visible before prefetch)
        if (a.viewport_tier != b.viewport_tier)
            return a.viewport_tier > b.viewport_tier;

        // Lower distance = higher priority (closer to cursor)
        if (a.distance != b.distance)
            return a.distance > b.distance;

        // Newer request = higher priority (tie-breaker)
        return a.timestamp < b.timestamp;
    }
};

class ArtworkLoader {
public:
    static ArtworkLoader& instance();

    // Request artwork loading (non-blocking)
    // Returns immediately, artwork will be loaded in background
    void request_artwork(const std::string& track_path);

    // Request artwork with priority for radial rendering
    // distance: Manhattan distance from current selection
    // viewport_tier: 0=visible (high priority), 1=prefetch (low priority)
    void request_artwork_with_priority(
        const std::string& track_path,
        int distance,
        int viewport_tier
    );

    // Check if artwork is ready for a track
    // If ready, returns the data and marks it as consumed
    bool get_artwork(const std::string& track_path, std::vector<uint8_t>& out_data);

    // Check if any artwork has finished loading (for triggering re-renders)
    bool has_pending_updates();
    void clear_pending_updates();

    // Clear pending requests (queue + in-flight tracking)
    void clear_requests();

    // Clear loaded cache (hash references)
    void clear_cache();

    // Clear both requests and cache (for library refresh/shutdown)
    void clear_all();

    // Update viewport state for dirty flag pattern
    void update_viewport(
        int scroll_offset,
        int selected_index,
        const std::vector<std::string>& visible_paths
    );

    // Check if path is currently in viewport
    bool is_in_viewport(const std::string& path) const;

    ~ArtworkLoader();

    /// Update cache context based on queue state (for smart eviction)
    /// Keeps current track + next N tracks, evicts everything else
    void update_queue_context(
        const std::optional<std::string>& current_path,
        const std::vector<std::string>& next_paths
    );

    /// Zero-copy artwork access - returns pointer to cached data (or nullptr if not ready)
    const ArtworkData* get_artwork_ref(const std::string& path);

    /// Check if artwork is fully loaded into memory (not just path cached)
    bool is_artwork_loaded(const std::string& track_path);

private:
    ArtworkLoader();
    ArtworkLoader(const ArtworkLoader&) = delete;
    ArtworkLoader& operator=(const ArtworkLoader&) = delete;

    void worker_thread();

    // LRU eviction helpers
    void evict_if_needed();
    void mark_accessed(const std::string& path);

    std::thread worker_;
    std::atomic<bool> should_stop_{false};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::priority_queue<ArtworkRequest, std::vector<ArtworkRequest>, RequestComparator> request_queue_;  // Priority queue: Process by viewport tier, then distance from selection

    std::mutex cache_mutex_;
    std::unordered_map<std::string, CacheEntry> cache_;  // Changed to CacheEntry for LRU tracking
    std::list<std::string> lru_list_;  // Tracks access order (front = newest)
    static constexpr size_t MAX_LOADER_CACHE_SIZE = 500;  // Tunable cache limit

    // Request deduplication: Track in-flight requests to prevent duplicates
    std::mutex pending_mutex_;
    std::unordered_set<std::string> pending_requests_;

    // Viewport state tracking for dirty flag pattern
    mutable std::mutex viewport_mutex_;
    ViewportState viewport_state_;

    // Generation token for request invalidation
    std::atomic<uint64_t> viewport_generation_{0};

    std::atomic<bool> has_updates_{false};
};

} // namespace ouroboros::ui

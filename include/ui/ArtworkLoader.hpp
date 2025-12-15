#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>
#include <unordered_map>

namespace ouroboros::ui {

struct ArtworkData {
    std::vector<uint8_t> jpeg_data;
    std::string path;
    std::string mime_type;
    std::string hash;  // SHA-256 hash for content-addressed caching
    bool loaded = false;
};

class ArtworkLoader {
public:
    static ArtworkLoader& instance();

    // Request artwork loading (non-blocking)
    // Returns immediately, artwork will be loaded in background
    void request_artwork(const std::string& track_path);

    // Check if artwork is ready for a track
    // If ready, returns the data and marks it as consumed
    bool get_artwork(const std::string& track_path, std::vector<uint8_t>& out_data);

    // Check if any artwork has finished loading (for triggering re-renders)
    bool has_pending_updates();
    void clear_pending_updates();

    // Clear all pending requests
    void clear_requests();

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

    std::thread worker_;
    std::atomic<bool> should_stop_{false};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::string> request_queue_;

    std::mutex cache_mutex_;
    std::unordered_map<std::string, ArtworkData> cache_;

    std::atomic<bool> has_updates_{false};
};

} // namespace ouroboros::ui

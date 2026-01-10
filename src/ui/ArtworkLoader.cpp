#include "ui/ArtworkLoader.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/ArtworkCache.hpp"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include "util/Logger.hpp"

namespace ouroboros::ui {

ArtworkLoader& ArtworkLoader::instance() {
    static ArtworkLoader instance;
    return instance;
}

ArtworkLoader::ArtworkLoader() {
    // Start initial worker threads
    ouroboros::util::Logger::info("ArtworkLoader: Starting with " + std::to_string(MIN_WORKERS) +
                                  " worker(s), max=" + std::to_string(get_max_workers()) +
                                  " (hardware threads)");
    for (size_t i = 0; i < MIN_WORKERS; ++i) {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back(&ArtworkLoader::worker_thread, this);
    }
}

ArtworkLoader::~ArtworkLoader() {
    should_stop_ = true;
    queue_cv_.notify_all();  // Wake all waiting workers

    // Wait for all workers to exit
    while (active_workers_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Join all worker threads
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }
    ouroboros::util::Logger::info("ArtworkLoader: All workers stopped");
}

void ArtworkLoader::request_artwork(const std::string& track_path) {
    // Wrapper for backward compatibility - uses default priority
    request_artwork_with_priority(track_path, 0, 0);
}

void ArtworkLoader::request_artwork_with_priority(
    const std::string& track_path,
    int distance,
    int viewport_tier
) {
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        // Check if already loaded
        auto it = cache_.find(track_path);
        if (it != cache_.end() && it->second.data.loaded) {
            mark_accessed(track_path);  // Update LRU
            return;
        }

        // Mark as pending (create empty CacheEntry if doesn't exist)
        if (it == cache_.end()) {
            lru_list_.push_front(track_path);
            CacheEntry entry;
            entry.last_access = std::chrono::steady_clock::now();
            entry.lru_iter = lru_list_.begin();  // Store iterator for O(1) LRU
            cache_[track_path] = entry;
        }
    }

    // Deduplication: Check if already in flight
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_requests_.find(track_path) != pending_requests_.end()) {
            return;  // Already queued, skip duplicate
        }
        pending_requests_.insert(track_path);
    }

    // Create prioritized request with generation token
    ArtworkRequest req;
    req.track_path = track_path;
    req.distance = distance;
    req.viewport_tier = viewport_tier;
    req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.viewport_generation = viewport_generation_.load();  // Stamp with current generation

    ouroboros::util::Logger::debug("ArtworkLoader: Queuing request - path=" + track_path +
                                   ", distance=" + std::to_string(distance) +
                                   ", tier=" + std::to_string(viewport_tier) +
                                   ", gen=" + std::to_string(req.viewport_generation));

    size_t queue_size;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        request_queue_.push(req);
        queue_size = request_queue_.size();
    }
    queue_cv_.notify_one();

    // Dynamic scaling: spawn more workers if queue is deep
    size_t current = active_workers_.load();
    if (queue_size > SPAWN_THRESHOLD * current && current < get_max_workers()) {
        // Spawn new worker - it will register itself via active_workers_.fetch_add(1)
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back(&ArtworkLoader::worker_thread, this);
        ouroboros::util::Logger::info("ArtworkLoader: Spawning worker (queue_size=" +
                                     std::to_string(queue_size) + ", active=" +
                                     std::to_string(current) + ")");
    }
}

bool ArtworkLoader::get_artwork(const std::string& track_path, std::vector<uint8_t>& out_data) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(track_path);

    if (it != cache_.end() && it->second.data.loaded && !it->second.data.hash.empty()) {
        // Fetch from global cache using hash
        auto& global_cache = backend::ArtworkCache::instance();
        const auto* cached_entry = global_cache.get(it->second.data.hash);
        if (cached_entry) {
            out_data = cached_entry->data;
            mark_accessed(track_path);  // Update LRU on access
            return true;
        }
    }
    return false;
}

bool ArtworkLoader::has_pending_updates() {
    return has_updates_.load();
}

void ArtworkLoader::clear_pending_updates() {
    has_updates_.store(false);
}

void ArtworkLoader::clear_requests() {
    // Invalidate all pending requests via generation token
    viewport_generation_.fetch_add(1);

    // Clear pending set so paths can be re-queued
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.clear();
    }

    // DO NOT touch cache_ - preserve loaded artwork!

    ouroboros::util::Logger::debug("ArtworkLoader: Cleared request queue, generation=" +
                                   std::to_string(viewport_generation_.load()));
}

void ArtworkLoader::clear_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    size_t cache_size = cache_.size();
    cache_.clear();
    lru_list_.clear();

    ouroboros::util::Logger::debug("ArtworkLoader: Cleared artwork cache (" +
                                   std::to_string(cache_size) + " entries)");
}

void ArtworkLoader::clear_all() {
    clear_requests();
    clear_cache();

    ouroboros::util::Logger::info("ArtworkLoader: Full cache/queue reset");
}

void ArtworkLoader::update_viewport(
    int scroll_offset,
    int selected_index,
    const std::vector<std::string>& visible_paths
) {
    std::lock_guard<std::mutex> lock(viewport_mutex_);

    // Dirty flag: Skip if viewport hasn't changed
    if (!viewport_state_.has_changed(scroll_offset, selected_index)) {
        ouroboros::util::Logger::debug("ArtworkLoader: Viewport unchanged, skipping update");
        return;
    }

    ouroboros::util::Logger::debug("ArtworkLoader: Viewport changed - scroll=" +
                                  std::to_string(scroll_offset) +
                                  ", selected=" + std::to_string(selected_index) +
                                  ", visible_count=" + std::to_string(visible_paths.size()));

    // Update state
    viewport_state_.update(scroll_offset, selected_index);
    viewport_state_.visible_paths.clear();
    viewport_state_.visible_paths.insert(visible_paths.begin(), visible_paths.end());

    // Mark cache entries as viewport-protected for LRU eviction
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        for (auto& [path, entry] : cache_) {
            entry.in_viewport = (viewport_state_.visible_paths.count(path) > 0);
        }
    }
}

bool ArtworkLoader::is_in_viewport(const std::string& path) const {
    std::lock_guard<std::mutex> lock(viewport_mutex_);
    return viewport_state_.visible_paths.count(path) > 0;
}

void ArtworkLoader::mark_accessed(const std::string& path) {
    // Move to front of LRU list (most recently used) - O(1) using stored iterator
    auto it = cache_.find(path);
    if (it != cache_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
        it->second.last_access = std::chrono::steady_clock::now();
    }
}

void ArtworkLoader::evict_if_needed() {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    if (cache_.size() <= MAX_LOADER_CACHE_SIZE) {
        return;  // Below limit, no eviction needed
    }

    size_t to_evict = cache_.size() - MAX_LOADER_CACHE_SIZE;
    size_t evicted = 0;

    ouroboros::util::Logger::debug("ArtworkLoader: Cache full (" +
                                  std::to_string(cache_.size()) +
                                  " entries), evicting " +
                                  std::to_string(to_evict) + " items");

    // Iterate LRU list from oldest (back) to newest (front)
    auto it = lru_list_.rbegin();
    while (to_evict > 0 && it != lru_list_.rend()) {
        auto cache_it = cache_.find(*it);

        if (cache_it == cache_.end()) {
            ++it;
            continue;
        }

        // NEVER evict viewport-protected entries
        if (cache_it->second.in_viewport) {
            ouroboros::util::Logger::debug("ArtworkLoader: Skipping eviction of viewport entry: " + *it);
            ++it;
            continue;
        }

        // Evict this entry
        ouroboros::util::Logger::debug("ArtworkLoader: Evicting LRU entry: " + *it);
        cache_.erase(cache_it);
        it = decltype(it)(lru_list_.erase(std::next(it).base()));
        --to_evict;
        ++evicted;
    }

    ouroboros::util::Logger::info("ArtworkLoader: Evicted " + std::to_string(evicted) +
                                 " entries, cache size now " + std::to_string(cache_.size()));
}

void ArtworkLoader::worker_thread() {
    // Register this worker as active
    active_workers_.fetch_add(1);
    ouroboros::util::Logger::info("ArtworkLoader: Worker thread started (active=" +
                                  std::to_string(active_workers_.load()) + ")");

    while (!should_stop_) {
        std::string track_path;
        int viewport_tier = 0;  // Track tier to decide if we trigger UI update

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Wait with timeout for idle detection
            bool got_work = queue_cv_.wait_for(lock, IDLE_TIMEOUT, [this] {
                return !request_queue_.empty() || should_stop_;
            });

            if (should_stop_) {
                break;
            }

            // Idle timeout - exit if we're not the last worker
            if (!got_work && request_queue_.empty()) {
                if (active_workers_.load() > MIN_WORKERS) {
                    active_workers_.fetch_sub(1);
                    ouroboros::util::Logger::info("ArtworkLoader: Worker exiting (idle timeout, active=" +
                                                  std::to_string(active_workers_.load()) + ")");
                    return;  // Thread exits
                }
                continue;  // Last worker stays alive
            }

            if (!request_queue_.empty()) {
                ArtworkRequest req = request_queue_.top();  // Priority queue: Get highest priority request
                request_queue_.pop();
                track_path = req.track_path;
                viewport_tier = req.viewport_tier;  // Capture tier for update decision

                // Check if request is stale (viewport changed since queuing)
                // BUT: tier=0 requests (NowPlaying current track) are NEVER stale - always process
                if (req.viewport_tier > 0 && req.viewport_generation != viewport_generation_.load()) {
                    ouroboros::util::Logger::debug("ArtworkLoader: Skipping stale request (gen " +
                                                  std::to_string(req.viewport_generation) +
                                                  " != current " +
                                                  std::to_string(viewport_generation_.load()) +
                                                  "): " + req.track_path);

                    // Remove from pending set so it can be re-queued
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        pending_requests_.erase(req.track_path);
                    }
                    continue;  // Skip processing, get next request
                }

                ouroboros::util::Logger::debug("ArtworkLoader: Processing priority request - distance=" + std::to_string(req.distance) +
                                              ", tier=" + std::to_string(req.viewport_tier) +
                                              ", gen=" + std::to_string(req.viewport_generation) +
                                              ", queue_size=" + std::to_string(request_queue_.size()));
            } else {
                continue;
            }
        }

        ouroboros::util::Logger::info("ArtworkLoader: Processing request for: " + track_path);

        // Get album directory for hash lookup
        std::string album_dir = std::filesystem::path(track_path).parent_path().string();

        // Phase 1: Check directory → hash mapping in global cache (O(1) - avoids disk I/O entirely)
        std::string artwork_hash;
        bool cache_hit = false;

        auto& global_cache = backend::ArtworkCache::instance();
        const std::string* dir_hash = global_cache.get_hash_for_dir(album_dir);
        if (dir_hash) {
            artwork_hash = *dir_hash;
            ouroboros::util::Logger::debug("ArtworkLoader: DIR HIT - " + album_dir.substr(album_dir.rfind('/') + 1) + " → hash " + artwork_hash.substr(0, 8) + "...");
        } else {
            // Fallback: Check if we have hash stored for this specific track path in local cache
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = cache_.find(track_path);
            if (it != cache_.end() && !it->second.data.hash.empty()) {
                artwork_hash = it->second.data.hash;
            }
        }

        // Phase 2: Check global ArtworkCache if we have a hash (no disk I/O needed!)
        if (!artwork_hash.empty()) {
            const auto* cached_entry = global_cache.get(artwork_hash);
            if (cached_entry) {
                ouroboros::util::Logger::info("ArtworkLoader: CACHE HIT for hash: " + artwork_hash.substr(0, 16) + "... (" + std::to_string(cached_entry->data.size()) + " bytes)");

                std::lock_guard<std::mutex> lock(cache_mutex_);
                auto it = cache_.find(track_path);
                if (it != cache_.end()) {
                    // DON'T COPY DATA - just store hash reference
                    it->second.data.jpeg_data.clear();  // Free local copy
                    it->second.data.mime_type = cached_entry->mime_type;
                    it->second.data.hash = artwork_hash;
                    it->second.data.path = track_path;
                    it->second.data.loaded = true;
                    mark_accessed(track_path);  // Update LRU
                    // Only trigger UI update for visible items (tier 0), not prefetch (tier 1)
                    if (viewport_tier == 0) {
                        has_updates_.store(true);
                    }
                }
                cache_hit = true;
            }
        }

        // If cache miss, load from disk (blocking I/O)
        if (!cache_hit) {
            auto result = backend::MetadataParser::extract_artwork_data(track_path);

            // Store result ONLY in global ArtworkCache (no local duplication)
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                auto it = cache_.find(track_path);
                // Re-add to cache if it was evicted while we were loading
                if (it == cache_.end()) {
                    lru_list_.push_front(track_path);
                    CacheEntry entry;
                    entry.last_access = std::chrono::steady_clock::now();
                    entry.lru_iter = lru_list_.begin();  // Store iterator for O(1) LRU
                    cache_[track_path] = entry;
                    it = cache_.find(track_path);
                }
                if (it != cache_.end()) {
                    if (result.data.empty()) {
                        ouroboros::util::Logger::warn("ArtworkLoader: No artwork data returned for: " + track_path);
                        // Mark as loaded but empty so we don't retry
                        it->second.data.jpeg_data.clear();
                        it->second.data.path = track_path;
                        it->second.data.hash = "";
                        it->second.data.loaded = true;
                    } else {
                        ouroboros::util::Logger::info("ArtworkLoader: CACHE MISS - Loaded from disk: " + std::to_string(result.data.size()) + " bytes, hash: " + result.hash.substr(0, 16) + "...");

                        // Store in global ArtworkCache (handles dir→hash mapping internally)
                        auto& global_cache = backend::ArtworkCache::instance();
                        global_cache.store(result.hash, result.data, result.mime_type, album_dir);

                        // Local cache only stores hash reference, NOT data
                        it->second.data.jpeg_data.clear();  // Don't duplicate data
                        it->second.data.mime_type = result.mime_type;
                        it->second.data.hash = result.hash;
                        it->second.data.path = track_path;
                        it->second.data.loaded = true;
                        mark_accessed(track_path);  // Update LRU
                    }
                    // Only trigger UI update for visible items (tier 0), not prefetch (tier 1)
                    if (viewport_tier == 0) {
                        has_updates_.store(true);
                    }
                }
            }
        }

        // Remove from pending set - request has been processed
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(track_path);
            ouroboros::util::Logger::debug("ArtworkLoader: Removed from pending set (size=" + std::to_string(pending_requests_.size()) + ")");
        }

        // Trigger LRU eviction if cache is full
        evict_if_needed();
    }

    // Decrement active count when exiting (due to should_stop_)
    active_workers_.fetch_sub(1);
    ouroboros::util::Logger::info("ArtworkLoader: Worker thread exiting (shutdown, active=" +
                                  std::to_string(active_workers_.load()) + ")");
}

void ArtworkLoader::update_queue_context(
    const std::optional<std::string>& current_path,
    const std::vector<std::string>& next_paths
) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Build set of paths to keep
    std::unordered_set<std::string> keep_paths;
    if (current_path) {
        keep_paths.insert(*current_path);
    }
    keep_paths.insert(next_paths.begin(), next_paths.end());

    // Evict items not in keep_paths, BUT respect viewport protection
    // (AlbumBrowser's visible albums should never be evicted by queue changes)
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (keep_paths.find(it->first) == keep_paths.end() && !it->second.in_viewport) {
            it = cache_.erase(it);  // Evict only if not in viewport
        } else {
            ++it;
        }
    }
}

const ArtworkData* ArtworkLoader::get_artwork_ref(const std::string& path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find(path);
    if (it != cache_.end() && it->second.data.loaded && !it->second.data.hash.empty()) {
        // Need to populate jpeg_data from global cache for zero-copy access
        auto& global_cache = backend::ArtworkCache::instance();
        const auto* cached_entry = global_cache.get(it->second.data.hash);
        if (cached_entry) {
            // Temporarily store reference (this is unavoidable for API compat)
            it->second.data.jpeg_data = cached_entry->data;
            mark_accessed(path);  // Update LRU on access
            return &it->second.data;
        }
    }

    if (it == cache_.end()) {
        // ouroboros::util::Logger::debug("ArtworkLoader: Cache MISS for " + path);
    } else {
        // ouroboros::util::Logger::debug("ArtworkLoader: Item found but NOT LOADED for " + path);
    }

    return nullptr;  // Not ready yet
}

bool ArtworkLoader::is_artwork_loaded(const std::string& track_path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find(track_path);
    if (it == cache_.end()) return false;

    return it->second.data.loaded;  // True only if data is in memory
}

} // namespace ouroboros::ui

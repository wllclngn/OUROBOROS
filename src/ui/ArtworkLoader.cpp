#include "ui/ArtworkLoader.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/ArtworkCache.hpp"
#include <algorithm>
#include <fstream>
#include <unordered_set>
#include "util/Logger.hpp"

namespace ouroboros::ui {

ArtworkLoader& ArtworkLoader::instance() {
    static ArtworkLoader instance;
    return instance;
}

ArtworkLoader::ArtworkLoader() {
    // Start background worker thread
    worker_ = std::thread(&ArtworkLoader::worker_thread, this);
}

ArtworkLoader::~ArtworkLoader() {
    should_stop_ = true;
    queue_cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ArtworkLoader::request_artwork(const std::string& track_path) {
    {
        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
        // If already cached or loading, skip
        if (cache_.find(track_path) != cache_.end()) {
            return;
        }
        // Mark as pending
        cache_[track_path] = ArtworkData{};
    }

    // Deduplication: Check if already in flight
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_requests_.find(track_path) != pending_requests_.end()) {
            return;  // Already queued, skip duplicate
        }
        pending_requests_.insert(track_path);
    }

    ouroboros::util::Logger::debug("ArtworkLoader: Queuing request for " + track_path);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        request_queue_.push(track_path);
    }
    queue_cv_.notify_one();
}

bool ArtworkLoader::get_artwork(const std::string& track_path, std::vector<uint8_t>& out_data) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(track_path);

    if (it != cache_.end() && it->second.loaded && !it->second.hash.empty()) {
        // Fetch from global cache using hash
        auto& global_cache = backend::ArtworkCache::instance();
        const auto* cached_entry = global_cache.get(it->second.hash);
        if (cached_entry) {
            out_data = cached_entry->data;
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
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::stack<std::string> empty;
        std::swap(request_queue_, empty);
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_.clear();
    }
}

void ArtworkLoader::worker_thread() {
    ouroboros::util::Logger::info("ArtworkLoader: Worker thread started");

    while (!should_stop_) {
        std::string track_path;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !request_queue_.empty() || should_stop_;
            });

            if (should_stop_) {
                break;
            }

            if (!request_queue_.empty()) {
                track_path = request_queue_.top();  // LIFO: Get most recent request
                request_queue_.pop();
                ouroboros::util::Logger::debug("ArtworkLoader: LIFO POP - Processing most recent request (queue_size=" + std::to_string(request_queue_.size()) + ")");
            } else {
                continue;
            }
        }

        ouroboros::util::Logger::info("ArtworkLoader: Processing request for: " + track_path);

        // Phase 2: Try ArtworkCache first (O(1) hash-based lookup)
        std::string artwork_hash;
        bool cache_hit = false;

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = cache_.find(track_path);
            if (it != cache_.end() && !it->second.hash.empty()) {
                // We know the hash from previous load
                artwork_hash = it->second.hash;
            }
        }

        // Check global ArtworkCache if we have a hash
        if (!artwork_hash.empty()) {
            auto& global_cache = backend::ArtworkCache::instance();
            const auto* cached_entry = global_cache.get(artwork_hash);
            if (cached_entry) {
                ouroboros::util::Logger::info("ArtworkLoader: CACHE HIT for hash: " + artwork_hash.substr(0, 16) + "... (" + std::to_string(cached_entry->data.size()) + " bytes)");

                std::lock_guard<std::mutex> lock(cache_mutex_);
                auto it = cache_.find(track_path);
                if (it != cache_.end()) {
                    // DON'T COPY DATA - just store hash reference
                    it->second.jpeg_data.clear();  // Free local copy
                    it->second.mime_type = cached_entry->mime_type;
                    it->second.hash = artwork_hash;
                    it->second.path = track_path;
                    it->second.loaded = true;
                    has_updates_.store(true);
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
                if (it != cache_.end()) {
                    if (result.data.empty()) {
                        ouroboros::util::Logger::warn("ArtworkLoader: No artwork data returned for: " + track_path);
                        // Mark as loaded but empty so we don't retry
                        it->second.jpeg_data.clear();
                        it->second.path = track_path;
                        it->second.hash = "";
                        it->second.loaded = true;
                    } else {
                        ouroboros::util::Logger::info("ArtworkLoader: CACHE MISS - Loaded from disk: " + std::to_string(result.data.size()) + " bytes, hash: " + result.hash.substr(0, 16) + "...");

                        // Store in global ArtworkCache ONLY (single source of truth)
                        auto& global_cache = backend::ArtworkCache::instance();
                        global_cache.store(result.hash, result.data, result.mime_type);

                        // Local cache only stores hash reference, NOT data
                        it->second.jpeg_data.clear();  // Don't duplicate data
                        it->second.mime_type = result.mime_type;
                        it->second.hash = result.hash;
                        it->second.path = track_path;
                        it->second.loaded = true;
                    }
                    // Signal that artwork is ready for UI update
                    has_updates_.store(true);
                }
            }
        }

        // Remove from pending set - request has been processed
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(track_path);
            ouroboros::util::Logger::debug("ArtworkLoader: Removed from pending set (size=" + std::to_string(pending_requests_.size()) + ")");
        }
    }

    ouroboros::util::Logger::info("ArtworkLoader: Worker thread exiting");
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

    // Evict everything not in keep_paths
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (keep_paths.find(it->first) == keep_paths.end()) {
            it = cache_.erase(it);  // Evict
        } else {
            ++it;
        }
    }
}

const ArtworkData* ArtworkLoader::get_artwork_ref(const std::string& path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    auto it = cache_.find(path);
    if (it != cache_.end() && it->second.loaded && !it->second.hash.empty()) {
        // Need to populate jpeg_data from global cache for zero-copy access
        auto& global_cache = backend::ArtworkCache::instance();
        const auto* cached_entry = global_cache.get(it->second.hash);
        if (cached_entry) {
            // Temporarily store reference (this is unavoidable for API compat)
            it->second.jpeg_data = cached_entry->data;
            return &it->second;
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

    return it->second.loaded;  // True only if data is in memory
}

} // namespace ouroboros::ui

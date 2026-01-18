#include "ui/ArtworkWindow.hpp"
#include "backend/ArtworkCache.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/Config.hpp"
#include "util/Logger.hpp"

#include <filesystem>
#include <sys/ioctl.h>
#include <unistd.h>

// stb_image for decoding
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

// stb_image_resize2 (implementation in ImageRenderer.cpp)
#include "stb/stb_image_resize2.h"

// stb_image_write for PNG encoding
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#pragma GCC diagnostic pop

namespace ouroboros::ui {

ArtworkWindow& ArtworkWindow::instance() {
    static ArtworkWindow instance;
    return instance;
}

ArtworkWindow::ArtworkWindow() {
    // Get memory limit from config
    const auto& config = backend::Config::instance();
    memory_limit_bytes_ = static_cast<size_t>(config.get_artwork_memory_limit_mb()) * 1024 * 1024;

    detect_cell_size();

    util::Logger::info("ArtworkWindow: Starting with " + std::to_string(NUM_WORKERS) +
                      " workers, memory limit " + std::to_string(memory_limit_bytes_ / (1024 * 1024)) + " MB");

    // Start worker threads
    for (size_t i = 0; i < NUM_WORKERS; ++i) {
        workers_.emplace_back(&ArtworkWindow::worker_thread, this);
    }
}

ArtworkWindow::~ArtworkWindow() {
    should_stop_ = true;
    queue_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    util::Logger::info("ArtworkWindow: Shutdown complete");
}

void ArtworkWindow::detect_cell_size() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_xpixel > 0 && w.ws_ypixel > 0) {
        cell_width_ = w.ws_xpixel / w.ws_col;
        cell_height_ = w.ws_ypixel / w.ws_row;
        util::Logger::info("ArtworkWindow: Detected cell size " +
                          std::to_string(cell_width_) + "x" + std::to_string(cell_height_));
    }
}

void ArtworkWindow::request(const std::string& path, int priority, int width_cols, int height_rows,
                            bool notify, bool force_extract) {
    util::Logger::debug("ArtworkWindow::request called: priority=" + std::to_string(priority) +
                       " force_extract=" + std::string(force_extract ? "true" : "false") +
                       " path=" + path.substr(path.rfind('/') + 1));
    if (path.empty()) return;

    // Key by album directory, not individual track path - all tracks share same artwork
    std::string album_dir = std::filesystem::path(path).parent_path().string();
    CacheKey key{album_dir, width_cols, height_rows};

    // Check if already cached (including failed attempts)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);

        // For force_extract (NowPlaying), check track_key FIRST - unique artwork stored there
        if (force_extract) {
            CacheKey track_key{path, width_cols, height_rows};
            auto track_it = cache_.find(track_key);
            if (track_it != cache_.end() && track_it->second) {
                NowPlayingSlotState state = track_it->second->state.load(std::memory_order_acquire);
                if (state == NowPlayingSlotState::Ready) {
                    // Per-track entry ready (UNIQUE artwork) - use it
                    lru_list_.splice(lru_list_.begin(), lru_list_, track_it->second->lru_iter);
                    util::Logger::debug("ArtworkWindow::request: [CACHED-UNIQUE] track_key Ready, returning");
                    return;
                }
                util::Logger::debug("ArtworkWindow::request: track_key exists but state=" +
                                   std::to_string(static_cast<int>(state)) + ", will queue");
            }
            // force_extract: skip dir_key check, queue for extraction (verified_tracks_ checked below)
        }

        // Check directory entry (but NOT for force_extract - need SHA256 comparison)
        if (!force_extract) {
            auto it = cache_.find(key);
            if (it != cache_.end() && it->second) {
                NowPlayingSlotState state = it->second->state.load(std::memory_order_acquire);

                if (state == NowPlayingSlotState::Ready) {
                    // Dir entry ready - use it for normal album browser requests
                    lru_list_.splice(lru_list_.begin(), lru_list_, it->second->lru_iter);
                    return;
                }
                if (state == NowPlayingSlotState::Evicted) {
                    // Was evicted - needs re-decode, fall through to queue
                    util::Logger::debug("ArtworkWindow: Re-queuing evicted entry for " +
                                       path.substr(path.rfind('/') + 1));
                }
                if (state == NowPlayingSlotState::Failed) {
                    // No artwork exists - don't retry
                    return;
                }
            }
        }
        // force_extract=true falls through to queue for SHA256 comparison
    }

    // Check if already pending or verified
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::string pending_key = path + ":" + std::to_string(width_cols) + "x" + std::to_string(height_rows);

        // For force_extract: check if already verified to match album artwork
        if (force_extract && verified_tracks_.count(pending_key)) {
            return;  // Already verified to use album artwork
        }

        if (pending_paths_.count(pending_key)) {
            return;  // Already queued
        }
        pending_paths_.insert(pending_key);

        WindowRequest req;
        req.path = path;
        req.priority = priority;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        req.target_width = width_cols * cell_width_;
        req.target_height = height_rows * cell_height_;
        req.force_extract = force_extract;

        request_queue_.push(req);

        // Log priority for debugging radial loading
        std::string filename = path.substr(path.rfind('/') + 1);
        if (filename.length() > 30) filename = filename.substr(0, 30) + "...";
        util::Logger::debug("ArtworkWindow: Queued priority=" + std::to_string(priority) +
                           " path=" + filename);
    }

    if (notify) {
        queue_cv_.notify_one();
    }
}

void ArtworkWindow::flush_requests() {
    queue_cv_.notify_all();
}

const DecodedArtwork* ArtworkWindow::get_decoded(const std::string& path, int width_cols, int height_rows) {
    std::string album_dir = std::filesystem::path(path).parent_path().string();

    std::lock_guard<std::mutex> lock(cache_mutex_);

    // First check for per-track artwork (NowPlaying with unique artwork)
    CacheKey track_key{path, width_cols, height_rows};
    auto it = cache_.find(track_key);
    util::Logger::debug("ArtworkWindow::get_decoded: track_key lookup " +
                       std::string(it != cache_.end() && it->second ? "FOUND" : "NOT FOUND") +
                       " for " + path.substr(path.rfind('/') + 1));
    if (it != cache_.end() && it->second &&
        it->second->state.load(std::memory_order_acquire) == NowPlayingSlotState::Ready) {
        // Update LRU
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second->lru_iter);

        // Return pointer to static thread-local to avoid dangling pointer issues
        thread_local DecodedArtwork result;
        result.data = it->second->decoded_pixels.data();
        result.data_size = it->second->decoded_pixels.size();
        result.width = it->second->decoded_width;
        result.height = it->second->decoded_height;
        result.format = it->second->format;
        result.hash = it->second->hash;
        return &result;
    }

    // Fall back to directory-based lookup (normal albums)
    CacheKey dir_key{album_dir, width_cols, height_rows};
    it = cache_.find(dir_key);
    bool dir_ready = it != cache_.end() && it->second &&
                     it->second->state.load(std::memory_order_acquire) == NowPlayingSlotState::Ready;
    util::Logger::debug("ArtworkWindow::get_decoded: dir_key lookup " +
                       std::string(it != cache_.end() && it->second ? (dir_ready ? "FOUND+READY" : "FOUND+NOT_READY") : "NOT_FOUND") +
                       " dims=" + std::to_string(width_cols) + "x" + std::to_string(height_rows) +
                       " album=" + album_dir.substr(album_dir.rfind('/') + 1));
    if (dir_ready) {
        // Update LRU
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second->lru_iter);

        thread_local DecodedArtwork result;
        result.data = it->second->decoded_pixels.data();
        result.data_size = it->second->decoded_pixels.size();
        result.width = it->second->decoded_width;
        result.height = it->second->decoded_height;
        result.format = it->second->format;
        result.hash = it->second->hash;
        return &result;
    }

    return nullptr;
}

void ArtworkWindow::reset() {
    util::Logger::debug("ArtworkWindow: Reset called - clearing request queue");

    // Clear queue only - cache uses LRU eviction and shouldn't be cleared
    // (clearing cache causes NowPlaying artwork to flicker on Big Jump)
    // Don't set has_updates_ - queue clearing doesn't require re-render
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!request_queue_.empty()) {
            request_queue_.pop();
        }
        pending_paths_.clear();
    }
}

bool ArtworkWindow::has_updates() {
    return has_updates_.load();
}

void ArtworkWindow::clear_updates() {
    has_updates_.store(false);
}

size_t ArtworkWindow::get_entry_count() const {
    // Note: not thread-safe, for debugging only
    return cache_.size();
}

void ArtworkWindow::evict_until_under_limit() {
    // NOTE: Caller must hold cache_mutex_
    // Two-tier eviction: deallocate pixels but keep entry metadata for re-decode
    size_t evicted = 0;
    size_t bytes_freed = 0;

    while (total_bytes_.load() > memory_limit_bytes_ && !lru_list_.empty()) {
        auto oldest_key = lru_list_.back();
        auto it = cache_.find(oldest_key);

        bool did_evict = false;

        if (it != cache_.end() && it->second) {
            auto& entry = it->second;
            NowPlayingSlotState current = entry->state.load(std::memory_order_acquire);

            // Only evict Ready entries (not Empty, Loading, Evicted, or Failed)
            if (current == NowPlayingSlotState::Ready) {
                size_t entry_bytes = entry->decoded_pixels.size();
                bytes_freed += entry_bytes;
                total_bytes_.fetch_sub(entry_bytes);

                // Deallocate pixels but keep entry metadata
                entry->decoded_pixels.clear();
                entry->decoded_pixels.shrink_to_fit();  // Actually deallocate memory
                entry->state.store(NowPlayingSlotState::Evicted, std::memory_order_release);

                ++evicted;
                did_evict = true;

                util::Logger::debug("ArtworkWindow: Evicted " + oldest_key.album_dir.substr(
                    oldest_key.album_dir.rfind('/') + 1) + " (" +
                    std::to_string(entry_bytes / 1024) + " KB)");
            }
        }

        // Always pop from LRU - either we evicted it, or it's stale/not Ready
        lru_list_.pop_back();

        if (!did_evict && it != cache_.end() && it->second) {
            // Entry wasn't Ready - no memory freed, continue trying next entry
            util::Logger::debug("ArtworkWindow::evict: skipped non-Ready entry (state=" +
                               std::to_string(static_cast<int>(
                                   it->second->state.load(std::memory_order_acquire))) + ")");
        }
    }

    if (evicted > 0) {
        util::Logger::info("ArtworkWindow: Evicted " + std::to_string(evicted) +
                          " entries, freed " + std::to_string(bytes_freed / (1024 * 1024)) +
                          " MB, now " + std::to_string(total_bytes_.load() / (1024 * 1024)) + " MB");
    }
}

void ArtworkWindow::worker_thread() {
    util::Logger::debug("ArtworkWindow: Worker thread started");

    while (!should_stop_) {
        WindowRequest req;
        std::string pending_key;

        // Get next request
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            bool got_work = queue_cv_.wait_for(lock, WORKER_TIMEOUT, [this] {
                return !request_queue_.empty() || should_stop_;
            });

            if (should_stop_) break;
            if (!got_work || request_queue_.empty()) continue;

            req = request_queue_.top();
            request_queue_.pop();
            pending_key = req.path + ":" + std::to_string(req.target_width / cell_width_) +
                         "x" + std::to_string(req.target_height / cell_height_);

            // Log priority when pulled from queue to verify ordering
            std::string filename = req.path.substr(req.path.rfind('/') + 1);
            if (filename.length() > 25) filename = filename.substr(0, 25) + "...";
            util::Logger::debug("ArtworkWindow: PROCESSING priority=" + std::to_string(req.priority) +
                               " " + filename);
        }

        // Process request - get album directory for cache key
        std::string album_dir = std::filesystem::path(req.path).parent_path().string();

        auto& global_cache = backend::ArtworkCache::instance();

        // Double-check: if already in decoded cache, skip (race condition guard)
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            int w = req.target_width / cell_width_;
            int h = req.target_height / cell_height_;

            if (req.force_extract) {
                // For force_extract (NowPlaying): only skip if per-track entry exists
                CacheKey track_key{req.path, w, h};
                auto it = cache_.find(track_key);
                if (it != cache_.end() && it->second &&
                    it->second->state.load(std::memory_order_acquire) == NowPlayingSlotState::Ready) {
                    util::Logger::debug("ArtworkWindow::worker: [SKIP] track_key already Ready");
                    std::lock_guard<std::mutex> qlock(queue_mutex_);
                    pending_paths_.erase(pending_key);
                    continue;  // Per-track entry exists, skip
                }
                util::Logger::debug("ArtworkWindow::worker: [EXTRACT] force_extract, will extract and SHA256 compare");
                // No per-track entry - fall through to ALWAYS extract and SHA256 compare
            } else {
                // For AlbumBrowser: skip if directory entry exists
                CacheKey dir_key{album_dir, w, h};
                auto it = cache_.find(dir_key);
                if (it != cache_.end() && it->second &&
                    it->second->state.load(std::memory_order_acquire) == NowPlayingSlotState::Ready) {
                    std::lock_guard<std::mutex> qlock(queue_mutex_);
                    pending_paths_.erase(pending_key);
                    continue;
                }
            }
        }

        // Get directory's cached hash
        std::string dir_hash;
        const std::string* dir_hash_ptr = global_cache.get_hash_for_dir(album_dir);
        if (dir_hash_ptr) {
            dir_hash = *dir_hash_ptr;
        }

        std::string artwork_hash;
        std::vector<uint8_t> jpeg_data;
        bool has_unique_artwork = false;  // True if track has different artwork than directory

        // Handle force_extract (NowPlaying): ALWAYS extract and SHA256 compare
        // No caching shortcuts - ~20ms per track played is negligible
        if (req.force_extract) {
            auto result = backend::MetadataParser::extract_artwork_data(req.path);
            if (!result.data.empty()) {
                artwork_hash = result.hash;

                // Compare with directory hash
                if (artwork_hash == dir_hash && !dir_hash.empty()) {
                    // Same hash - track uses album artwork, will store under DIR key
                    // Mark verified to prevent repeated extraction on future requests
                    {
                        std::lock_guard<std::mutex> qlock(queue_mutex_);
                        int w = req.target_width / cell_width_;
                        int h = req.target_height / cell_height_;
                        std::string verified_key = req.path + ":" + std::to_string(w) + "x" + std::to_string(h);
                        verified_tracks_.insert(verified_key);
                    }
                    util::Logger::debug("ArtworkWindow::worker: [MATCH] hash=" + artwork_hash.substr(0, 8) +
                                       " matches dir, will use DIR key for " +
                                       req.path.substr(req.path.rfind('/') + 1));
                } else {
                    // Different hash (or no dir hash) - track has unique artwork
                    has_unique_artwork = true;
                    // Store per-track artwork in global cache
                    global_cache.store(artwork_hash, result.data, result.mime_type, album_dir);
                    // Store unique hash for per-track cache key lookup
                    global_cache.mark_verified(req.path, artwork_hash);

                    // Clear ALL Failed entries for this album (any dimensions) so AlbumBrowser will retry
                    // NowPlaying and AlbumBrowser use different dimensions, so we must clear all
                    {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        for (auto& [key, entry] : cache_) {
                            if (entry && key.album_dir == album_dir &&
                                entry->state.load(std::memory_order_acquire) == NowPlayingSlotState::Failed) {
                                // Reset to Empty so it can be re-queued and decoded
                                entry->state.store(NowPlayingSlotState::Empty, std::memory_order_release);
                                util::Logger::debug("ArtworkWindow: Reset Failed entry to Empty for " + album_dir +
                                                   " dims=" + std::to_string(key.width) + "x" + std::to_string(key.height));
                            }
                        }
                    }

                    util::Logger::debug("ArtworkWindow: [UNIQUE] " + artwork_hash.substr(0, 8) +
                                       " differs from directory cache " +
                                       (dir_hash.empty() ? "(none)" : dir_hash.substr(0, 8)));
                }
            }
        } else {
            // Normal AlbumBrowser request - use directory hash
            artwork_hash = dir_hash;
        }

        // Get jpeg data from global cache using the determined hash
        if (!artwork_hash.empty()) {
            const auto* cached_entry = global_cache.get(artwork_hash);
            if (cached_entry) {
                jpeg_data = cached_entry->data;  // Copy jpeg data
                util::Logger::debug("ArtworkWindow: Cache hit for " + artwork_hash.substr(0, 8) + "...");
            }
        }

        // If not in cache, load from disk (fallback for AlbumBrowser when cache not pre-populated)
        if (jpeg_data.empty() && !req.force_extract) {
            auto result = backend::MetadataParser::extract_artwork_data(req.path);
            if (!result.data.empty()) {
                jpeg_data = std::move(result.data);
                artwork_hash = result.hash;
                // Store in global cache for future use
                global_cache.store(result.hash, jpeg_data, result.mime_type, album_dir);
                util::Logger::debug("ArtworkWindow: Loaded from disk, hash " + artwork_hash.substr(0, 8) + "...");
            }
        }

        // Decode jpeg to pixels
        if (!jpeg_data.empty()) {
            auto decode_result = decode_jpeg(jpeg_data, req.target_width, req.target_height);

            if (decode_result.valid) {
                // Use track_path for unique artwork (podcasts), album_dir otherwise
                std::string cache_key_str = has_unique_artwork ? req.path : album_dir;
                CacheKey key{cache_key_str, req.target_width / cell_width_, req.target_height / cell_height_};

                util::Logger::debug(std::string("ArtworkWindow::worker: [STORE] key=") +
                                   (has_unique_artwork ? "TRACK" : "DIR") +
                                   " dims=" + std::to_string(req.target_width / cell_width_) + "x" +
                                   std::to_string(req.target_height / cell_height_) +
                                   " hash=" + artwork_hash.substr(0, 8) +
                                   " path=" + req.path.substr(req.path.rfind('/') + 1));

                std::lock_guard<std::mutex> lock(cache_mutex_);

                // Check if entry already exists (might be Evicted, reuse it)
                auto existing_it = cache_.find(key);
                size_t entry_bytes = decode_result.pixels.size();

                if (existing_it != cache_.end() && existing_it->second) {
                    // Reuse existing entry - just update pixels and state
                    auto& entry = existing_it->second;
                    entry->decoded_pixels = std::move(decode_result.pixels);
                    entry->decoded_width = decode_result.width;
                    entry->decoded_height = decode_result.height;
                    entry->format = decode_result.format;
                    entry->hash = artwork_hash;

                    total_bytes_.fetch_add(entry_bytes);

                    // Add to front of LRU (iterator was invalidated during eviction)
                    lru_list_.push_front(key);
                    entry->lru_iter = lru_list_.begin();

                    // PUBLISH: Release makes all prior writes visible to readers
                    entry->state.store(NowPlayingSlotState::Ready, std::memory_order_release);

                    util::Logger::debug("ArtworkWindow: Refilled evicted entry for " +
                                       req.path.substr(req.path.rfind('/') + 1) +
                                       " (" + std::to_string(entry_bytes / 1024) + " KB)");
                } else {
                    // Create new entry
                    auto entry = std::make_unique<NowPlayingSlot>();
                    entry->hash = artwork_hash;
                    entry->decoded_pixels = std::move(decode_result.pixels);
                    entry->decoded_width = decode_result.width;
                    entry->decoded_height = decode_result.height;
                    entry->format = decode_result.format;

                    // Add to LRU
                    lru_list_.push_front(key);
                    entry->lru_iter = lru_list_.begin();

                    cache_[key] = std::move(entry);

                    // PUBLISH: Release makes all prior writes visible to readers
                    cache_[key]->state.store(NowPlayingSlotState::Ready, std::memory_order_release);
                }

                // Track memory for new entry (already added for refilled)
                if (existing_it == cache_.end()) {
                    total_bytes_.fetch_add(entry_bytes);
                }

                // Evict if over limit
                evict_until_under_limit();

                // Only signal updates for VISIBLE items (priority < 1000)
                // Prefetch items (priority >= 1000) shouldn't trigger re-renders
                if (req.priority < 1000) {
                    has_updates_.store(true);
                }

                util::Logger::debug("ArtworkWindow: Decoded " + req.path.substr(req.path.rfind('/') + 1) +
                                   " (" + std::to_string(entry_bytes / 1024) + " KB), total " +
                                   std::to_string(total_bytes_.load() / (1024 * 1024)) + " MB" +
                                   (has_unique_artwork ? " [UNIQUE]" : ""));
            }
        } else {
            // No artwork found - cache a Failed entry to prevent infinite retry loop
            CacheKey key{album_dir, req.target_width / cell_width_, req.target_height / cell_height_};

            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Check if not already cached (another worker might have added it)
            auto existing_it = cache_.find(key);
            if (existing_it == cache_.end()) {
                auto entry = std::make_unique<NowPlayingSlot>();

                // DON'T add Failed entries to LRU - they have no pixels to evict
                // lru_iter left default/invalid since it's never used for Failed entries
                cache_[key] = std::move(entry);

                // PUBLISH: Set Failed state to prevent retry
                cache_[key]->state.store(NowPlayingSlotState::Failed, std::memory_order_release);

                util::Logger::debug("ArtworkWindow: No artwork for " +
                                   req.path.substr(req.path.rfind('/') + 1) + " (cached as Failed)");
            } else if (existing_it->second &&
                       existing_it->second->state.load(std::memory_order_acquire) == NowPlayingSlotState::Evicted) {
                // Entry was evicted but still no artwork - mark as Failed
                existing_it->second->state.store(NowPlayingSlotState::Failed, std::memory_order_release);

                util::Logger::debug("ArtworkWindow: No artwork for evicted entry " +
                                   req.path.substr(req.path.rfind('/') + 1) + " (marked Failed)");
            }
        }

        // Remove from pending
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            pending_paths_.erase(pending_key);
        }
    }

    util::Logger::debug("ArtworkWindow: Worker thread exiting");
}

ArtworkWindow::DecodeResult ArtworkWindow::decode_jpeg(const std::vector<uint8_t>& jpeg_data,
                                                        int target_w, int target_h) {
    DecodeResult result;
    result.valid = false;

    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(
        jpeg_data.data(), static_cast<int>(jpeg_data.size()),
        &w, &h, &channels, 3
    );

    if (!pixels) {
        util::Logger::warn("ArtworkWindow: Failed to decode jpeg");
        return result;
    }

    // Check aspect ratio (>5% off from square needs letterboxing)
    float aspect_ratio = static_cast<float>(w) / static_cast<float>(h);
    bool needs_letterbox = (aspect_ratio < 0.95f || aspect_ratio > 1.05f);

    if (needs_letterbox) {
        // Calculate scale to fit within target bounds
        float scale = std::min(static_cast<float>(target_w) / w,
                               static_cast<float>(target_h) / h);
        int scaled_w = std::max(1, std::min(static_cast<int>(w * scale), target_w));
        int scaled_h = std::max(1, std::min(static_cast<int>(h * scale), target_h));

        // Resize to scaled dimensions
        std::vector<unsigned char> scaled_rgb(scaled_w * scaled_h * 3);
        stbir_resize(pixels, w, h, 0, scaled_rgb.data(), scaled_w, scaled_h, 0,
                     STBIR_RGB, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_MITCHELL);
        stbi_image_free(pixels);

        // Create RGBA canvas with transparent background
        std::vector<unsigned char> rgba(target_w * target_h * 4, 0);

        // Center the scaled image
        int offset_x = (target_w - scaled_w) / 2;
        int offset_y = (target_h - scaled_h) / 2;
        for (int y = 0; y < scaled_h; ++y) {
            for (int x = 0; x < scaled_w; ++x) {
                int src = (y * scaled_w + x) * 3;
                int dst = ((offset_y + y) * target_w + (offset_x + x)) * 4;
                rgba[dst + 0] = scaled_rgb[src + 0];
                rgba[dst + 1] = scaled_rgb[src + 1];
                rgba[dst + 2] = scaled_rgb[src + 2];
                rgba[dst + 3] = 255;  // Opaque
            }
        }

        // Encode to PNG
        int png_len = 0;
        unsigned char* png_raw = stbi_write_png_to_mem(rgba.data(), target_w * 4, target_w, target_h, 4, &png_len);

        if (png_raw && png_len > 0) {
            result.pixels.assign(png_raw, png_raw + png_len);
            STBIW_FREE(png_raw);
            result.width = target_w;
            result.height = target_h;
            result.format = CachedFormat::PNG;
            result.valid = true;
        } else {
            // Fallback to raw RGBA
            result.pixels = std::move(rgba);
            result.width = target_w;
            result.height = target_h;
            result.format = CachedFormat::RGB;
            result.valid = true;
        }
    } else {
        // Square or nearly-square: direct resize to RGB
        std::vector<unsigned char> output(target_w * target_h * 3);
        stbir_resize(pixels, w, h, 0, output.data(), target_w, target_h, 0,
                     STBIR_RGB, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_MITCHELL);
        stbi_image_free(pixels);

        result.pixels = std::move(output);
        result.width = target_w;
        result.height = target_h;
        result.format = CachedFormat::RGB;
        result.valid = true;
    }

    return result;
}

} // namespace ouroboros::ui

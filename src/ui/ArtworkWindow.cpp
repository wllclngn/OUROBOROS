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

void ArtworkWindow::request(const std::string& path, int priority, int width_cols, int height_rows, bool notify) {
    if (path.empty()) return;

    // Key by album directory, not individual track path - all tracks share same artwork
    std::string album_dir = std::filesystem::path(path).parent_path().string();
    CacheKey key{album_dir, width_cols, height_rows};

    // Check if already cached (including failed attempts)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (it->second.ready) {
                // Already decoded, just update LRU
                lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
                return;
            }
            if (it->second.hash == "FAILED") {
                // Previously failed, don't retry
                return;
            }
        }
    }

    // Check if already pending
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::string pending_key = path + ":" + std::to_string(width_cols) + "x" + std::to_string(height_rows);
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
    // Key by album directory, not individual track path
    std::string album_dir = std::filesystem::path(path).parent_path().string();
    CacheKey key{album_dir, width_cols, height_rows};

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second.ready) {
        // Update LRU
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);

        // Return pointer to static thread-local to avoid dangling pointer issues
        thread_local DecodedArtwork result;
        result.data = it->second.decoded_pixels.data();
        result.data_size = it->second.decoded_pixels.size();
        result.width = it->second.decoded_width;
        result.height = it->second.decoded_height;
        result.format = it->second.format;
        result.hash = it->second.hash;
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
    size_t evicted = 0;
    size_t bytes_freed = 0;

    while (total_bytes_.load() > memory_limit_bytes_ && !lru_list_.empty()) {
        auto oldest_key = lru_list_.back();
        auto it = cache_.find(oldest_key);
        if (it != cache_.end()) {
            size_t entry_bytes = it->second.decoded_pixels.size();
            bytes_freed += entry_bytes;
            total_bytes_.fetch_sub(entry_bytes);
            cache_.erase(it);
            ++evicted;
        }
        lru_list_.pop_back();
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

        // Double-check: if already in decoded cache, skip (race condition guard)
        {
            CacheKey key{album_dir, req.target_width / cell_width_, req.target_height / cell_height_};
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end() && it->second.ready) {
                // Already decoded - skip
                std::lock_guard<std::mutex> qlock(queue_mutex_);
                pending_paths_.erase(pending_key);
                continue;
            }
        }
        auto& global_cache = backend::ArtworkCache::instance();

        // Try to get hash from directory mapping
        std::string artwork_hash;
        const std::string* dir_hash = global_cache.get_hash_for_dir(album_dir);
        if (dir_hash) {
            artwork_hash = *dir_hash;
        }

        // Get jpeg data from global cache
        std::vector<uint8_t> jpeg_data;
        if (!artwork_hash.empty()) {
            const auto* cached_entry = global_cache.get(artwork_hash);
            if (cached_entry) {
                jpeg_data = cached_entry->data;  // Copy jpeg data
                util::Logger::debug("ArtworkWindow: Cache hit for " + artwork_hash.substr(0, 8) + "...");
            }
        }

        // If not in cache, load from disk
        if (jpeg_data.empty()) {
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
                CacheKey key{album_dir, req.target_width / cell_width_, req.target_height / cell_height_};

                std::lock_guard<std::mutex> lock(cache_mutex_);

                // Add to cache
                Entry entry;
                entry.hash = artwork_hash;
                entry.decoded_pixels = std::move(decode_result.pixels);
                entry.decoded_width = decode_result.width;
                entry.decoded_height = decode_result.height;
                entry.format = decode_result.format;
                entry.ready = true;

                // Track memory
                size_t entry_bytes = entry.decoded_pixels.size();
                total_bytes_.fetch_add(entry_bytes);

                // Add to LRU
                lru_list_.push_front(key);
                entry.lru_iter = lru_list_.begin();

                cache_[key] = std::move(entry);

                // Evict if over limit
                evict_until_under_limit();

                // Only signal updates for VISIBLE items (priority < 1000)
                // Prefetch items (priority >= 1000) shouldn't trigger re-renders
                if (req.priority < 1000) {
                    has_updates_.store(true);
                }

                util::Logger::debug("ArtworkWindow: Decoded " + req.path.substr(req.path.rfind('/') + 1) +
                                   " (" + std::to_string(entry_bytes / 1024) + " KB), total " +
                                   std::to_string(total_bytes_.load() / (1024 * 1024)) + " MB");
            }
        } else {
            // No artwork found - cache a "failed" entry to prevent infinite retry loop
            CacheKey key{album_dir, req.target_width / cell_width_, req.target_height / cell_height_};

            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Check if not already cached (another worker might have added it)
            if (cache_.find(key) == cache_.end()) {
                Entry entry;
                entry.ready = false;  // Mark as "tried but failed"
                entry.hash = "FAILED";  // Sentinel value

                lru_list_.push_front(key);
                entry.lru_iter = lru_list_.begin();
                cache_[key] = std::move(entry);

                util::Logger::debug("ArtworkWindow: No artwork for " +
                                   req.path.substr(req.path.rfind('/') + 1) + " (cached as failed)");
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

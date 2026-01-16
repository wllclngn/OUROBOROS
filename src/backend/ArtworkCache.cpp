#include "backend/ArtworkCache.hpp"
#include "util/Logger.hpp"
#include <fstream>
#include <cstring>

// For image validation
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

namespace ouroboros::backend {

ArtworkCache& ArtworkCache::instance() {
    static ArtworkCache instance;
    return instance;
}

void ArtworkCache::store(const std::string& hash, std::vector<uint8_t> data, const std::string& mime_type, const std::string& source_dir) {
    if (hash.empty() || data.empty()) {
        return;
    }

    // Validate image before storing (reject corrupt data)
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()),
                                                  &w, &h, &channels, 4);

    if (!pixels) {
        util::Logger::warn("ArtworkCache: Corrupt image data, skipping hash: " + hash.substr(0, 16) + "...");
        return;
    }

    stbi_image_free(pixels);

    // Store valid image
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already exists
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        // Already cached - just increment ref count
        it->second.ref_count++;
        // Update dir mapping if not set
        if (!source_dir.empty() && dir_to_hash_.find(source_dir) == dir_to_hash_.end()) {
            dir_to_hash_[source_dir] = hash;
        }
        return;
    }

    // Store new entry
    cache_[hash] = RawArtworkEntry{std::move(data), mime_type, source_dir, 1};
    dirty_ = true;  // Mark cache as needing save

    // Store dir→hash mapping
    if (!source_dir.empty()) {
        dir_to_hash_[source_dir] = hash;
    }

    util::Logger::debug("ArtworkCache: Stored artwork " + hash.substr(0, 16) + "... (" +
                       std::to_string(cache_[hash].data.size() / 1024) + " KB, " + mime_type + ")");
}

const RawArtworkEntry* ArtworkCache::get(const std::string& hash) const {
    if (hash.empty()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        return &it->second;
    }

    return nullptr;
}

const std::string* ArtworkCache::get_hash_for_dir(const std::string& dir) const {
    if (dir.empty()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = dir_to_hash_.find(dir);
    if (it != dir_to_hash_.end()) {
        return &it->second;
    }

    return nullptr;
}

void ArtworkCache::ref(const std::string& hash) {
    if (hash.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        it->second.ref_count++;
    }
}

void ArtworkCache::unref(const std::string& hash) {
    if (hash.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(hash);
    if (it != cache_.end()) {
        if (it->second.ref_count > 0) {
            it->second.ref_count--;
        }

        // Evict if no more references
        if (it->second.ref_count == 0) {
            util::Logger::debug("ArtworkCache: Evicting unused artwork " + hash.substr(0, 16) + "...");
            // Clean up dir mapping before erasing
            const std::string& source_dir = it->second.source_dir;
            if (!source_dir.empty()) {
                auto dir_it = dir_to_hash_.find(source_dir);
                if (dir_it != dir_to_hash_.end() && dir_it->second == hash) {
                    dir_to_hash_.erase(dir_it);
                }
            }
            cache_.erase(it);
            dirty_ = true;
        }
    }
}

void ArtworkCache::mark_verified(const std::string& path, const std::string& hash) {
    if (path.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (verified_tracks_.insert(path).second) {
        dirty_ = true;  // Only mark dirty if actually inserted
    }
    // Store track-specific hash if provided (for unique per-track artwork)
    if (!hash.empty()) {
        track_to_hash_[path] = hash;
        dirty_ = true;
    }
}

bool ArtworkCache::is_verified(const std::string& path) const {
    if (path.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    return verified_tracks_.count(path) > 0;
}

const std::string* ArtworkCache::get_hash_for_track(const std::string& path) const {
    if (path.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = track_to_hash_.find(path);
    if (it != track_to_hash_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool ArtworkCache::is_dirty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirty_;
}

bool ArtworkCache::save(const std::filesystem::path& cache_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Skip save if nothing changed
    if (!dirty_) {
        util::Logger::info("ArtworkCache: No changes, skipping save");
        return true;
    }

    try {
        std::filesystem::create_directories(cache_path.parent_path());

        std::ofstream out(cache_path, std::ios::binary);
        if (!out) {
            util::Logger::error("ArtworkCache: Failed to open cache file for writing: " + cache_path.string());
            return false;
        }

        // Write header
        out.write(reinterpret_cast<const char*>(&CACHE_MAGIC), sizeof(CACHE_MAGIC));
        out.write(reinterpret_cast<const char*>(&CACHE_VERSION), sizeof(CACHE_VERSION));

        // Write entry count
        uint64_t count = cache_.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));

        // Write each entry
        for (const auto& [hash, entry] : cache_) {
            // Hash (64 bytes - SHA-256 hex string)
            uint32_t hash_len = static_cast<uint32_t>(hash.length());
            out.write(reinterpret_cast<const char*>(&hash_len), sizeof(hash_len));
            out.write(hash.data(), hash_len);

            // MIME type
            uint32_t mime_len = static_cast<uint32_t>(entry.mime_type.length());
            out.write(reinterpret_cast<const char*>(&mime_len), sizeof(mime_len));
            if (mime_len > 0) {
                out.write(entry.mime_type.data(), mime_len);
            }

            // Source directory
            uint32_t dir_len = static_cast<uint32_t>(entry.source_dir.length());
            out.write(reinterpret_cast<const char*>(&dir_len), sizeof(dir_len));
            if (dir_len > 0) {
                out.write(entry.source_dir.data(), dir_len);
            }

            // Artwork data
            uint64_t data_len = entry.data.size();
            out.write(reinterpret_cast<const char*>(&data_len), sizeof(data_len));
            out.write(reinterpret_cast<const char*>(entry.data.data()), data_len);

            // Ref count
            out.write(reinterpret_cast<const char*>(&entry.ref_count), sizeof(entry.ref_count));
        }

        // Write verified tracks count and paths
        uint64_t verified_count = verified_tracks_.size();
        out.write(reinterpret_cast<const char*>(&verified_count), sizeof(verified_count));
        for (const auto& path : verified_tracks_) {
            uint32_t path_len = static_cast<uint32_t>(path.length());
            out.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
            out.write(path.data(), path_len);
        }

        // Write track-to-hash mappings (for tracks with unique artwork)
        uint64_t track_hash_count = track_to_hash_.size();
        out.write(reinterpret_cast<const char*>(&track_hash_count), sizeof(track_hash_count));
        for (const auto& [path, hash] : track_to_hash_) {
            uint32_t path_len = static_cast<uint32_t>(path.length());
            out.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
            out.write(path.data(), path_len);
            uint32_t hash_len = static_cast<uint32_t>(hash.length());
            out.write(reinterpret_cast<const char*>(&hash_len), sizeof(hash_len));
            out.write(hash.data(), hash_len);
        }

        // Write dir-to-hash mappings (directory -> artwork hash)
        uint64_t dir_hash_count = dir_to_hash_.size();
        out.write(reinterpret_cast<const char*>(&dir_hash_count), sizeof(dir_hash_count));
        for (const auto& [dir, hash] : dir_to_hash_) {
            uint32_t dir_len = static_cast<uint32_t>(dir.length());
            out.write(reinterpret_cast<const char*>(&dir_len), sizeof(dir_len));
            out.write(dir.data(), dir_len);
            uint32_t hash_len = static_cast<uint32_t>(hash.length());
            out.write(reinterpret_cast<const char*>(&hash_len), sizeof(hash_len));
            out.write(hash.data(), hash_len);
        }

        dirty_ = false;  // Mark as clean after successful save
        util::Logger::info("ArtworkCache: Saved " + std::to_string(count) + " entries, " +
                          std::to_string(dir_hash_count) + " dirs, " +
                          std::to_string(track_hash_count) + " unique tracks to " + cache_path.string());
        return true;

    } catch (const std::exception& e) {
        util::Logger::error("ArtworkCache: Failed to save cache: " + std::string(e.what()));
        return false;
    }
}

bool ArtworkCache::load(const std::filesystem::path& cache_path) {
    if (!std::filesystem::exists(cache_path)) {
        util::Logger::info("ArtworkCache: No existing cache file found");
        return false;
    }

    try {
        std::ifstream in(cache_path, std::ios::binary);
        if (!in) {
            util::Logger::error("ArtworkCache: Failed to open cache file for reading: " + cache_path.string());
            return false;
        }

        // Read and validate header
        uint64_t magic;
        uint32_t version;
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != CACHE_MAGIC) {
            util::Logger::error("ArtworkCache: Invalid cache magic number");
            return false;
        }

        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version != CACHE_VERSION) {
            util::Logger::warn("ArtworkCache: Cache version mismatch (file=" + std::to_string(version) +
                              ", expected=" + std::to_string(CACHE_VERSION) + "), will rebuild");
            return false;
        }

        // Read entry count
        uint64_t count;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));

        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        cache_.reserve(count);

        // Read each entry
        for (uint64_t i = 0; i < count; ++i) {
            // Hash
            uint32_t hash_len;
            in.read(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
            std::string hash(hash_len, '\0');
            in.read(hash.data(), hash_len);

            // MIME type
            uint32_t mime_len;
            in.read(reinterpret_cast<char*>(&mime_len), sizeof(mime_len));
            std::string mime_type;
            if (mime_len > 0) {
                mime_type.resize(mime_len);
                in.read(mime_type.data(), mime_len);
            }

            // Source directory
            uint32_t dir_len;
            in.read(reinterpret_cast<char*>(&dir_len), sizeof(dir_len));
            std::string source_dir;
            if (dir_len > 0) {
                source_dir.resize(dir_len);
                in.read(source_dir.data(), dir_len);
            }

            // Artwork data
            uint64_t data_len;
            in.read(reinterpret_cast<char*>(&data_len), sizeof(data_len));
            std::vector<uint8_t> data(data_len);
            in.read(reinterpret_cast<char*>(data.data()), data_len);

            // Ref count (read but ignore - reset to 0 since LRU cache is empty at startup)
            size_t ref_count_ignored;
            in.read(reinterpret_cast<char*>(&ref_count_ignored), sizeof(ref_count_ignored));

            // Store entry with ref_count=0 (no LRU entries reference it yet)
            cache_[hash] = RawArtworkEntry{std::move(data), std::move(mime_type), source_dir, 0};
        }

        // Read verified tracks
        verified_tracks_.clear();
        uint64_t verified_count;
        in.read(reinterpret_cast<char*>(&verified_count), sizeof(verified_count));
        for (uint64_t i = 0; i < verified_count; ++i) {
            uint32_t path_len;
            in.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
            std::string path(path_len, '\0');
            in.read(path.data(), path_len);
            verified_tracks_.insert(std::move(path));
        }

        // Read track-to-hash mappings (for tracks with unique artwork)
        track_to_hash_.clear();
        uint64_t track_hash_count;
        in.read(reinterpret_cast<char*>(&track_hash_count), sizeof(track_hash_count));
        for (uint64_t i = 0; i < track_hash_count; ++i) {
            uint32_t path_len;
            in.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
            std::string path(path_len, '\0');
            in.read(path.data(), path_len);
            uint32_t hash_len;
            in.read(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
            std::string hash(hash_len, '\0');
            in.read(hash.data(), hash_len);
            track_to_hash_[std::move(path)] = std::move(hash);
        }

        // Read dir-to-hash mappings (directory -> artwork hash)
        dir_to_hash_.clear();
        uint64_t dir_hash_count;
        in.read(reinterpret_cast<char*>(&dir_hash_count), sizeof(dir_hash_count));
        for (uint64_t i = 0; i < dir_hash_count; ++i) {
            uint32_t dir_len;
            in.read(reinterpret_cast<char*>(&dir_len), sizeof(dir_len));
            std::string dir(dir_len, '\0');
            in.read(dir.data(), dir_len);
            uint32_t hash_len;
            in.read(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
            std::string hash(hash_len, '\0');
            in.read(hash.data(), hash_len);
            dir_to_hash_[std::move(dir)] = std::move(hash);
        }

        dirty_ = false;  // Freshly loaded, no changes yet
        util::Logger::info("ArtworkCache: Loaded " + std::to_string(cache_.size()) + " entries, " +
                          std::to_string(dir_to_hash_.size()) + " dirs, " +
                          std::to_string(track_to_hash_.size()) + " unique tracks from " + cache_path.string());
        return true;

    } catch (const std::exception& e) {
        util::Logger::error("ArtworkCache: Failed to load cache: " + std::string(e.what()));
        cache_.clear();
        return false;
    }
}

size_t ArtworkCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

size_t ArtworkCache::memory_usage() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t total = 0;
    for (const auto& [hash, entry] : cache_) {
        total += entry.data.size();
        total += hash.size();
        total += entry.mime_type.size();
    }

    return total;
}

void ArtworkCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

}  // namespace ouroboros::backend

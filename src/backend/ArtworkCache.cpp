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

void ArtworkCache::store(const std::string& hash, std::vector<uint8_t> data, const std::string& mime_type) {
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
        return;
    }

    // Store new entry
    cache_[hash] = ArtworkEntry{std::move(data), mime_type, 1};

    util::Logger::debug("ArtworkCache: Stored artwork " + hash.substr(0, 16) + "... (" +
                       std::to_string(cache_[hash].data.size() / 1024) + " KB, " + mime_type + ")");
}

const ArtworkEntry* ArtworkCache::get(const std::string& hash) const {
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
            cache_.erase(it);
        }
    }
}

bool ArtworkCache::save(const std::filesystem::path& cache_path) const {
    try {
        std::filesystem::create_directories(cache_path.parent_path());

        std::ofstream out(cache_path, std::ios::binary);
        if (!out) {
            util::Logger::error("ArtworkCache: Failed to open cache file for writing: " + cache_path.string());
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

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

            // Artwork data
            uint64_t data_len = entry.data.size();
            out.write(reinterpret_cast<const char*>(&data_len), sizeof(data_len));
            out.write(reinterpret_cast<const char*>(entry.data.data()), data_len);

            // Ref count
            out.write(reinterpret_cast<const char*>(&entry.ref_count), sizeof(entry.ref_count));
        }

        util::Logger::info("ArtworkCache: Saved " + std::to_string(count) + " entries to " + cache_path.string());
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
            util::Logger::warn("ArtworkCache: Cache version mismatch, will rebuild");
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

            // Artwork data
            uint64_t data_len;
            in.read(reinterpret_cast<char*>(&data_len), sizeof(data_len));
            std::vector<uint8_t> data(data_len);
            in.read(reinterpret_cast<char*>(data.data()), data_len);

            // Ref count
            size_t ref_count;
            in.read(reinterpret_cast<char*>(&ref_count), sizeof(ref_count));

            // Store entry (without validation - assume cache is valid)
            cache_[hash] = ArtworkEntry{std::move(data), std::move(mime_type), ref_count};
        }

        util::Logger::info("ArtworkCache: Loaded " + std::to_string(cache_.size()) + " entries from " + cache_path.string());
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

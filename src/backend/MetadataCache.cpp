#include "backend/MetadataCache.hpp"

namespace ouroboros::backend {

MetadataCache::MetadataCache(std::chrono::seconds ttl) : ttl_(ttl) {}

void MetadataCache::cache_metadata(const std::string& file_path, const model::Track& metadata) {
    cache_[file_path] = {metadata, std::chrono::steady_clock::now()};
}

std::optional<model::Track> MetadataCache::get_cached_metadata(const std::string& file_path) const {
    auto it = cache_.find(file_path);
    if (it == cache_.end()) return std::nullopt;

    auto now = std::chrono::steady_clock::now();
    if (now - it->second.timestamp > ttl_) return std::nullopt;

    return it->second.metadata;
}

bool MetadataCache::has_cached_metadata(const std::string& file_path) const {
    return cache_.find(file_path) != cache_.end();
}

void MetadataCache::clear() {
    cache_.clear();
}

void MetadataCache::cleanup_expired() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (now - it->second.timestamp > ttl_) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace ouroboros::backend

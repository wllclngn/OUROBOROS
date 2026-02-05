#pragma once

#include "model/Snapshot.hpp"
#include <unordered_map>
#include <chrono>

namespace ouroboros::backend {

class MetadataCache {
public:
    MetadataCache(std::chrono::seconds ttl = std::chrono::seconds(3600));

    void cache_metadata(const std::string& file_path, const model::Track& metadata);
    [[nodiscard]] std::optional<model::Track> get_cached_metadata(const std::string& file_path) const;
    [[nodiscard]] bool has_cached_metadata(const std::string& file_path) const;
    
    void clear();
    void cleanup_expired();

private:
    struct CacheEntry {
        model::Track metadata;
        std::chrono::steady_clock::time_point timestamp;
    };

    std::unordered_map<std::string, CacheEntry> cache_;
    std::chrono::seconds ttl_;
};

}  // namespace ouroboros::backend

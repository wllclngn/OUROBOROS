#pragma once

#include "model/Snapshot.hpp"
#include <string>
#include <optional>
#include <vector>

namespace ouroboros::backend {

// Artwork extraction result with content-addressed hash
struct ArtworkExtractionResult {
    std::vector<uint8_t> data;
    std::string mime_type;
    std::string hash;  // SHA-256 hash of data (empty if no artwork)
};

class MetadataParser {
public:
    // Parse file and return Track with complete metadata
    static model::Track parse_file(const std::string& path);

    // JIT Artwork Loading (Public Helper)
    // Returns artwork data with SHA-256 hash for content-addressed caching
    static ArtworkExtractionResult extract_artwork_data(const std::string& path);
    
private:
    // Helper parsers using native libraries
    static bool parse_mp3(const std::string& path, model::Track& track);
    static bool parse_sndfile(const std::string& path, model::Track& track);
    static bool parse_m4a(const std::string& path, model::Track& track);

    // Determine audio format from extension
    static model::AudioFormat detect_format(const std::string& path);
    
    // Filename parsing fallbacks
    static std::string extract_title_from_filename(const std::string& path);
    static std::string extract_artist_from_filename(const std::string& path);
};

}  // namespace ouroboros::backend

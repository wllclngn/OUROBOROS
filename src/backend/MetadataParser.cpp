#include "backend/MetadataParser.hpp"
#include "util/ArtworkHasher.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <mpg123.h>
#include <sndfile.h>
#include <cstring>
#include <vector>
#include "util/Logger.hpp"

/*
 * METADATA PARSING with libsndfile
 *
 * This file uses libsndfile (https://github.com/libsndfile/libsndfile) for
 * reading FLAC, OGG/Vorbis, and WAV audio files.
 *
 * libsndfile is a C library created by Erik de Castro Lopo and maintained by
 * the libsndfile team. It provides a unified API for reading and writing files
 * containing sampled audio data across multiple formats (WAV, AIFF, FLAC, OGG, etc.).
 *
 * Key features we utilize:
 * - Unified interface: Handle multiple audio formats through one standard API
 * - Automatic format conversion: Seamlessly converts between file formats and application data
 * - Metadata abstraction: Handles endianness, bit depth, and format-specific details automatically
 * - Embedded tag reading: Extract artist, title, album, date, genre from FLAC/OGG Vorbis comments
 * - O(1) bitrate calculation: sf_current_byterate() provides bytes/sec without filesystem I/O
 *
 * For FLAC/OGG/WAV files, we use libsndfile's SF_INFO structure to get:
 * - Sample rate, channels, total frames (for duration calculation)
 * - Bit depth (from format submask for PCM formats)
 * - Embedded metadata tags (via sf_get_string)
 * - Current byterate (via sf_current_byterate for bitrate calculation)
 *
 * This approach is lightweight and format-specific, avoiding heavy abstraction layers
 * like TagLib while using the same library that decodes the audio for playback.
 *
 * Licensed under LGPL-2.1. Credit to the libsndfile team for this excellent library.
 */

namespace ouroboros::backend {

namespace {
    std::string trim(const std::string& str) {
        if (str.empty()) return "";
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }
}

// Helper class to ensure mpg123 is initialized
struct Mpg123Initializer {
    Mpg123Initializer() { mpg123_init(); }
    ~Mpg123Initializer() { mpg123_exit(); }
};
static Mpg123Initializer g_mpg123_init;

model::Track MetadataParser::parse_file(const std::string& path) {
    model::Track track;
    track.path = path;

    if (!std::filesystem::exists(path)) {
        track.is_valid = false;
        track.error_message = "File not found";
        return track;
    }

    // Detect format
    track.format = detect_format(path);

    bool parsed = false;
    if (track.format == model::AudioFormat::MP3) {
        parsed = parse_mp3(path, track);
    } else {
        parsed = parse_sndfile(path, track);
    }

    if (!parsed) {
        track.is_valid = false;
        track.error_message = "Failed to parse audio file";
        return track;
    }

    // Post-process metadata
    if (track.title.empty()) {
        track.title = extract_title_from_filename(path);
    }
    if (track.artist.empty()) {
        track.artist = extract_artist_from_filename(path);
    }
    if (track.album.empty()) {
        track.album = extract_album_from_path(path);
    }

    // NO ARTWORK LOADING HERE! (JIT optimization)

    track.is_valid = true;
    return track;
}

bool MetadataParser::parse_mp3(const std::string& path, model::Track& track) {
    mpg123_handle* mh = mpg123_new(nullptr, nullptr);
    if (!mh) return false;

    if (mpg123_open(mh, path.c_str()) != MPG123_OK) {
        mpg123_delete(mh);
        return false;
    }

    // Scan to get accurate length and parse ID3 tags
    mpg123_scan(mh);

    // Get Audio Info
    long rate;
    int channels, encoding;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) == MPG123_OK) {
        track.sample_rate = static_cast<int>(rate);
        track.channels = channels;
        track.bit_depth = 16; 
        
        // Calculate bitrate
        mpg123_frameinfo mi;
        if (mpg123_info(mh, &mi) == MPG123_OK) {
            track.bitrate = mi.bitrate;
        }
    }

    // Get Duration
    off_t length = mpg123_length(mh);
    if (length > 0 && track.sample_rate > 0) {
        double seconds = static_cast<double>(length) / track.sample_rate;
        track.duration_ms = static_cast<int>(seconds * 1000);
    }

    // Get ID3 Tags
    mpg123_id3v1* v1;
    mpg123_id3v2* v2;
    if (mpg123_id3(mh, &v1, &v2) == MPG123_OK) {
        if (v2) {
            if (v2->title && v2->title->p) track.title = trim(v2->title->p);
            if (v2->artist && v2->artist->p) track.artist = trim(v2->artist->p);
            if (v2->album && v2->album->p) track.album = trim(v2->album->p);
            if (v2->genre && v2->genre->p) track.genre = trim(v2->genre->p);
            if (v2->year && v2->year->p) track.date = trim(v2->year->p);

            // Parse track number from TRCK frame
            for (size_t i = 0; i < v2->texts; ++i) {
                // Check if this is the TRCK frame (track number)
                if (std::strncmp(v2->text[i].id, "TRCK", 4) == 0) {
                    if (v2->text[i].text.p) {
                        std::string track_num_str = trim(v2->text[i].text.p);
                        if (!track_num_str.empty()) {
                            try {
                                // Handle formats like "01/12" or "1 of 12" - extract first number
                                size_t slash_pos = track_num_str.find('/');
                                size_t space_pos = track_num_str.find(' ');
                                size_t end_pos = std::min(slash_pos, space_pos);
                                if (end_pos != std::string::npos) {
                                    track_num_str = track_num_str.substr(0, end_pos);
                                }
                                track.track_number = std::stoi(track_num_str);
                            } catch (...) {
                                // If parsing fails, track_number remains 0 (default)
                            }
                        }
                    }
                    break;  // Found TRCK frame, no need to continue
                }
            }
        }
        else if (v1) {
            track.title = trim(std::string(v1->title, 30));
            track.artist = trim(std::string(v1->artist, 30));
            track.album = trim(std::string(v1->album, 30));
            track.genre = std::to_string(v1->genre);
            track.date = trim(std::string(v1->year, 4));

            // ID3v1.1: track number is in comment[29] if comment[28] is null
            if (v1->comment[28] == 0 && v1->comment[29] != 0) {
                track.track_number = static_cast<int>(v1->comment[29]);
            }
        }
    }
    mpg123_close(mh);
    mpg123_delete(mh);
    return true;
}

bool MetadataParser::parse_sndfile(const std::string& path, model::Track& track) {
    ouroboros::util::Logger::debug("MetadataParser: Parsing file with libsndfile");

    SF_INFO sfinfo;
    std::memset(&sfinfo, 0, sizeof(sfinfo));

    SNDFILE* sndfile = sf_open(path.c_str(), SFM_READ, &sfinfo);
    if (!sndfile) return false;

    track.sample_rate = sfinfo.samplerate;
    track.channels = sfinfo.channels;
    track.duration_ms = static_cast<int>((static_cast<double>(sfinfo.frames) / sfinfo.samplerate) * 1000);
    
    int subformat = sfinfo.format & SF_FORMAT_SUBMASK;
    if (subformat == SF_FORMAT_PCM_16) track.bit_depth = 16;
    else if (subformat == SF_FORMAT_PCM_24) track.bit_depth = 24;
    else if (subformat == SF_FORMAT_PCM_32) track.bit_depth = 32;
    else track.bit_depth = 16;

    auto get_tag = [&](int tag_id) -> std::string {
        const char* val = sf_get_string(sndfile, tag_id);
        return val ? trim(val) : "";
    };

    track.title = get_tag(SF_STR_TITLE);
    track.artist = get_tag(SF_STR_ARTIST);
    track.album = get_tag(SF_STR_ALBUM);
    track.date = get_tag(SF_STR_DATE);
    track.genre = get_tag(SF_STR_GENRE);
    
    std::string track_num_str = get_tag(SF_STR_TRACKNUMBER);
    if (!track_num_str.empty()) {
        try {
            // Handle formats like "01/12" or "1 of 12" - extract first number
            size_t slash_pos = track_num_str.find('/');
            size_t space_pos = track_num_str.find(' ');
            size_t end_pos = std::min(slash_pos, space_pos);
            if (end_pos != std::string::npos) {
                track_num_str = track_num_str.substr(0, end_pos);
            }
            track.track_number = std::stoi(track_num_str);
        } catch (...) {
            // If parsing fails, track_number remains 0 (default)
        }
    }

    // Calculate bitrate using O(1) libsndfile function (must be called before sf_close)
    // sf_current_byterate() returns bytes/sec without requiring filesystem I/O
    int byterate = sf_current_byterate(sndfile);
    if (byterate > 0) {
        track.bitrate = (byterate * 8) / 1000; // Convert bytes/sec to kbps
        ouroboros::util::Logger::debug("MetadataParser: Calculated bitrate " +
                                       std::to_string(track.bitrate) + "kbps for " + path);
    } else {
        track.bitrate = 0;
        ouroboros::util::Logger::debug("MetadataParser: Bitrate unavailable for " + path);
    }

    sf_close(sndfile);
    return true;
}

model::AudioFormat MetadataParser::detect_format(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".mp3") return model::AudioFormat::MP3;
    if (ext == ".flac") return model::AudioFormat::FLAC;
    if (ext == ".ogg" || ext == ".opus") return model::AudioFormat::OGG;
    if (ext == ".wav") return model::AudioFormat::WAV;
    
    return model::AudioFormat::Unknown;
}

ArtworkExtractionResult MetadataParser::extract_artwork_data(const std::string& path) {
    namespace fs = std::filesystem;

    ouroboros::util::Logger::info("=== ARTWORK SEARCH START for: " + path + " ===");

    fs::path audio_file(path);
    fs::path dir = audio_file.parent_path();

    // LOGGING DISABLED: Called on every track change, spams debug logs
    // ouroboros::util::Logger::debug("Searching directory: " + dir.string());

    const std::vector<std::string> art_names = {
        "cover.jpg", "cover.png", "cover.jpeg",
        "folder.jpg", "folder.png",
        "album.jpg", "album.png",
        "front.jpg", "front.png",
        "Cover.jpg", "Cover.png",
        "Folder.jpg", "Folder.png"
    };

    for (const auto& name : art_names) {
        fs::path candidate = dir / name;
        // LOGGING DISABLED: Called 12+ times per track change, spams debug logs
        // ouroboros::util::Logger::debug("Checking: " + candidate.string());

        if (fs::exists(candidate)) {
            ouroboros::util::Logger::info("FOUND ARTWORK: " + candidate.string());
            std::ifstream file(candidate, std::ios::binary);
            if (file) {
                std::vector<uint8_t> data(
                    (std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>()
                );

                std::string ext = candidate.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                std::string mime = "image/jpeg";
                if (ext == ".png") mime = "image/png";

                // Compute SHA-256 hash for content-addressed caching
                std::string hash = util::ArtworkHasher::hash_artwork(data);

                ouroboros::util::Logger::info("Loaded " + std::to_string(data.size()) + " bytes, hash: " + hash.substr(0, 16) + "...");
                return {std::move(data), mime, hash};
            }
        }
    }

    ouroboros::util::Logger::warn("No artwork found with common names, scanning directory...");

    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            // LOGGING DISABLED: Called for every file in directory, spams debug logs
            // ouroboros::util::Logger::debug("Found file: " + entry.path().filename().string() + " (ext: " + ext + ")");

            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") {
                ouroboros::util::Logger::info("Using fallback artwork: " + entry.path().string());
                std::ifstream file(entry.path(), std::ios::binary);
                if (file) {
                    std::vector<uint8_t> data(
                        (std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>()
                    );

                    std::string mime = (ext == ".png") ? "image/png" : "image/jpeg";

                    // Compute SHA-256 hash for content-addressed caching
                    std::string hash = util::ArtworkHasher::hash_artwork(data);

                    ouroboros::util::Logger::info("Loaded " + std::to_string(data.size()) + " bytes, hash: " + hash.substr(0, 16) + "...");
                    return {std::move(data), mime, hash};
                }
            }
        }
    } catch (const fs::filesystem_error&) {
        ouroboros::util::Logger::error("Filesystem error while scanning directory");
    }

    ouroboros::util::Logger::error("NO ARTWORK FILES FOUND in " + dir.string());
    return {{}, "", ""};  // Empty data, mime, and hash
}

std::string MetadataParser::extract_title_from_filename(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    std::string filename = p.stem().string();
    
    size_t dash_pos = filename.find(" - ");
    if (dash_pos != std::string::npos && dash_pos < filename.length() - 3) {
        return filename.substr(dash_pos + 3);
    }
    
    if (filename.length() > 3 && std::isdigit(filename[0]) && std::isdigit(filename[1])) {
        if (filename[2] == '.' || filename[2] == ' ' || filename[2] == '_') {
            filename = filename.substr(3);
        }
    }
    
    std::replace(filename.begin(), filename.end(), '_', ' ');
    return filename;
}

std::string MetadataParser::extract_artist_from_filename(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    std::string filename = p.stem().string();
    
    size_t dash_pos = filename.find(" - ");
    if (dash_pos != std::string::npos) {
        std::string artist = filename.substr(0, dash_pos);
        if (artist.length() > 3 && std::isdigit(artist[0]) && std::isdigit(artist[1])) {
            if (artist[2] == '.' || artist[2] == ' ' || artist[2] == '_') {
                artist = artist.substr(3);
            }
        }
        return artist;
    }
    return "Unknown Artist";
}

std::string MetadataParser::extract_album_from_path(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    fs::path parent = p.parent_path();
    if (!parent.empty()) {
        return parent.filename().string();
    }
    return "Unknown Album";
}

}  // namespace ouroboros::backend
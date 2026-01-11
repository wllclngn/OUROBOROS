#include "backend/Library.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/ArtworkCache.hpp"
#include "util/Logger.hpp"
#include "util/DirectoryScanner.hpp"
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>  // For stat() to get inode
#include <algorithm>
#include <thread>
#include <atomic>
#include <vector>

namespace ouroboros::backend {

// Binary format version magic
constexpr uint32_t CACHE_MAGIC = 0x4F55524F; // 'OURO'
constexpr uint32_t CACHE_VERSION = 3;  // v3: Added file_mtime and file_inode for optimization

// Helper to read/write string
static void write_string(std::ofstream& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.length());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0) out.write(s.data(), len);
}

static std::string read_string(std::ifstream& in) {
    uint32_t len;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (len > 0) {
        std::string s(len, '\0');
        in.read(s.data(), len);
        return s;
    }
    return "";
}

Library::Library() {}

void Library::set_music_directory(const std::filesystem::path& dir) {
    // Legacy single-directory support
    music_dirs_.clear();
    music_dirs_.push_back(dir);
}

void Library::set_music_directories(const std::vector<std::filesystem::path>& dirs) {
    music_dirs_.clear();
    for (const auto& dir : dirs) {
        // Only add directories that exist (gracefully handle unmounted drives)
        if (std::filesystem::exists(dir)) {
            music_dirs_.push_back(dir);
        } else {
            ouroboros::util::Logger::warn("Skipping non-existent directory: " + dir.string());
        }
    }
}

bool Library::save_to_cache(const std::filesystem::path& cache_path) const {
    try {
        std::filesystem::create_directories(cache_path.parent_path());
        std::ofstream out(cache_path, std::ios::binary);
        if (!out) return false;

        // Header
        out.write(reinterpret_cast<const char*>(&CACHE_MAGIC), sizeof(CACHE_MAGIC));
        out.write(reinterpret_cast<const char*>(&CACHE_VERSION), sizeof(CACHE_VERSION));

        // Track count
        uint64_t count = tracks_.size();
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));

        for (const auto& [path, t] : tracks_) {
            write_string(out, t.path);
            write_string(out, t.title);
            write_string(out, t.artist);
            write_string(out, t.album);
            write_string(out, t.genre);
            write_string(out, t.date);

            // POD fields
            out.write(reinterpret_cast<const char*>(&t.track_number), sizeof(t.track_number));
            out.write(reinterpret_cast<const char*>(&t.duration_ms), sizeof(t.duration_ms));
            out.write(reinterpret_cast<const char*>(&t.format), sizeof(t.format));
            out.write(reinterpret_cast<const char*>(&t.sample_rate), sizeof(t.sample_rate));
            out.write(reinterpret_cast<const char*>(&t.channels), sizeof(t.channels));
            out.write(reinterpret_cast<const char*>(&t.bit_depth), sizeof(t.bit_depth));
            out.write(reinterpret_cast<const char*>(&t.bitrate), sizeof(t.bitrate));

            // Phase 4: Serialize artwork hash
            write_string(out, t.artwork_hash);

            // CACHE_VERSION 3: Optimization fields
            out.write(reinterpret_cast<const char*>(&t.file_mtime), sizeof(t.file_mtime));
            out.write(reinterpret_cast<const char*>(&t.file_inode), sizeof(t.file_inode));

            // is_valid
            out.write(reinterpret_cast<const char*>(&t.is_valid), sizeof(t.is_valid));
        }
        
        return true;
    } catch (const std::exception& e) {
        util::Logger::error("Failed to save library cache: " + std::string(e.what()));
        return false;
    }
}

bool Library::load_from_cache(const std::filesystem::path& cache_path) {
    if (!std::filesystem::exists(cache_path)) return false;

    try {
        std::ifstream in(cache_path, std::ios::binary);
        if (!in) return false;

        uint32_t magic, version;
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != CACHE_MAGIC) return false;
        
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version != CACHE_VERSION && version != 2) {
            util::Logger::error("Unsupported cache version: " + std::to_string(version));
            return false;  // Only support v2 and v3
        }

        uint64_t count;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));

        std::unordered_map<std::string, model::Track> loaded_tracks;
        loaded_tracks.reserve(count);

        for (uint64_t i = 0; i < count; ++i) {
            model::Track t;
            t.path = read_string(in);
            t.title = read_string(in);
            t.artist = read_string(in);
            t.album = read_string(in);
            t.genre = read_string(in);
            t.date = read_string(in);
            
            in.read(reinterpret_cast<char*>(&t.track_number), sizeof(t.track_number));
            in.read(reinterpret_cast<char*>(&t.duration_ms), sizeof(t.duration_ms));
            in.read(reinterpret_cast<char*>(&t.format), sizeof(t.format));
            in.read(reinterpret_cast<char*>(&t.sample_rate), sizeof(t.sample_rate));
            in.read(reinterpret_cast<char*>(&t.channels), sizeof(t.channels));
            in.read(reinterpret_cast<char*>(&t.bit_depth), sizeof(t.bit_depth));
            in.read(reinterpret_cast<char*>(&t.bitrate), sizeof(t.bitrate));

            // Phase 4: Deserialize artwork hash
            t.artwork_hash = read_string(in);

            // CACHE_VERSION 3: Optimization fields
            if (version >= 3) {
                in.read(reinterpret_cast<char*>(&t.file_mtime), sizeof(t.file_mtime));
                in.read(reinterpret_cast<char*>(&t.file_inode), sizeof(t.file_inode));
            } else {
                // v2 cache: populate mtime from filesystem
                if (std::filesystem::exists(t.path)) {
                    auto ftime = std::filesystem::last_write_time(t.path);
                    t.file_mtime = std::chrono::system_clock::to_time_t(
                        std::chrono::file_clock::to_sys(ftime)
                    );
                }
            }

            in.read(reinterpret_cast<char*>(&t.is_valid), sizeof(t.is_valid));
            
            // Verify file still exists
            if (std::filesystem::exists(t.path)) {
                loaded_tracks[t.path] = t;
            }
        }

        tracks_ = std::move(loaded_tracks);
        return true;
    } catch (const std::exception& e) {
        util::Logger::error("Failed to load library cache: " + std::string(e.what()));
        return false;
    }
}

void Library::scan_directory(const std::function<void(int scanned, int total)>& progress_callback) {
    ouroboros::util::Logger::info("Library: Starting directory scan for " + std::to_string(music_dirs_.size()) + " directories");

    is_scanning_ = true;

    // OPTIMIZED SINGLE-PASS SCAN using getdents64 (Phase 2 + Phase 4)
    // Scan all configured directories and merge results
    util::DirectoryScanner::ScanResult scan_result;
    for (const auto& music_dir : music_dirs_) {
        util::Logger::info("Library: Scanning directory: " + music_dir.string());
        auto dir_result = util::DirectoryScanner::scan_directory(music_dir);

        // Merge results
        scan_result.audio_files.insert(scan_result.audio_files.end(),
                                        dir_result.audio_files.begin(),
                                        dir_result.audio_files.end());
        scan_result.dir_mtimes.insert(dir_result.dir_mtimes.begin(), dir_result.dir_mtimes.end());
        scan_result.file_mtimes.insert(dir_result.file_mtimes.begin(), dir_result.file_mtimes.end());
        scan_result.tree_hash ^= dir_result.tree_hash;  // XOR combine hashes
    }

    const int total_files = scan_result.audio_files.size();
    util::Logger::info("Library: Found " + std::to_string(total_files) + " audio files");

    // Store directory mtimes for TIER 1 validation
    dir_mtimes_ = scan_result.dir_mtimes;
    last_tree_hash_ = scan_result.tree_hash;

    // Categorize files: cached vs new
    std::vector<std::string> files_to_parse;
    std::unordered_map<std::string, model::Track> new_tracks;

    for (const auto& path_str : scan_result.audio_files) {
        auto it = tracks_.find(path_str);
        if (it != tracks_.end()) {
            // Check if file was modified (mtime comparison)
            auto scan_mtime_it = scan_result.file_mtimes.find(path_str);
            if (scan_mtime_it != scan_result.file_mtimes.end() &&
                it->second.file_mtime > 0 &&
                scan_mtime_it->second <= it->second.file_mtime) {
                // File unchanged - keep cached metadata
                new_tracks[path_str] = it->second;
            } else {
                // File modified or mtime unknown - reparse
                files_to_parse.push_back(path_str);
            }
        } else {
            // New file - parse it
            files_to_parse.push_back(path_str);
        }
    }

    util::Logger::info("Library: Reusing " + std::to_string(new_tracks.size()) +
                      " cached tracks, parsing " + std::to_string(files_to_parse.size()) + " new/modified");

    // PARALLEL PARSING (TIER 3 optimization)
    if (!files_to_parse.empty()) {
        const size_t num_threads = std::thread::hardware_concurrency();
        const size_t num_files = files_to_parse.size();

        util::Logger::info("TIER 3: Parsing " + std::to_string(num_files) +
                          " files with " + std::to_string(num_threads) + " threads");

        std::atomic<size_t> work_index{0};
        std::atomic<int> completed{0};
        std::vector<model::Track> results(num_files);

        // Launch worker threads
        std::vector<std::thread> workers;
        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t idx = work_index.fetch_add(1);
                    if (idx >= num_files) break;

                    const std::string& path_str = files_to_parse[idx];
                    std::filesystem::path path(path_str);

                    // Parse metadata (thread-safe)
                    model::Track track = MetadataParser::parse_file(path_str);

                    // Ensure format is set
                    if (track.format == model::AudioFormat::Unknown) {
                        if (path.extension() == ".mp3") track.format = model::AudioFormat::MP3;
                        else if (path.extension() == ".flac") track.format = model::AudioFormat::FLAC;
                        else if (path.extension() == ".ogg") track.format = model::AudioFormat::OGG;
                        else if (path.extension() == ".wav") track.format = model::AudioFormat::WAV;
                    }

                    // Populate optimization fields
                    auto scan_mtime_it = scan_result.file_mtimes.find(path_str);
                    if (scan_mtime_it != scan_result.file_mtimes.end()) {
                        track.file_mtime = scan_mtime_it->second;
                    }

                    struct stat st;
                    if (stat(path_str.c_str(), &st) == 0) {
                        track.file_inode = st.st_ino;
                    }

                    // Extract artwork
                    auto artwork_result = MetadataParser::extract_artwork_data(path_str);
                    if (!artwork_result.data.empty() && !artwork_result.hash.empty()) {
                        track.artwork_hash = artwork_result.hash;
                        auto& artwork_cache = ArtworkCache::instance();
                        artwork_cache.store(artwork_result.hash, artwork_result.data, artwork_result.mime_type, path.parent_path().string());
                    }

                    results[idx] = track;

                    // Update progress every 100 files
                    int done = completed.fetch_add(1) + 1;
                    if (progress_callback && done % 100 == 0) {
                        progress_callback(new_tracks.size() + done, total_files);
                    }
                }
            });
        }

        // Wait for all threads
        for (auto& worker : workers) {
            worker.join();
        }

        // Merge results
        for (size_t i = 0; i < num_files; ++i) {
            new_tracks[files_to_parse[i]] = std::move(results[i]);
        }
    }

    // Final progress update
    if (progress_callback && total_files > 0) {
        progress_callback(total_files, total_files);
    }

    // Metadata is now fully populated by MetadataParser during scan
    tracks_ = std::move(new_tracks);
    is_scanning_ = false;
}

std::vector<model::Track> Library::get_all_tracks() const {
    std::vector<model::Track> result;
    result.reserve(tracks_.size());
    for (const auto& [path, track] : tracks_) {
        result.push_back(track);
    }
    return result;
}

std::optional<model::Track> Library::get_track_by_path(const std::filesystem::path& path) const {
    auto it = tracks_.find(path.string());
    if (it != tracks_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void Library::set_tracks(const std::vector<model::Track>& tracks) {
    tracks_.clear();
    tracks_.reserve(tracks.size());
    for (const auto& t : tracks) {
        tracks_[t.path] = t;
    }
}

size_t Library::get_track_count() const {
    return tracks_.size();
}

bool Library::is_scanning() const {
    return is_scanning_;
}

// ═══════════════════════════════════════════════════════════════════════════
// MULTI-TIER CACHE VALIDATION (Phase 3)
// ═══════════════════════════════════════════════════════════════════════════

Library::CacheValidationResult Library::validate_cache_tier0(const std::filesystem::path& cache_path) {
    (void)cache_path;  // Unused for now
    util::Logger::info("TIER 0: Validating cache with tree hash");

    // Load cache header only (first 256 bytes would contain tree_hash in future)
    // For now, we'll do a quick directory scan and compare
    // Scan all configured directories and merge results
    util::DirectoryScanner::ScanResult scan_result;
    for (const auto& music_dir : music_dirs_) {
        auto dir_result = util::DirectoryScanner::scan_directory(music_dir);
        scan_result.audio_files.insert(scan_result.audio_files.end(),
                                        dir_result.audio_files.begin(),
                                        dir_result.audio_files.end());
        scan_result.dir_mtimes.insert(dir_result.dir_mtimes.begin(), dir_result.dir_mtimes.end());
        scan_result.file_mtimes.insert(dir_result.file_mtimes.begin(), dir_result.file_mtimes.end());
        scan_result.tree_hash ^= dir_result.tree_hash;  // XOR combine
    }

    // Store for future use
    last_tree_hash_ = scan_result.tree_hash;
    dir_mtimes_ = scan_result.dir_mtimes;

    // Compare file counts as quick check
    if (scan_result.audio_files.size() != tracks_.size()) {
        util::Logger::info("TIER 0: File count mismatch (" +
                          std::to_string(scan_result.audio_files.size()) + " vs " +
                          std::to_string(tracks_.size()) + ")");
        return CacheValidationResult::CountMismatch;
    }

    // Quick check: all cached files still exist
    for (const auto& [path, track] : tracks_) {
        if (!std::filesystem::exists(path)) {
            util::Logger::info("TIER 0: Cached file no longer exists: " + path);
            return CacheValidationResult::MissingFiles;
        }
    }

    util::Logger::info("TIER 0: Cache validation passed");
    return CacheValidationResult::Valid;
}

std::vector<std::string> Library::find_dirty_directories(
    const std::unordered_map<std::string, std::time_t>& current_dir_mtimes,
    const std::unordered_map<std::string, std::time_t>& cached_dir_mtimes
) {
    std::vector<std::string> dirty_dirs;

    util::Logger::info("TIER 1: Checking for dirty directories");

    // Check for new or modified directories
    for (const auto& [dir, mtime] : current_dir_mtimes) {
        auto it = cached_dir_mtimes.find(dir);
        if (it == cached_dir_mtimes.end()) {
            // New directory
            dirty_dirs.push_back(dir);
            util::Logger::debug("TIER 1: New directory: " + dir);
        } else if (it->second < mtime) {
            // Modified directory
            dirty_dirs.push_back(dir);
            util::Logger::debug("TIER 1: Modified directory: " + dir);
        }
    }

    // Check for deleted directories
    for (const auto& [dir, mtime] : cached_dir_mtimes) {
        if (current_dir_mtimes.find(dir) == current_dir_mtimes.end()) {
            dirty_dirs.push_back(dir);
            util::Logger::debug("TIER 1: Deleted directory: " + dir);
        }
    }

    util::Logger::info("TIER 1: Found " + std::to_string(dirty_dirs.size()) + " dirty directories");
    return dirty_dirs;
}

void Library::scan_for_changes(
    const std::vector<std::string>& changed_files,
    const std::vector<std::string>& deleted_files,
    const std::function<void(int, int)>& progress_callback
) {
    util::Logger::info("TIER 2: Scanning for changes (" +
                      std::to_string(changed_files.size()) + " changed, " +
                      std::to_string(deleted_files.size()) + " deleted)");

    int processed = 0;
    int total = changed_files.size() + deleted_files.size();

    // Remove deleted files
    for (const auto& path : deleted_files) {
        tracks_.erase(path);
        processed++;
        if (progress_callback && processed % 10 == 0) {
            progress_callback(processed, total);
        }
    }

    // Parse changed/new files IN PARALLEL (TIER 3 optimization)
    if (!changed_files.empty()) {
        const size_t num_threads = std::thread::hardware_concurrency();
        const size_t num_files = changed_files.size();

        util::Logger::info("TIER 3: Parsing " + std::to_string(num_files) +
                          " files with " + std::to_string(num_threads) + " threads");

        // Shared atomic counter for work distribution
        std::atomic<size_t> work_index{0};
        std::atomic<int> completed{0};

        // Results storage (one per file)
        std::vector<model::Track> results(num_files);

        // Launch worker threads
        std::vector<std::thread> workers;
        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t idx = work_index.fetch_add(1);
                    if (idx >= num_files) break;

                    const std::string& path_str = changed_files[idx];
                    std::filesystem::path path(path_str);

                    // Parse metadata (thread-safe)
                    model::Track track = MetadataParser::parse_file(path_str);

                    // Ensure format is set
                    if (track.format == model::AudioFormat::Unknown) {
                        if (path.extension() == ".mp3") track.format = model::AudioFormat::MP3;
                        else if (path.extension() == ".flac") track.format = model::AudioFormat::FLAC;
                        else if (path.extension() == ".ogg") track.format = model::AudioFormat::OGG;
                        else if (path.extension() == ".wav") track.format = model::AudioFormat::WAV;
                    }

                    // Populate optimization fields
                    try {
                        auto ftime = std::filesystem::last_write_time(path);
                        track.file_mtime = std::chrono::system_clock::to_time_t(
                            std::chrono::file_clock::to_sys(ftime)
                        );

                        struct stat st;
                        if (stat(path_str.c_str(), &st) == 0) {
                            track.file_inode = st.st_ino;
                        }
                    } catch (...) {
                        // Failed to get mtime/inode, continue anyway
                    }

                    // Extract artwork
                    auto artwork_result = MetadataParser::extract_artwork_data(path_str);
                    if (!artwork_result.data.empty() && !artwork_result.hash.empty()) {
                        track.artwork_hash = artwork_result.hash;
                        auto& artwork_cache = ArtworkCache::instance();
                        artwork_cache.store(artwork_result.hash, artwork_result.data, artwork_result.mime_type, path.parent_path().string());
                    }

                    // Store result
                    results[idx] = track;

                    // Update progress every 100 files
                    int done = completed.fetch_add(1) + 1;
                    if (progress_callback && done % 100 == 0) {
                        progress_callback(deleted_files.size() + done, total);
                    }
                }
            });
        }

        // Wait for all threads to complete
        for (auto& worker : workers) {
            worker.join();
        }

        // Merge results into tracks_ map (single-threaded for safety)
        for (size_t i = 0; i < num_files; ++i) {
            tracks_[changed_files[i]] = results[i];
        }

        processed = deleted_files.size() + num_files;
    }

    // Final progress update
    if (progress_callback && total > 0) {
        progress_callback(total, total);
    }

    util::Logger::info("TIER 2: Scan complete");
}

}  // namespace ouroboros::backend
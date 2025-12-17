#include "backend/HierarchicalCache.hpp"
#include "backend/MetadataParser.hpp"
#include "backend/ArtworkCache.hpp"
#include "util/Logger.hpp"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <openssl/sha.h>

namespace ouroboros::backend {

HierarchicalCache::HierarchicalCache() {
    util::Logger::info("HierarchicalCache: Initialized");
}

void HierarchicalCache::set_cache_root(const std::filesystem::path& cache_root) {
    cache_root_ = cache_root;
    util::Logger::info("HierarchicalCache: Cache root set to " + cache_root_.string());
}

void HierarchicalCache::set_music_root(const std::filesystem::path& music_root) {
    music_root_ = music_root;
    util::Logger::info("HierarchicalCache: Music root set to " + music_root_.string());
}

std::vector<std::filesystem::path> HierarchicalCache::get_top_level_directories() const {
    std::vector<std::filesystem::path> dirs;

    if (!std::filesystem::exists(music_root_)) {
        util::Logger::warn("HierarchicalCache: Music root does not exist: " + music_root_.string());
        return dirs;
    }

    for (const auto& entry : std::filesystem::directory_iterator(music_root_)) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        }
    }

    util::Logger::info("HierarchicalCache: Found " + std::to_string(dirs.size()) + " top-level directories");
    return dirs;
}

std::filesystem::path HierarchicalCache::get_top_level_directory_for_track(
    const std::string& track_path
) const {
    std::filesystem::path p(track_path);

    // Find the first directory under music_root_
    while (p.has_parent_path() && p.parent_path() != music_root_) {
        p = p.parent_path();
    }

    return p;
}

std::string HierarchicalCache::compute_directory_tree_hash(const std::filesystem::path& directory) {
    // Simple hash: concatenate all file paths + mtimes, then SHA-256
    std::ostringstream tree_data;

    // Handle single file case (root files)
    if (std::filesystem::is_regular_file(directory)) {
        auto mtime = std::filesystem::last_write_time(directory);
        tree_data << directory.string() << ":"
                 << mtime.time_since_epoch().count() << "\n";
    } else {
        // Recursive scan for directories
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                auto mtime = std::filesystem::last_write_time(entry.path());
                tree_data << entry.path().string() << ":"
                         << mtime.time_since_epoch().count() << "\n";
            }
        }
    }

    std::string data = tree_data.str();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash);

    std::ostringstream hex;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }

    return hex.str();
}

void HierarchicalCache::generate_hierarchical_caches(
    const std::unordered_map<std::string, model::Track>& all_tracks
) {
    util::Logger::info("HierarchicalCache: Starting hierarchical cache generation for " +
                      std::to_string(all_tracks.size()) + " tracks");

    auto top_level_dirs = get_top_level_directories();

    // Partition tracks by top-level directory
    std::unordered_map<std::string, std::vector<model::Track>> partitioned_tracks;

    for (const auto& [path, track] : all_tracks) {
        auto top_dir = get_top_level_directory_for_track(path);
        partitioned_tracks[top_dir.string()].push_back(track);
    }

    util::Logger::info("HierarchicalCache: Partitioned into " +
                      std::to_string(partitioned_tracks.size()) + " directories");

    // Generate per-directory caches
    CacheManifest manifest;
    manifest.music_root = music_root_.string();
    manifest.last_global_scan = std::chrono::system_clock::now();

    for (const auto& [dir_path, tracks] : partitioned_tracks) {
        std::filesystem::path dir(dir_path);
        std::string dir_name = dir.filename().string();

        util::Logger::info("HierarchicalCache: Generating cache for " + dir_name +
                          " (" + std::to_string(tracks.size()) + " tracks)");

        generate_directory_cache(dir, tracks);

        // Add to manifest
        DirectoryMetadata dir_meta;
        dir_meta.path = dir_name;
        dir_meta.track_count = tracks.size();
        dir_meta.cache_file = "directories/" + dir_name + "/metadata.bin";
        dir_meta.artwork_file = "directories/" + dir_name + "/artwork.cache";
        dir_meta.last_scanned = std::chrono::system_clock::now();
        dir_meta.tree_hash = compute_directory_tree_hash(dir);

        manifest.directories.push_back(dir_meta);
    }

    // Generate global index
    generate_global_index(top_level_dirs, all_tracks);

    // Save manifest
    save_manifest(manifest);

    util::Logger::info("HierarchicalCache: Hierarchical cache generation complete");
}

void HierarchicalCache::generate_directory_cache(
    const std::filesystem::path& directory,
    const std::vector<model::Track>& tracks
) {
    std::string dir_name = directory.filename().string();
    std::filesystem::path cache_dir = cache_root_ / "directories" / dir_name;
    std::filesystem::create_directories(cache_dir);

    std::filesystem::path cache_file = cache_dir / "metadata.bin";
    std::ofstream out(cache_file, std::ios::binary);

    if (!out) {
        util::Logger::error("HierarchicalCache: Failed to create cache file: " + cache_file.string());
        return;
    }

    // Write header
    out.write(reinterpret_cast<const char*>(&DIRECTORY_CACHE_MAGIC), sizeof(uint64_t));
    out.write(reinterpret_cast<const char*>(&FORMAT_VERSION), sizeof(uint32_t));

    // Write parent directory
    std::string parent = directory.string();
    uint32_t parent_len = parent.size();
    out.write(reinterpret_cast<const char*>(&parent_len), sizeof(uint32_t));
    out.write(parent.c_str(), parent_len);

    // Write track count
    uint64_t track_count = tracks.size();
    out.write(reinterpret_cast<const char*>(&track_count), sizeof(uint64_t));

    // Write track metadata
    write_directory_metadata(out, tracks);

    util::Logger::debug("HierarchicalCache: Wrote " + std::to_string(tracks.size()) +
                       " tracks to " + cache_file.filename().string());
}

void HierarchicalCache::write_directory_metadata(
    std::ofstream& out,
    const std::vector<model::Track>& tracks
) {
    // Use same format as current library.bin for compatibility
    for (const auto& track : tracks) {
        // Path
        uint32_t path_len = track.path.size();
        out.write(reinterpret_cast<const char*>(&path_len), sizeof(uint32_t));
        out.write(track.path.c_str(), path_len);

        // Title
        uint32_t title_len = track.title.size();
        out.write(reinterpret_cast<const char*>(&title_len), sizeof(uint32_t));
        out.write(track.title.c_str(), title_len);

        // Artist
        uint32_t artist_len = track.artist.size();
        out.write(reinterpret_cast<const char*>(&artist_len), sizeof(uint32_t));
        out.write(track.artist.c_str(), artist_len);

        // Album
        uint32_t album_len = track.album.size();
        out.write(reinterpret_cast<const char*>(&album_len), sizeof(uint32_t));
        out.write(track.album.c_str(), album_len);

        // Duration
        out.write(reinterpret_cast<const char*>(&track.duration_ms), sizeof(int));
    }
}

void HierarchicalCache::generate_global_index(
    const std::vector<std::filesystem::path>& directories,
    const std::unordered_map<std::string, model::Track>& all_tracks
) {
    util::Logger::info("HierarchicalCache: Generating global index for " +
                      std::to_string(all_tracks.size()) + " tracks");

    GlobalIndex index;

    // Build directory list
    for (const auto& dir : directories) {
        index.directories.push_back(dir.filename().string());
    }

    // Build track index entries (lightweight)
    for (const auto& [path, track] : all_tracks) {
        IndexEntry entry;
        entry.path = path;
        entry.title = track.title;
        entry.artist = track.artist;
        entry.album = track.album;

        // Find directory index
        auto top_dir = get_top_level_directory_for_track(path);
        auto it = std::find(index.directories.begin(), index.directories.end(),
                           top_dir.filename().string());
        entry.directory_index = (it != index.directories.end())
                               ? std::distance(index.directories.begin(), it)
                               : 0;

        // Artwork hash (empty for now, can be populated later)
        entry.artwork_hash = "";

        index.tracks.push_back(entry);
    }

    write_global_index(index);

    util::Logger::info("HierarchicalCache: Global index written");
}

void HierarchicalCache::write_global_index(const GlobalIndex& index) {
    std::filesystem::path index_file = cache_root_ / "index.bin";
    std::ofstream out(index_file, std::ios::binary);

    if (!out) {
        util::Logger::error("HierarchicalCache: Failed to create index file");
        return;
    }

    // Header
    out.write(reinterpret_cast<const char*>(&GLOBAL_INDEX_MAGIC), sizeof(uint64_t));
    out.write(reinterpret_cast<const char*>(&FORMAT_VERSION), sizeof(uint32_t));

    // Total track count
    uint64_t total_tracks = index.tracks.size();
    out.write(reinterpret_cast<const char*>(&total_tracks), sizeof(uint64_t));

    // Directory count
    uint32_t dir_count = index.directories.size();
    out.write(reinterpret_cast<const char*>(&dir_count), sizeof(uint32_t));

    // Write directories
    for (const auto& dir : index.directories) {
        uint32_t len = dir.size();
        out.write(reinterpret_cast<const char*>(&len), sizeof(uint32_t));
        out.write(dir.c_str(), len);
    }

    // Write track entries
    for (const auto& entry : index.tracks) {
        // Path
        uint32_t path_len = entry.path.size();
        out.write(reinterpret_cast<const char*>(&path_len), sizeof(uint32_t));
        out.write(entry.path.c_str(), path_len);

        // Title
        uint16_t title_len = std::min<size_t>(entry.title.size(), 65535);
        out.write(reinterpret_cast<const char*>(&title_len), sizeof(uint16_t));
        out.write(entry.title.c_str(), title_len);

        // Artist
        uint16_t artist_len = std::min<size_t>(entry.artist.size(), 65535);
        out.write(reinterpret_cast<const char*>(&artist_len), sizeof(uint16_t));
        out.write(entry.artist.c_str(), artist_len);

        // Album
        uint16_t album_len = std::min<size_t>(entry.album.size(), 65535);
        out.write(reinterpret_cast<const char*>(&album_len), sizeof(uint16_t));
        out.write(entry.album.c_str(), album_len);

        // Directory index
        out.write(reinterpret_cast<const char*>(&entry.directory_index), sizeof(uint32_t));

        // Artwork hash (64 bytes fixed)
        char hash_buf[64] = {0};
        std::copy_n(entry.artwork_hash.c_str(),
                   std::min<size_t>(entry.artwork_hash.size(), 64), hash_buf);
        out.write(hash_buf, 64);
    }

    util::Logger::info("HierarchicalCache: Wrote index.bin (" +
                      std::to_string(index.tracks.size()) + " entries)");
}

GlobalIndex HierarchicalCache::load_global_index() {
    util::Logger::debug("HierarchicalCache: Loading global index");

    GlobalIndex index;
    std::filesystem::path index_file = cache_root_ / "index.bin";

    if (!std::filesystem::exists(index_file)) {
        util::Logger::warn("HierarchicalCache: index.bin not found");
        return index;
    }

    std::ifstream in(index_file, std::ios::binary);
    if (!in) {
        util::Logger::error("HierarchicalCache: Failed to open index.bin");
        return index;
    }

    // Read header
    uint64_t magic;
    uint32_t version;
    in.read(reinterpret_cast<char*>(&magic), sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));

    if (magic != GLOBAL_INDEX_MAGIC) {
        util::Logger::error("HierarchicalCache: Invalid index.bin magic number");
        return index;
    }

    // Read counts
    uint64_t total_tracks;
    uint32_t dir_count;
    in.read(reinterpret_cast<char*>(&total_tracks), sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(&dir_count), sizeof(uint32_t));

    // Read directories
    for (uint32_t i = 0; i < dir_count; i++) {
        uint32_t len;
        in.read(reinterpret_cast<char*>(&len), sizeof(uint32_t));
        std::string dir(len, '\0');
        in.read(&dir[0], len);
        index.directories.push_back(dir);
    }

    // Read track entries
    for (uint64_t i = 0; i < total_tracks; i++) {
        IndexEntry entry;

        uint32_t path_len;
        in.read(reinterpret_cast<char*>(&path_len), sizeof(uint32_t));
        entry.path.resize(path_len);
        in.read(&entry.path[0], path_len);

        uint16_t title_len;
        in.read(reinterpret_cast<char*>(&title_len), sizeof(uint16_t));
        entry.title.resize(title_len);
        in.read(&entry.title[0], title_len);

        uint16_t artist_len;
        in.read(reinterpret_cast<char*>(&artist_len), sizeof(uint16_t));
        entry.artist.resize(artist_len);
        in.read(&entry.artist[0], artist_len);

        uint16_t album_len;
        in.read(reinterpret_cast<char*>(&album_len), sizeof(uint16_t));
        entry.album.resize(album_len);
        in.read(&entry.album[0], album_len);

        in.read(reinterpret_cast<char*>(&entry.directory_index), sizeof(uint32_t));

        char hash_buf[64];
        in.read(hash_buf, 64);
        entry.artwork_hash = std::string(hash_buf, 64);

        index.tracks.push_back(entry);
    }

    util::Logger::info("HierarchicalCache: Loaded global index (" +
                      std::to_string(index.tracks.size()) + " tracks)");

    return index;
}

std::unordered_map<std::string, model::Track> HierarchicalCache::load_directory(
    const std::filesystem::path& directory
) {
    std::string dir_name = directory.filename().string();

    util::Logger::info("HierarchicalCache: Loading directory cache for " + dir_name);

    std::unordered_map<std::string, model::Track> tracks;

    std::filesystem::path cache_file = cache_root_ / "directories" / dir_name / "metadata.bin";

    if (!std::filesystem::exists(cache_file)) {
        util::Logger::warn("HierarchicalCache: Cache file not found: " + cache_file.string());
        return tracks;
    }

    std::ifstream in(cache_file, std::ios::binary);
    if (!in) {
        util::Logger::error("HierarchicalCache: Failed to open cache file");
        return tracks;
    }

    // Read header
    uint64_t magic;
    uint32_t version;
    in.read(reinterpret_cast<char*>(&magic), sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));

    if (magic != DIRECTORY_CACHE_MAGIC) {
        util::Logger::error("HierarchicalCache: Invalid cache magic number");
        return tracks;
    }

    // Read parent directory
    uint32_t parent_len;
    in.read(reinterpret_cast<char*>(&parent_len), sizeof(uint32_t));
    std::string parent(parent_len, '\0');
    in.read(&parent[0], parent_len);

    // Read track count
    uint64_t track_count;
    in.read(reinterpret_cast<char*>(&track_count), sizeof(uint64_t));

    // Read tracks
    auto track_list = read_directory_metadata(in);

    for (const auto& track : track_list) {
        tracks[track.path] = track;
    }

    // Cache in memory
    loaded_directories_[dir_name] = tracks;

    util::Logger::info("HierarchicalCache: Loaded " + std::to_string(tracks.size()) +
                      " tracks from " + dir_name);

    return tracks;
}

std::vector<model::Track> HierarchicalCache::read_directory_metadata(std::ifstream& in) {
    std::vector<model::Track> tracks;

    while (in.peek() != EOF) {
        model::Track track;

        uint32_t path_len;
        if (!in.read(reinterpret_cast<char*>(&path_len), sizeof(uint32_t))) break;
        track.path.resize(path_len);
        in.read(&track.path[0], path_len);

        uint32_t title_len;
        in.read(reinterpret_cast<char*>(&title_len), sizeof(uint32_t));
        track.title.resize(title_len);
        in.read(&track.title[0], title_len);

        uint32_t artist_len;
        in.read(reinterpret_cast<char*>(&artist_len), sizeof(uint32_t));
        track.artist.resize(artist_len);
        in.read(&track.artist[0], artist_len);

        uint32_t album_len;
        in.read(reinterpret_cast<char*>(&album_len), sizeof(uint32_t));
        track.album.resize(album_len);
        in.read(&track.album[0], album_len);

        in.read(reinterpret_cast<char*>(&track.duration_ms), sizeof(int));

        tracks.push_back(track);
    }

    return tracks;
}

void HierarchicalCache::unload_directory(const std::filesystem::path& directory) {
    std::string dir_name = directory.filename().string();

    auto it = loaded_directories_.find(dir_name);
    if (it != loaded_directories_.end()) {
        util::Logger::info("HierarchicalCache: Unloading directory " + dir_name +
                          " (" + std::to_string(it->second.size()) + " tracks)");
        loaded_directories_.erase(it);
    }
}

void HierarchicalCache::unload_all() {
    util::Logger::info("HierarchicalCache: Unloading all directories (" +
                      std::to_string(loaded_directories_.size()) + " loaded)");
    loaded_directories_.clear();
}

std::vector<std::string> HierarchicalCache::search_index(
    const GlobalIndex& index,
    const std::string& query
) {
    util::Logger::debug("HierarchicalCache: Searching index for: " + query);

    std::vector<std::string> results;
    std::string lower_query = query;
    std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    for (const auto& entry : index.tracks) {
        std::string lower_title = entry.title;
        std::string lower_artist = entry.artist;
        std::transform(lower_title.begin(), lower_title.end(), lower_title.begin(), ::tolower);
        std::transform(lower_artist.begin(), lower_artist.end(), lower_artist.begin(), ::tolower);

        if (lower_title.find(lower_query) != std::string::npos ||
            lower_artist.find(lower_query) != std::string::npos) {
            results.push_back(entry.path);
        }
    }

    util::Logger::info("HierarchicalCache: Search found " + std::to_string(results.size()) + " results");

    return results;
}

CacheManifest HierarchicalCache::load_manifest() {
    CacheManifest manifest;

    std::filesystem::path manifest_file = cache_root_ / "config" / "cache_manifest.json";

    if (!std::filesystem::exists(manifest_file)) {
        util::Logger::warn("HierarchicalCache: cache_manifest.json not found");
        return manifest;
    }

    std::ifstream in(manifest_file);
    if (!in) {
        util::Logger::error("HierarchicalCache: Failed to open manifest");
        return manifest;
    }

    // TODO: Parse JSON (requires JSON library)
    // For now, return empty manifest

    util::Logger::debug("HierarchicalCache: Loaded manifest");

    return manifest;
}

void HierarchicalCache::save_manifest(const CacheManifest& manifest) {
    std::filesystem::path manifest_dir = cache_root_ / "config";
    std::filesystem::create_directories(manifest_dir);

    std::filesystem::path manifest_file = manifest_dir / "cache_manifest.json";
    std::ofstream out(manifest_file);

    if (!out) {
        util::Logger::error("HierarchicalCache: Failed to create manifest file");
        return;
    }

    // Write JSON manually (simple format)
    out << "{\n";
    out << "  \"version\": " << manifest.version << ",\n";
    out << "  \"music_root\": \"" << manifest.music_root << "\",\n";
    out << "  \"directories\": [\n";

    for (size_t i = 0; i < manifest.directories.size(); i++) {
        const auto& dir = manifest.directories[i];
        out << "    {\n";
        out << "      \"path\": \"" << dir.path << "\",\n";
        out << "      \"track_count\": " << dir.track_count << ",\n";
        out << "      \"cache_file\": \"" << dir.cache_file << "\",\n";
        out << "      \"artwork_file\": \"" << dir.artwork_file << "\",\n";
        out << "      \"tree_hash\": \"" << dir.tree_hash << "\"\n";
        out << "    }";
        if (i < manifest.directories.size() - 1) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    util::Logger::info("HierarchicalCache: Saved cache_manifest.json (" +
                      std::to_string(manifest.directories.size()) + " directories)");
}

std::vector<std::string> HierarchicalCache::get_loaded_directory_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : loaded_directories_) {
        names.push_back(name);
    }
    return names;
}

} // namespace ouroboros::backend

#include "collectors/LibraryCollector.hpp"
#include "backend/Library.hpp"
#include "backend/Config.hpp"
#include "util/Platform.hpp"
#include "util/Logger.hpp"
#include "util/TimSort.hpp"
#include "util/DirectoryScanner.hpp"
#include "util/UnicodeUtils.hpp"
#include <thread>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <atomic>

namespace ouroboros::collectors {

// Extract primary artist by stripping featuring/collaboration suffixes
static std::string extract_primary_artist(const std::string& artist) {
    static const std::vector<std::string> patterns = {
        " feat. ", " featuring ", " ft. ", " (feat. ", " (feat ",
        " (ft. ", " (ft ", " with "
    };

    std::string lower = artist;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    size_t earliest = std::string::npos;
    for (const auto& p : patterns) {
        size_t pos = lower.find(p);
        if (pos != std::string::npos && pos < earliest) {
            earliest = pos;
        }
    }

    if (earliest != std::string::npos) {
        std::string result = artist.substr(0, earliest);
        // Trim trailing whitespace
        while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
            result.pop_back();
        }
        return result;
    }
    return artist;
}

// Detect if album has scattered artists (compilation)
static bool detect_scattered(const model::AlbumGroup& album, const std::vector<model::Track>& tracks) {
    std::unordered_set<std::string> unique_artists;

    for (int idx : album.track_indices) {
        const auto& track = tracks[idx];

        // Explicit compilation flag
        if (track.is_compilation) return true;

        // Check for "Various Artists" in artist field
        std::string artist_lower = track.artist;
        std::transform(artist_lower.begin(), artist_lower.end(), artist_lower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        if (artist_lower.find("various artists") != std::string::npos) return true;

        // Extract and normalize primary artist
        std::string primary = extract_primary_artist(track.artist);
        std::string normalized = util::normalize_for_search(primary);
        unique_artists.insert(normalized);
    }

    size_t track_count = album.track_indices.size();
    size_t unique_count = unique_artists.size();

    // Thresholds for scattered detection
    if (unique_count > 3) return true;
    if (track_count > 4 && unique_count > track_count / 2) return true;

    return false;
}

// Compute album groups from sorted tracks (called once at library load)
static void compute_album_groups(model::LibraryState& lib_state, const backend::Config& config) {
    util::Logger::info("Computing album groups from " + std::to_string(lib_state.tracks.size()) + " tracks");

    // Helper to get sort key for artist (strips prefixes based on config)
    auto get_artist_sort_key = [&config](const std::string& artist) -> std::string {
        if (artist.empty()) return artist;
        size_t start = 0;
        if (config.sort_ignore_the_prefix && artist.size() >= 4) {
            if ((artist[0] == 'T' || artist[0] == 't') &&
                (artist[1] == 'H' || artist[1] == 'h') &&
                (artist[2] == 'E' || artist[2] == 'e') &&
                artist[3] == ' ') {
                start = 4;
            }
        }
        if (config.sort_ignore_bracket_prefix && start < artist.size() && artist[start] == '[') {
            start++;
        }
        return (start > 0) ? artist.substr(start) : artist;
    };

    // Helper to convert year string to int for numeric comparison
    auto year_to_int = [](const std::string& y) -> int {
        if (y.empty()) return 9999;
        try {
            std::string year_str = y.substr(0, 4);
            return std::stoi(year_str);
        } catch (...) {
            return 9999;
        }
    };

    // ═══════════════════════════════════════════════════════════════════════
    // STEP 1: Group tracks by DIRECTORY (not artist+album)
    // Directory is truth. All tracks in same folder = same album.
    // ═══════════════════════════════════════════════════════════════════════
    std::map<std::string, model::AlbumGroup> groups;

    for (size_t i = 0; i < lib_state.tracks.size(); ++i) {
        const auto& track = lib_state.tracks[i];

        // Linux-native directory extraction (no std::filesystem overhead)
        size_t last_slash = track.path.rfind('/');
        std::string album_dir = (last_slash != std::string::npos)
            ? track.path.substr(0, last_slash)
            : track.path;

        // Key = directory path (unique per album)
        std::string key = album_dir;

        if (groups.find(key) == groups.end()) {
            model::AlbumGroup g;
            g.title = track.album.empty() ? "Unknown Album" : track.album;
            g.artist = track.artist.empty() ? "Unknown Artist" : track.artist;
            g.year = track.date;
            g.normalized_title = util::normalize_for_search(g.title);
            g.normalized_artist = util::normalize_for_search(g.artist);
            g.representative_track_path = track.path;
            g.album_directory = album_dir;
            g.is_scattered = false;  // Will be detected in Step 2
            groups[key] = g;
        }
        groups[key].track_indices.push_back(static_cast<int>(i));
    }

    // Convert to vector
    std::vector<model::AlbumGroup> albums;
    albums.reserve(groups.size());
    for (auto& [k, v] : groups) {
        albums.push_back(std::move(v));
    }

    util::Logger::info("Grouped into " + std::to_string(albums.size()) + " directory-based albums");

    // ═══════════════════════════════════════════════════════════════════════
    // STEP 2: Sort tracks + detect scattered albums IN PARALLEL
    // Each album is independent - perfect for parallel processing
    // Uses atomic work-stealing pattern for load balancing across cores
    // ═══════════════════════════════════════════════════════════════════════
    const size_t num_threads = std::thread::hardware_concurrency();
    const size_t num_albums = albums.size();
    std::atomic<size_t> work_index{0};

    util::Logger::info("Processing " + std::to_string(num_albums) + " albums with " +
                      std::to_string(num_threads) + " threads");

    std::vector<std::thread> workers;
    for (size_t t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
            while (true) {
                size_t idx = work_index.fetch_add(1, std::memory_order_relaxed);
                if (idx >= num_albums) break;

                auto& album = albums[idx];

                // Sort tracks within this album by track number (TimSort for natural runs)
                ouroboros::util::timsort(album.track_indices,
                    [&lib_state](int a, int b) {
                        return lib_state.tracks[a].track_number < lib_state.tracks[b].track_number;
                    });

                // Detect if this album is scattered (compilation)
                album.is_scattered = detect_scattered(album, lib_state.tracks);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    size_t scattered_count = std::count_if(albums.begin(), albums.end(),
        [](const model::AlbumGroup& a) { return a.is_scattered; });
    util::Logger::info("Detected " + std::to_string(scattered_count) + " scattered (compilation) albums");

    // ═══════════════════════════════════════════════════════════════════════
    // STEP 3: Sort albums (parallel)
    // Scattered: by title | Unified: by artist, then year, then title
    // ═══════════════════════════════════════════════════════════════════════
    bool sort_by_year = config.sort_albums_by_year;

    util::Logger::info("Sorting " + std::to_string(albums.size()) + " albums (parallel)");
    ouroboros::util::parallel_timsort(albums,
        [sort_by_year, &get_artist_sort_key, &year_to_int](
            const model::AlbumGroup& a, const model::AlbumGroup& b) {

            // Scattered albums sort by title, unified by artist
            std::string key_a = a.is_scattered ? a.normalized_title : get_artist_sort_key(a.artist);
            std::string key_b = b.is_scattered ? b.normalized_title : get_artist_sort_key(b.artist);

            int cmp = util::case_insensitive_compare(key_a, key_b);
            if (cmp != 0) return cmp < 0;

            // Same primary key, compare by year
            if (sort_by_year) {
                int ya = year_to_int(a.year);
                int yb = year_to_int(b.year);
                if (ya != yb) return ya < yb;
            }

            // Finally by title
            return util::case_insensitive_compare(a.normalized_title, b.normalized_title) < 0;
        });

    lib_state.albums = std::move(albums);
    util::Logger::info("Album groups computed: " + std::to_string(lib_state.albums.size()) + " albums");
}

LibraryCollector::LibraryCollector(std::shared_ptr<backend::SnapshotPublisher> publisher,
                                   const backend::Config& config)
    : publisher_(publisher), config_(config) {}

void LibraryCollector::run(std::stop_token stop_token) {
    backend::Library library;

    // Helper to get sort key for artist (strips prefixes based on config)
    auto get_artist_sort_key = [this](const std::string& artist) -> std::string {
        if (artist.empty()) return artist;

        size_t start = 0;

        // Strip "The " prefix if configured (case-insensitive)
        if (config_.sort_ignore_the_prefix && artist.size() >= 4) {
            if ((artist[0] == 'T' || artist[0] == 't') &&
                (artist[1] == 'H' || artist[1] == 'h') &&
                (artist[2] == 'E' || artist[2] == 'e') &&
                artist[3] == ' ') {
                start = 4;
            }
        }

        // Strip "[" prefix if configured
        if (config_.sort_ignore_bracket_prefix && start < artist.size() && artist[start] == '[') {
            start++;
        }

        return (start > 0) ? artist.substr(start) : artist;
    };

    // Use config music_directories if set, otherwise fall back to Platform default
    if (!config_.music_directories.empty()) {
        for (const auto& dir : config_.music_directories) {
            util::Logger::info("Music directory: " + dir.string());
        }
        library.set_music_directories(config_.music_directories);
    } else {
        auto default_dir = util::Platform::get_music_directory();
        util::Logger::info("Music directory (default): " + default_dir.string());
        library.set_music_directory(default_dir);
    }

    // Publish early scanning state so UI shows loading indicator during validation
    publisher_->update([](model::Snapshot& snap) {
        auto loading_state = std::make_shared<model::LibraryState>();
        loading_state->is_scanning = true;
        loading_state->scanned_count = 0;
        loading_state->total_count = 0;
        snap.library = loading_state;
    });

    // Cache file path
    std::filesystem::path cache_dir;
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    if (xdg_cache) {
        cache_dir = std::filesystem::path(xdg_cache) / "ouroboros";
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            cache_dir = std::filesystem::path(home) / ".cache" / "ouroboros";
        } else {
            cache_dir = "/tmp/ouroboros_cache";
        }
    }
    std::filesystem::path cache_file = cache_dir / "library.bin";

    bool cache_valid = false;

    // ═══════════════════════════════════════════════════════════════════════
    // TIER 0: Load Monolithic Cache + Tree Hash Validation
    // ═══════════════════════════════════════════════════════════════════════
    backend::Library::CacheValidationResult tier0_result = backend::Library::CacheValidationResult::GenericFailure;

    if (std::filesystem::exists(cache_file)) {
        if (library.load_from_cache(cache_file)) {
            util::Logger::info("Cache loaded: " + std::to_string(library.get_track_count()) + " tracks");

            // Validate cache with TIER 0
            tier0_result = library.validate_cache_tier0(cache_file);

            if (tier0_result == backend::Library::CacheValidationResult::Valid) {
                util::Logger::info("TIER 0: Cache validated successfully - skipping scan");
                cache_valid = true;

                // Instant publish!
                auto new_lib_state = std::make_shared<model::LibraryState>();
                new_lib_state->tracks = library.get_all_tracks();

                // Sort library (parallel)
                util::Logger::info("Sorting library (parallel): " + std::to_string(new_lib_state->tracks.size()) + " tracks");
                ouroboros::util::parallel_timsort(new_lib_state->tracks, [&get_artist_sort_key](const model::Track& a, const model::Track& b) {
                    int cmp = util::case_insensitive_compare(get_artist_sort_key(a.artist), get_artist_sort_key(b.artist));
                    if (cmp != 0) return cmp < 0;
                    if (a.date != b.date) return a.date < b.date;
                    return a.track_number < b.track_number;
                });
                util::Logger::info("Library sorted successfully");

                new_lib_state->is_scanning = false;
                new_lib_state->scanned_count = library.get_track_count();
                new_lib_state->total_count = library.get_track_count();

                // Publish tracks immediately so Track view renders fast
                publisher_->update([&](model::Snapshot& s) {
                    s.library = new_lib_state;
                    s.timestamp = std::chrono::steady_clock::now();
                });

                // Compute album groups in background AFTER Track view is ready
                util::Logger::info("Computing album groups in background...");
                auto albums_state = std::make_shared<model::LibraryState>(*new_lib_state);
                compute_album_groups(*albums_state, config_);
                publisher_->update([&, albums_state](model::Snapshot& s) {
                    s.library = albums_state;
                    s.timestamp = std::chrono::steady_clock::now();
                });
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // TIER 1: Directory-Level Scan (O(dirs))
    // ═══════════════════════════════════════════════════════════════════════
    // Optimization: Only try TIER 1 if TIER 0 failed gently (e.g. Generic/Missing).
    // If TIER 0 said CountMismatch, we KNOW files were added/removed, so we must scan.
    bool skip_tier1 = (tier0_result == backend::Library::CacheValidationResult::CountMismatch);

    if (!cache_valid && !skip_tier1 && std::filesystem::exists(cache_file) && library.get_track_count() > 0) {
        util::Logger::info("TIER 0 failed - trying TIER 1 directory scan");

        // Scan all configured directories and merge mtimes
        std::unordered_map<std::string, std::time_t> current_dir_mtimes;
        if (!config_.music_directories.empty()) {
            for (const auto& dir : config_.music_directories) {
                auto dir_mtimes = util::DirectoryScanner::scan_directories_only(dir);
                current_dir_mtimes.insert(dir_mtimes.begin(), dir_mtimes.end());
            }
        } else {
            auto default_dir = util::Platform::get_music_directory();
            current_dir_mtimes = util::DirectoryScanner::scan_directories_only(default_dir);
        }
        auto dirty_dirs = library.find_dirty_directories(current_dir_mtimes, library.get_dir_mtimes());

        if (dirty_dirs.empty()) {
            util::Logger::info("TIER 1: No dirty directories - using cache");
            cache_valid = true;

            // Publish cached library
            auto new_lib_state = std::make_shared<model::LibraryState>();
            new_lib_state->tracks = library.get_all_tracks();

            util::Logger::info("Sorting library (parallel): " + std::to_string(new_lib_state->tracks.size()) + " tracks");
            ouroboros::util::parallel_timsort(new_lib_state->tracks, [&get_artist_sort_key](const model::Track& a, const model::Track& b) {
                int cmp = util::case_insensitive_compare(get_artist_sort_key(a.artist), get_artist_sort_key(b.artist));
                if (cmp != 0) return cmp < 0;
                if (a.date != b.date) return a.date < b.date;
                return a.track_number < b.track_number;
            });
            util::Logger::info("Library sorted successfully");

            new_lib_state->is_scanning = false;
            new_lib_state->scanned_count = library.get_track_count();
            new_lib_state->total_count = library.get_track_count();

            // Publish tracks immediately so Track view renders fast
            publisher_->update([&](model::Snapshot& s) {
                s.library = new_lib_state;
                s.timestamp = std::chrono::steady_clock::now();
            });

            // Compute album groups in background AFTER Track view is ready
            util::Logger::info("Computing album groups in background...");
            auto albums_state = std::make_shared<model::LibraryState>(*new_lib_state);
            compute_album_groups(*albums_state, config_);
            publisher_->update([&, albums_state](model::Snapshot& s) {
                s.library = albums_state;
                s.timestamp = std::chrono::steady_clock::now();
            });
        }
    } else if (skip_tier1) {
        util::Logger::info("Skipping TIER 1 because TIER 0 detected Count Mismatch (files added/removed)");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // TIER 2 + TIER 3: Full Scan with getdents64 + Parallel Parsing
    // ═══════════════════════════════════════════════════════════════════════
    if (!cache_valid) {
        util::Logger::info("Cache invalid - performing full scan with optimizations");

        // Publish scanning state
        publisher_->update([](model::Snapshot& snap) {
            auto scanning_state = std::make_shared<model::LibraryState>();
            if (snap.library) {
                scanning_state->tracks = snap.library->tracks;
            }
            scanning_state->is_scanning = true;
            scanning_state->scanned_count = 0;
            scanning_state->total_count = 0;
            snap.library = scanning_state;
        });

        // Scan with progress callback (uses getdents64 + parallel parsing)
        library.scan_directory([this](int scanned, int total) {
            publisher_->update([scanned, total](model::Snapshot& snap) {
                if (snap.library) {
                    auto progress_state = std::make_shared<model::LibraryState>(*snap.library);
                    progress_state->scanned_count = scanned;
                    progress_state->total_count = total;
                    snap.library = progress_state;
                }
            });
        });

        // Save monolithic cache
        library.save_to_cache(cache_file);

        // Publish final library
        auto new_lib_state = std::make_shared<model::LibraryState>();
        new_lib_state->tracks = library.get_all_tracks();

        util::Logger::info("Sorting scanned library (parallel): " + std::to_string(new_lib_state->tracks.size()) + " tracks");
        ouroboros::util::parallel_timsort(new_lib_state->tracks, [&get_artist_sort_key](const model::Track& a, const model::Track& b) {
            int cmp = util::case_insensitive_compare(get_artist_sort_key(a.artist), get_artist_sort_key(b.artist));
            if (cmp != 0) return cmp < 0;
            if (a.date != b.date) return a.date < b.date;
            return a.track_number < b.track_number;
        });
        util::Logger::info("Library sorted successfully");

        new_lib_state->is_scanning = false;
        new_lib_state->scanned_count = library.get_track_count();
        new_lib_state->total_count = library.get_track_count();

        // Publish tracks immediately so Track view renders fast
        publisher_->update([&](model::Snapshot& s) {
            s.library = new_lib_state;
            s.timestamp = std::chrono::steady_clock::now();
        });

        // Compute album groups in background AFTER Track view is ready
        util::Logger::info("Computing album groups in background...");
        auto albums_state = std::make_shared<model::LibraryState>(*new_lib_state);
        compute_album_groups(*albums_state, config_);
        publisher_->update([&, albums_state](model::Snapshot& s) {
            s.library = albums_state;
            s.timestamp = std::chrono::steady_clock::now();
        });
    }

    util::Logger::info("Library scan complete: " + std::to_string(library.get_track_count()) + " tracks");

    // Library only scans once at startup, so just sleep until shutdown
    while (!stop_token.stop_requested()) {
        // Sleep in short intervals to allow immediate shutdown
        for (int i = 0; i < 60; ++i) {
            if (stop_token.stop_requested()) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

}  // namespace ouroboros::collectors

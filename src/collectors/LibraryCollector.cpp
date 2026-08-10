#include "collectors/LibraryCollector.hpp"
#include "backend/Library.hpp"
#include "backend/Config.hpp"
#include "util/Platform.hpp"
#include "util/Logger.hpp"
#include "util/DirectoryScanner.hpp"
#include "util/UnicodeUtils.hpp"
#include "util/SortDispatch.hpp"
#include "util/SortKeys.hpp"
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <cstdint>

#include "sublimation.h"          // sublimation_parallel_for: the shared work-stealing pool
#include "sublimation_order.hpp"

namespace ouroboros::collectors {

namespace {

// Sort tracks by (artist, date, [directory], track number) through sublimation's
// byte-order string sort, using one precomputed composite key per track (see
// util/SortKeys.hpp). fold_case() makes the artist component sort exactly as
// case_insensitive_compare would, so the order matches the previous comparator.
// Index permutation -- no Track is copied until the final gather. group_by_dir
// adds the directory tiebreak (only the cache-hit path used it historically; the
// flag preserves each call site's exact order).
void sort_tracks_by_artist(std::vector<model::Track>& tracks,
                           const std::function<std::string(const std::string&)>& artist_key,
                           bool group_by_dir) {
    const size_t n = tracks.size();
    if (n < 2) return;

    std::vector<std::string> keys(n);
    std::vector<const char*> ptrs(n);
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& t = tracks[i];
        size_t slash = t.path.rfind('/');
        std::string dir = (slash != std::string::npos) ? t.path.substr(0, slash) : std::string();
        keys[i] = util::track_sort_key(util::fold_case(artist_key(t.artist)),
                                       t.date, dir, t.track_number, group_by_dir);
        ptrs[i] = keys[i].c_str();
        idx[i] = static_cast<uint32_t>(i);
    }
    util::sort_by_string(ptrs, idx);

    std::vector<model::Track> sorted;
    sorted.reserve(n);
    for (uint32_t k : idx) sorted.push_back(std::move(tracks[k]));
    tracks = std::move(sorted);
}

// Sort album groups by (scattered ? title : artist), then optional year, then
// title -- the previous album comparator, as a composite byte-order key.
void sort_albums(std::vector<model::AlbumGroup>& albums, bool sort_by_year,
                 const std::function<std::string(const std::string&)>& artist_key,
                 const std::function<int(const std::string&)>& year_to_int) {
    const size_t n = albums.size();
    if (n < 2) return;

    std::vector<std::string> keys(n);
    std::vector<const char*> ptrs(n);
    std::vector<uint32_t> idx(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& g = albums[i];
        std::string primary = g.is_scattered ? util::fold_case(g.normalized_title)
                                              : util::fold_case(artist_key(g.artist));
        std::string year_field = sort_by_year ? util::zeropad(year_to_int(g.year), 5) : std::string();
        keys[i] = util::album_sort_key(primary, year_field, util::fold_case(g.normalized_title));
        ptrs[i] = keys[i].c_str();
        idx[i] = static_cast<uint32_t>(i);
    }
    util::sort_by_string(ptrs, idx);

    std::vector<model::AlbumGroup> sorted;
    sorted.reserve(n);
    for (uint32_t k : idx) sorted.push_back(std::move(albums[k]));
    albums = std::move(sorted);
}

}  // namespace

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

    // STEP 1: Group tracks by DIRECTORY (not artist+album)
    // Directory is truth. All tracks in same folder = same album.
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

    // STEP 2: Sort tracks + detect scattered albums IN PARALLEL
    // Each album is independent - perfect for parallel processing
    // Runs on sublimation's Chase-Lev work-stealing engine, the same pool that
    // backs the parallel sort and radix -- one work-stealing engine in the
    // process rather than a second hand-rolled one. sublimation_default_workers
    // is cpuset-aware, so a taskset-confined run gets the cores it actually has
    // instead of hardware_concurrency's whole-machine count.
    const size_t num_albums = albums.size();
    const size_t num_threads = sublimation_default_workers();

    util::Logger::info("Processing " + std::to_string(num_albums) + " albums with " +
                      std::to_string(num_threads) + " threads");

    struct AlbumWork {
        std::vector<model::AlbumGroup>* albums;
        model::LibraryState* lib_state;
    };
    AlbumWork work{&albums, &lib_state};

    auto process_album = [](size_t idx, void* user) {
        auto* w = static_cast<AlbumWork*>(user);
        auto& album = (*w->albums)[idx];
        const auto& tracks = w->lib_state->tracks;

        // Sort tracks within this album by track number. Tiny N (a handful
        // to a few dozen per album). The pack sort tiebreaks on the packed
        // index, which is initialized from input order, so equal track
        // numbers keep their prior order without a separate stable pass.
        sublimation_order_u64(album.track_indices, false,
            [&tracks](int i) { return tracks[i].track_number; });

        // Detect if this album is scattered (compilation)
        album.is_scattered = detect_scattered(album, tracks);
    };

    // sublimation_scan owns the OOM fallback that used to be hand-written here,
    // copied from the CLI's call site. Album grouping is CPU-bound rather than
    // byte-weighted, so the byte gate is bypassed with a weight that always
    // clears it -- the fan-out decision is this caller's, the fallback is not.
    sublimation_scan(num_albums, sublimation_scan_min_bytes(),
                     process_album, &work, 0, nullptr);

    size_t scattered_count = std::count_if(albums.begin(), albums.end(),
        [](const model::AlbumGroup& a) { return a.is_scattered; });
    util::Logger::info("Detected " + std::to_string(scattered_count) + " scattered (compilation) albums");

    // STEP 3: Sort albums (parallel)
    // Scattered: by title | Unified: by artist, then year, then title
    bool sort_by_year = config.sort_albums_by_year;

    util::Logger::info("Sorting " + std::to_string(albums.size()) + " albums (sublimation)");
    sort_albums(albums, sort_by_year, get_artist_sort_key, year_to_int);

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
        library.set_music_directories({default_dir});
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

    // TIER 0: Load Monolithic Cache + Tree Hash Validation
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
                util::Logger::info("Sorting library (sublimation): " + std::to_string(new_lib_state->tracks.size()) + " tracks");
                sort_tracks_by_artist(new_lib_state->tracks, get_artist_sort_key, /*group_by_dir=*/true);
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

    // TIER 1: Directory-Level Scan (O(dirs))
    // Optimization: Only try TIER 1 if TIER 0 failed gently (e.g. Generic/Missing).
    // CountMismatch (files added/removed) and MetadataMismatch (in-place edit or
    // removal in a scanned dir) both skip TIER 1 and go straight to the incremental
    // reparse -- TIER 1 keys on directory mtime, which an in-place edit does not change,
    // so routing an edit through TIER 1 would wrongly clear it.
    bool skip_tier1 = (tier0_result == backend::Library::CacheValidationResult::CountMismatch ||
                       tier0_result == backend::Library::CacheValidationResult::MetadataMismatch);

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
            sort_tracks_by_artist(new_lib_state->tracks, get_artist_sort_key, /*group_by_dir=*/false);
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

    // TIER 2 + TIER 3: Full Scan with getdents64 + Parallel Parsing
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
        if (!library.save_to_cache(cache_file)) {
            util::Logger::error("Failed to save library cache: " + cache_file.string());
        }

        // Publish final library
        auto new_lib_state = std::make_shared<model::LibraryState>();
        new_lib_state->tracks = library.get_all_tracks();

        util::Logger::info("Sorting scanned library (parallel): " + std::to_string(new_lib_state->tracks.size()) + " tracks");
        sort_tracks_by_artist(new_lib_state->tracks, get_artist_sort_key, /*group_by_dir=*/false);
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

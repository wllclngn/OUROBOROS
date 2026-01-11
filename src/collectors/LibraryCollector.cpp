#include "collectors/LibraryCollector.hpp"
#include "backend/Library.hpp"
#include "util/Platform.hpp"
#include "util/Logger.hpp"
#include "util/TimSort.hpp"
#include "util/DirectoryScanner.hpp"
#include "util/UnicodeUtils.hpp"
#include <thread>
#include <fstream>

namespace ouroboros::collectors {

LibraryCollector::LibraryCollector(std::shared_ptr<backend::SnapshotPublisher> publisher,
                                   const backend::Config& config)
    : publisher_(publisher), config_(config) {}

void LibraryCollector::run(std::stop_token stop_token) {
    backend::Library library;

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

                // Sort library
                util::Logger::info("Sorting library: " + std::to_string(new_lib_state->tracks.size()) + " tracks");
                ouroboros::util::timsort(new_lib_state->tracks, [](const model::Track& a, const model::Track& b) {
                    int cmp = util::case_insensitive_compare(a.artist, b.artist);
                    if (cmp != 0) return cmp < 0;
                    if (a.date != b.date) return a.date < b.date;
                    return a.track_number < b.track_number;
                });
                util::Logger::info("Library sorted successfully");

                new_lib_state->is_scanning = false;
                new_lib_state->scanned_count = library.get_track_count();
                new_lib_state->total_count = library.get_track_count();

                publisher_->update([&](model::Snapshot& s) {
                    s.library = new_lib_state;
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

            util::Logger::info("Sorting library: " + std::to_string(new_lib_state->tracks.size()) + " tracks");
            ouroboros::util::timsort(new_lib_state->tracks, [](const model::Track& a, const model::Track& b) {
                int cmp = util::case_insensitive_compare(a.artist, b.artist);
                if (cmp != 0) return cmp < 0;
                if (a.date != b.date) return a.date < b.date;
                return a.track_number < b.track_number;
            });
            util::Logger::info("Library sorted successfully");

            new_lib_state->is_scanning = false;
            new_lib_state->scanned_count = library.get_track_count();
            new_lib_state->total_count = library.get_track_count();

            publisher_->update([&](model::Snapshot& s) {
                s.library = new_lib_state;
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

        util::Logger::info("Sorting scanned library: " + std::to_string(new_lib_state->tracks.size()) + " tracks");
        ouroboros::util::timsort(new_lib_state->tracks, [](const model::Track& a, const model::Track& b) {
            int cmp = util::case_insensitive_compare(a.artist, b.artist);
            if (cmp != 0) return cmp < 0;
            if (a.date != b.date) return a.date < b.date;
            return a.track_number < b.track_number;
        });
        util::Logger::info("Library sorted successfully");

        new_lib_state->is_scanning = false;
        new_lib_state->scanned_count = library.get_track_count();
        new_lib_state->total_count = library.get_track_count();

        publisher_->update([&](model::Snapshot& s) {
            s.library = new_lib_state;
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

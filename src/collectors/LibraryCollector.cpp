#include "collectors/LibraryCollector.hpp"
#include "backend/Library.hpp"
#include "util/Platform.hpp"
#include "util/Logger.hpp"
#include "util/TimSort.hpp"
#include "util/DirectoryScanner.hpp"
#include <thread>
#include <fstream>

namespace ouroboros::collectors {

LibraryCollector::LibraryCollector(std::shared_ptr<backend::SnapshotPublisher> publisher,
                                   const backend::Config& config)
    : publisher_(publisher), config_(config) {}

void LibraryCollector::run(std::stop_token stop_token) {
    backend::Library library;

    // Use config music_directory if set, otherwise fall back to Platform default
    auto music_dir = !config_.music_directory.empty()
        ? config_.music_directory
        : util::Platform::get_music_directory();

    util::Logger::info("Music directory: " + music_dir.string());
    library.set_music_directory(music_dir);
    
    // 1. Try to load cache
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
    // TIER 0: Tree Hash Validation (O(1))
    // ═══════════════════════════════════════════════════════════════════════
    if (std::filesystem::exists(cache_file)) {
        if (library.load_from_cache(cache_file)) {
            util::Logger::info("Cache loaded: " + std::to_string(library.get_track_count()) + " tracks");

            // Validate cache with TIER 0 (quick check)
            if (library.validate_cache_tier0(cache_file)) {
                util::Logger::info("TIER 0: Cache validated successfully - skipping scan");
                cache_valid = true;

                // Instant publish!
                auto new_lib_state = std::make_shared<model::LibraryState>();
                new_lib_state->tracks = library.get_all_tracks();

                // Sort library
                util::Logger::info("Sorting library: " + std::to_string(new_lib_state->tracks.size()) + " tracks");
                ouroboros::util::timsort(new_lib_state->tracks, [](const model::Track& a, const model::Track& b) {
                    if (a.artist != b.artist) return a.artist < b.artist;
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
    if (!cache_valid && std::filesystem::exists(cache_file) && library.get_track_count() > 0) {
        util::Logger::info("TIER 0 failed - trying TIER 1 directory scan");

        auto current_dir_mtimes = util::DirectoryScanner::scan_directories_only(music_dir);
        auto dirty_dirs = library.find_dirty_directories(current_dir_mtimes, library.get_dir_mtimes());

        if (dirty_dirs.empty()) {
            util::Logger::info("TIER 1: No dirty directories - using cache");
            cache_valid = true;

            // Publish cached library
            auto new_lib_state = std::make_shared<model::LibraryState>();
            new_lib_state->tracks = library.get_all_tracks();

            util::Logger::info("Sorting library: " + std::to_string(new_lib_state->tracks.size()) + " tracks");
            ouroboros::util::timsort(new_lib_state->tracks, [](const model::Track& a, const model::Track& b) {
                if (a.artist != b.artist) return a.artist < b.artist;
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

        // Save cache
        library.save_to_cache(cache_file);

        // Publish final library
        auto new_lib_state = std::make_shared<model::LibraryState>();
        new_lib_state->tracks = library.get_all_tracks();

        util::Logger::info("Sorting scanned library: " + std::to_string(new_lib_state->tracks.size()) + " tracks");
        ouroboros::util::timsort(new_lib_state->tracks, [](const model::Track& a, const model::Track& b) {
            if (a.artist != b.artist) return a.artist < b.artist;
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

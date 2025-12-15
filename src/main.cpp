#include "backend/SnapshotPublisher.hpp"
#include "backend/PipeWireClient.hpp"
#include "backend/Library.hpp"
#include "backend/Config.hpp"
#include "backend/ArtworkCache.hpp"
#include "collectors/LibraryCollector.hpp"
#include "collectors/PlaybackCollector.hpp"
#include "ui/Terminal.hpp"
#include "ui/Renderer.hpp"
#include "ui/ArtworkLoader.hpp"
#include "ui/ImageRenderer.hpp"
#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <poll.h>
#include <filesystem>

using namespace std::chrono_literals;

// Global shutdown flag (montauk pattern)
static std::atomic<bool> g_shutdown{false};

// Signal handler for graceful shutdown (Ctrl+C, kill, etc.)
static void signal_handler(int signum) {
    // Set shutdown flag
    g_shutdown.store(true);

    // CRITICAL: Restore terminal immediately on signal
    auto& terminal = ouroboros::ui::Terminal::instance();
    if (terminal.is_initialized()) {
        terminal.shutdown();
    }

    // Exit cleanly
    std::exit(signum == SIGINT ? 0 : signum);
}

int main() {
    try {
        // Initialize logger
        ouroboros::util::Logger::init();
        ouroboros::util::Logger::info("OUROBOROS starting...");

        // Load configuration
        auto config = ouroboros::backend::ConfigLoader::load_config();
        ouroboros::util::Logger::info("Configuration loaded");

        // Phase 3: Load persistent artwork cache from disk
        namespace fs = std::filesystem;
        fs::path cache_dir = fs::path(std::getenv("HOME")) / ".cache" / "ouroboros";
        fs::path cache_file = cache_dir / "artwork.cache";

        // Create cache directory if it doesn't exist
        if (!fs::exists(cache_dir)) {
            fs::create_directories(cache_dir);
            ouroboros::util::Logger::info("Created cache directory: " + cache_dir.string());
        }

        // Load cache if it exists
        auto& artwork_cache = ouroboros::backend::ArtworkCache::instance();
        if (fs::exists(cache_file)) {
            if (artwork_cache.load(cache_file)) {
                ouroboros::util::Logger::info("Loaded artwork cache: " + std::to_string(artwork_cache.size()) +
                    " entries, " + std::to_string(artwork_cache.memory_usage() / 1024) + " KB");
            } else {
                ouroboros::util::Logger::warn("Failed to load artwork cache from: " + cache_file.string());
            }
        } else {
            ouroboros::util::Logger::info("No existing artwork cache found, starting fresh");
        }

        // Initialize terminal
        auto& terminal = ouroboros::ui::Terminal::instance();
        terminal.init();

        // Install signal handlers for graceful shutdown (AFTER terminal init!)
        std::signal(SIGINT, signal_handler);   // Ctrl+C
        std::signal(SIGTERM, signal_handler);  // kill command

        // Create snapshot publisher (lock-free, thread-safe, double-buffered)
        auto publisher = std::make_shared<ouroboros::backend::SnapshotPublisher>();

        // Create collectors in separate threads
        ouroboros::collectors::LibraryCollector lib_collector(publisher);
        ouroboros::collectors::PlaybackCollector pb_collector(publisher);

        // Launch threads with BOTH stop_token AND global flag check
        std::jthread lib_thread([&lib_collector](std::stop_token st) {
            while (!st.stop_requested() && !g_shutdown.load()) {
                lib_collector.run(st);
                if (g_shutdown.load()) break;
            }
        });
        std::jthread pb_thread([&pb_collector](std::stop_token st) {
            while (!st.stop_requested() && !g_shutdown.load()) {
                pb_collector.run(st);
                if (g_shutdown.load()) break;
            }
        });

        // NO Queue Sync Thread! Queue is managed directly in Snapshot.
        // NO MetadataCollector Thread! Metadata parsed during library scan.

        ouroboros::util::Logger::info("Collectors started");

        // Start UI immediately - Library will populate asynchronously
        ouroboros::util::Logger::info("Collectors started, initializing UI...");

        // Setup EventBus handlers
        auto& event_bus = ouroboros::events::EventBus::instance();
        
        // ========== QUEUE MANAGEMENT ==========
        
        // Add track to queue
        event_bus.subscribe(ouroboros::events::Event::Type::AddTrackToQueue,
            [publisher](const ouroboros::events::Event& evt) {
                publisher->update([evt](ouroboros::model::Snapshot& snap) {
                    // Defensive: Check library exists
                    if (!snap.library) {
                        ouroboros::util::Logger::error("AddTrackToQueue: Library is null!");
                        return;
                    }

                    // Defensive: Bounds check with detailed logging
                    if (evt.index < 0 || evt.index >= static_cast<int>(snap.library->tracks.size())) {
                        ouroboros::util::Logger::error("AddTrackToQueue: Index out of bounds! index=" +
                            std::to_string(evt.index) + ", library size=" +
                            std::to_string(snap.library->tracks.size()));
                        return;
                    }

                    const auto& track = snap.library->tracks[evt.index];
                    ouroboros::util::Logger::debug("AddTrackToQueue: Adding index " + std::to_string(evt.index) +
                        " (title: " + track.title + ")");

                    // Defensive: Check queue exists
                    if (!snap.queue) {
                        ouroboros::util::Logger::error("AddTrackToQueue: Queue is null! Creating new queue.");
                        snap.queue = std::make_shared<ouroboros::model::QueueState>();
                    }

                    // Copy-On-Write Queue - store INDEX not full Track
                    auto new_queue = std::make_shared<ouroboros::model::QueueState>(*snap.queue);
                    new_queue->track_indices.push_back(evt.index);

                    ouroboros::util::Logger::debug("AddTrackToQueue: Queue size before: " +
                        std::to_string(snap.queue->track_indices.size()) +
                        ", after: " + std::to_string(new_queue->track_indices.size()));

                    // If queue was empty, start playing
                    if (new_queue->track_indices.size() == 1) {
                        new_queue->current_index = 0;
                        ouroboros::util::Logger::debug("AddTrackToQueue: Queue was empty, set current_index=0");
                    }

                    snap.queue = new_queue;
                    ouroboros::util::Logger::info("Added to queue: " + track.title +
                        " [Queue now has " + std::to_string(new_queue->track_indices.size()) +
                        " tracks, current_index=" + std::to_string(new_queue->current_index) + "]");
                });
            });
        
        // Next track
        event_bus.subscribe(ouroboros::events::Event::Type::NextTrack,
            [publisher](const ouroboros::events::Event&) {
                publisher->update([](ouroboros::model::Snapshot& snap) {
                    if (snap.queue->track_indices.empty()) {
                        ouroboros::util::Logger::warn("NextTrack: Queue is empty, cannot advance");
                        return;
                    }

                    auto new_queue = std::make_shared<ouroboros::model::QueueState>(*snap.queue);
                    size_t old_index = new_queue->current_index;

                    if (new_queue->current_index + 1 < new_queue->track_indices.size()) {
                        new_queue->current_index++;
                        snap.queue = new_queue;
                        ouroboros::util::Logger::info("NextTrack: Advanced from index " +
                            std::to_string(old_index) + " to " + std::to_string(new_queue->current_index) +
                            " (queue size: " + std::to_string(new_queue->track_indices.size()) + ")");
                    } else {
                        ouroboros::util::Logger::warn("NextTrack: Already at last track (index " +
                            std::to_string(new_queue->current_index) + " of " +
                            std::to_string(new_queue->track_indices.size()) + "), cannot advance");
                    }
                });
            });
        
        // Previous track
        event_bus.subscribe(ouroboros::events::Event::Type::PrevTrack,
            [publisher](const ouroboros::events::Event&) {
                 publisher->update([](ouroboros::model::Snapshot& snap) {
                    if (snap.queue->track_indices.empty()) {
                        ouroboros::util::Logger::warn("PrevTrack: Queue is empty, cannot go back");
                        return;
                    }

                    auto new_queue = std::make_shared<ouroboros::model::QueueState>(*snap.queue);
                    size_t old_index = new_queue->current_index;

                    if (new_queue->current_index > 0) {
                        new_queue->current_index--;
                        snap.queue = new_queue;
                        ouroboros::util::Logger::info("PrevTrack: Went back from index " +
                            std::to_string(old_index) + " to " + std::to_string(new_queue->current_index) +
                            " (queue size: " + std::to_string(new_queue->track_indices.size()) + ")");
                    } else {
                        ouroboros::util::Logger::warn("PrevTrack: Already at first track (index 0), cannot go back");
                    }
                });
            });

        // Clear queue
        event_bus.subscribe(ouroboros::events::Event::Type::ClearQueue,
            [publisher](const ouroboros::events::Event&) {
                publisher->update([](ouroboros::model::Snapshot& snap) {
                    auto new_queue = std::make_shared<ouroboros::model::QueueState>();
                    new_queue->track_indices.clear();
                    new_queue->current_index = 0;
                    snap.queue = new_queue;
                    ouroboros::util::Logger::info("ClearQueue: Queue cleared");
                });
            });
        
        // ========== PLAYBACK CONTROLS ==========
        
        // PlayPause is handled by PlaybackCollector
        
        // Seek forward
        event_bus.subscribe(ouroboros::events::Event::Type::SeekForward,
            [publisher](const ouroboros::events::Event& evt) {
                publisher->update([evt](ouroboros::model::Snapshot& snap) {
                    if (!snap.player.current_track_index.has_value()) return;

                    // Resolve track via Library
                    int track_idx = snap.player.current_track_index.value();
                    if (track_idx < 0 || track_idx >= static_cast<int>(snap.library->tracks.size())) return;
                    const auto& track = snap.library->tracks[track_idx];

                    int current_pos = snap.player.playback_position_ms;
                    int target = current_pos + (evt.seek_seconds * 1000);
                    int duration = track.duration_ms;

                    if (duration > 0 && target >= duration) {
                        // Skip to next track
                        // Note: We can't easily emit an event from inside update()
                        // So we just simulate next track logic here or rely on playback collector
                        // For now, just clamp
                        snap.player.seek_request_ms = duration; 
                    } else {
                        snap.player.seek_request_ms = target;
                    }
                });
            });
        
        // Seek backward
        event_bus.subscribe(ouroboros::events::Event::Type::SeekBackward,
            [publisher](const ouroboros::events::Event& evt) {
                publisher->update([evt](ouroboros::model::Snapshot& snap) {
                    int current_pos = snap.player.playback_position_ms;
                    snap.player.seek_request_ms = std::max(0, current_pos - (evt.seek_seconds * 1000));
                });
            });
        
        // Volume up
        event_bus.subscribe(ouroboros::events::Event::Type::VolumeUp,
            [publisher](const ouroboros::events::Event& evt) {
                publisher->update([evt](ouroboros::model::Snapshot& snap) {
                    snap.player.volume_percent = std::min(100, snap.player.volume_percent + evt.volume_delta);
                });
            });
        
        // Volume down
        event_bus.subscribe(ouroboros::events::Event::Type::VolumeDown,
            [publisher](const ouroboros::events::Event& evt) {
                publisher->update([evt](ouroboros::model::Snapshot& snap) {
                    snap.player.volume_percent = std::max(0, snap.player.volume_percent - evt.volume_delta);
                });
            });
            
        // Repeat Toggle
        event_bus.subscribe(ouroboros::events::Event::Type::RepeatToggle,
            [publisher](const ouroboros::events::Event&) {
                 publisher->update([](ouroboros::model::Snapshot& snap) {
                    switch (snap.player.repeat_mode) {
                        case ouroboros::model::RepeatMode::Off:
                            snap.player.repeat_mode = ouroboros::model::RepeatMode::One;
                            break;
                        case ouroboros::model::RepeatMode::One:
                            snap.player.repeat_mode = ouroboros::model::RepeatMode::All;
                            break;
                        case ouroboros::model::RepeatMode::All:
                            snap.player.repeat_mode = ouroboros::model::RepeatMode::Off;
                            break;
                    }
                 });
            });
        
        ouroboros::util::Logger::info("EventBus configured");

        // Create renderer (widgets are created internally)
        ouroboros::ui::Renderer renderer(publisher);

        ouroboros::util::Logger::info("Renderer initialized");

        // Force initial render so UI appears immediately
        renderer.render();

        // Main Loop with POLL
        // Start with seq=0 so any initial snapshot will trigger a render
        uint64_t last_rendered_seq = 0;
        std::optional<int> last_track_index;  // Track when current track changes

        while (!renderer.should_quit() && !g_shutdown.load()) {
            // LOGGING DISABLED: Main loop runs 30fps, creates I/O storm
            // ouroboros::util::Logger::debug("Main loop iteration start");
            bool needs_render = false;

            // Check for updates
            // ouroboros::util::Logger::debug("Main loop: Calling get_current()...");
            auto snap = publisher->get_current();
            // ouroboros::util::Logger::debug("Main loop: get_current() returned, seq=" +
            //     (snap ? std::to_string(snap->seq) : "null"));

            if (snap && snap->seq != last_rendered_seq) {
                needs_render = true;
                last_rendered_seq = snap->seq;
                // ouroboros::util::Logger::debug("Main loop: Snapshot changed, will render");
            }

            // Check if artwork finished loading
            auto& artwork_loader = ouroboros::ui::ArtworkLoader::instance();
            bool artwork_updated = false;
            
            if (artwork_loader.has_pending_updates()) {
                needs_render = true;
                artwork_updated = true;
                artwork_loader.clear_pending_updates();
            }

            // Check if image decoding finished (async jobs in ImageRenderer)
            auto& image_renderer = ouroboros::ui::ImageRenderer::instance();
            if (image_renderer.has_pending_updates()) {
                needs_render = true;
                artwork_updated = true;
                image_renderer.clear_pending_updates();
            }

            // Force continuous rendering while library is scanning (for loading animation)
            if (snap && snap->library && snap->library->is_scanning) {
                needs_render = true;
            }

            // Update artwork cache context ONLY when current track changes
            if (snap && snap->player.current_track_index != last_track_index) {
                ouroboros::util::Logger::debug("Track changed detected!");
                last_track_index = snap->player.current_track_index;

                if (snap->player.current_track_index.has_value()) {
                    int current_idx = snap->player.current_track_index.value();
                    if (current_idx >= 0 && current_idx < static_cast<int>(snap->library->tracks.size())) {
                        std::string current_path = snap->library->tracks[current_idx].path;

                        // Get next 5 tracks from queue
                        std::vector<std::string> next_paths;
                        if (snap->queue && snap->queue->current_index < snap->queue->track_indices.size()) {
                            for (size_t i = 1; i <= 5 && snap->queue->current_index + i < snap->queue->track_indices.size(); ++i) {
                                int idx = snap->queue->track_indices[snap->queue->current_index + i];
                                if (idx >= 0 && idx < static_cast<int>(snap->library->tracks.size())) {
                                    next_paths.push_back(snap->library->tracks[idx].path);
                                }
                            }
                        }

                        ouroboros::util::Logger::debug("Updating artwork cache context...");
                        artwork_loader.update_queue_context(current_path, next_paths);
                        ouroboros::util::Logger::debug("Artwork cache context updated");
                    }
                }
            }

            if (needs_render) {
                try {
                    ouroboros::util::Logger::debug("Main: Starting render...");
                    renderer.render(artwork_updated);
                    ouroboros::util::Logger::debug("Main: Render complete");
                } catch (const std::exception& e) {
                    ouroboros::util::Logger::error("Main: EXCEPTION during render: " + std::string(e.what()));
                    throw;  // Re-throw to crash with backtrace
                } catch (...) {
                    ouroboros::util::Logger::error("Main: UNKNOWN EXCEPTION during render!");
                    throw;
                }
            }

            // Poll for input (latency killer)
            // ouroboros::util::Logger::debug("Polling for input...");
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            int ret = poll(&pfd, 1, 33); // 33ms timeout (target ~30fps for UI updates)

            if (ret < 0) {
                if (errno == EINTR) {
                    ouroboros::util::Logger::debug("Poll interrupted by signal (EINTR), continuing");
                    continue; // Signal interrupted, just retry
                } else {
                    ouroboros::util::Logger::error("Poll failed: errno=" + std::to_string(errno));
                    break; // Fatal error
                }
            }

            // ouroboros::util::Logger::debug("Poll returned: " + std::to_string(ret));

            if (ret > 0 && (pfd.revents & POLLIN)) {
                try {
                    ouroboros::util::Logger::debug("Main: Handling input...");
                    renderer.handle_input();
                    ouroboros::util::Logger::debug("Main: Input handled, rendering...");
                    renderer.render(); // Update UI immediately after input
                    ouroboros::util::Logger::debug("Main: Post-input render complete");
                } catch (const std::exception& e) {
                    ouroboros::util::Logger::error("Main: EXCEPTION during input/render: " + std::string(e.what()));
                    throw;
                } catch (...) {
                    ouroboros::util::Logger::error("Main: UNKNOWN EXCEPTION during input/render!");
                    throw;
                }
            }
        }

        // IMMEDIATE TERMINAL RESTORATION
        // Ensure the user gets their terminal back instantly
        terminal.shutdown();
        ouroboros::util::Logger::info("Terminal restored, performing background cleanup...");

        // Phase 3: Save artwork cache to disk before shutdown
        ouroboros::util::Logger::info("Saving artwork cache...");
        if (artwork_cache.save(cache_file)) {
            ouroboros::util::Logger::info("Artwork cache saved: " + std::to_string(artwork_cache.size()) +
                " entries, " + std::to_string(artwork_cache.memory_usage() / 1024) + " KB");
        } else {
            ouroboros::util::Logger::error("Failed to save artwork cache to: " + cache_file.string());
        }

        ouroboros::util::Logger::info("OUROBOROS shutdown");

        return 0;
    } catch (const std::exception& e) {
        // CRITICAL: Restore terminal even on exception!
        auto& terminal = ouroboros::ui::Terminal::instance();
        terminal.shutdown();
        ouroboros::util::Logger::error("Fatal error: " + std::string(e.what()));
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}

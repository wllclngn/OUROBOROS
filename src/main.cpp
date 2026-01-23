#include "backend/SnapshotPublisher.hpp"
#include "backend/PipeWireClient.hpp"
#include "backend/Library.hpp"
#include "backend/Config.hpp"
#include "backend/ArtworkCache.hpp"
#include "collectors/LibraryCollector.hpp"
#include "collectors/PlaybackCollector.hpp"
#include "ui/Terminal.hpp"
#include "ui/Renderer.hpp"
#include "ui/ArtworkWindow.hpp"
#include "ui/ImageRenderer.hpp"
#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <clocale>
#include <unistd.h>
#include <poll.h>
#include <filesystem>
#include <sys/random.h>
#include <optional>
#include <vector>

using namespace std::chrono_literals;

// Global shutdown flag (montauk pattern)
static std::atomic<bool> g_shutdown{false};

// Async-signal-safe terminal restoration (atexit safety net)
static void on_atexit_restore() {
    // Minimal restore using only async-signal-safe write() syscall
    const char* alt_screen_off = "\033[?1049l";
    const char* show_cursor = "\033[?25h";
    const char* reset_sgr = "\033[0m";
    write(STDOUT_FILENO, alt_screen_off, 8);
    write(STDOUT_FILENO, show_cursor, 6);
    write(STDOUT_FILENO, reset_sgr, 4);
}

// Signal handler for graceful shutdown (Ctrl+C, kill, etc.)
// NOTE: Does NOT call std::exit() - lets main loop exit normally for proper cleanup
static void signal_handler(int) {
    g_shutdown.store(true);

    // Async-signal-safe: just restore cursor visibility
    // Full cleanup happens when main loop exits normally
    const char* show_cursor = "\033[?25h";
    write(STDOUT_FILENO, show_cursor, 6);
}


int main() {
    // Set locale for proper Unicode handling (required for wcwidth)
    std::setlocale(LC_ALL, "");

    try {
        // CRITICAL: Redirect stderr to log file to capture libmpg123 warnings
        // libmpg123 writes directly to stderr, bypassing our logger
        // This keeps the terminal clean while preserving debug info in the log
        freopen("/tmp/ouroboros_debug.log", "a", stderr);

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

        // Register atexit handler as safety net (montauk pattern)
        std::atexit(on_atexit_restore);

        // Install signal handlers for graceful shutdown (AFTER terminal init!)
        std::signal(SIGINT, signal_handler);   // Ctrl+C
        std::signal(SIGTERM, signal_handler);  // kill command

        // Create snapshot publisher (lock-free, thread-safe, double-buffered)
        auto publisher = std::make_shared<ouroboros::backend::SnapshotPublisher>();

        // Apply config values to initial snapshot (volume, shuffle, repeat)
        publisher->update([&config](ouroboros::model::Snapshot& snap) {
            snap.player.volume_percent = config.default_volume;
            snap.player.shuffle_enabled = config.shuffle;
            if (config.repeat == "one") {
                snap.player.repeat_mode = ouroboros::model::RepeatMode::One;
            } else if (config.repeat == "all") {
                snap.player.repeat_mode = ouroboros::model::RepeatMode::All;
            } else {
                snap.player.repeat_mode = ouroboros::model::RepeatMode::Off;
            }
            ouroboros::util::Logger::info("Config: Applied startup settings - volume=" +
                std::to_string(snap.player.volume_percent) + ", shuffle=" +
                (snap.player.shuffle_enabled ? "true" : "false") + ", repeat=" + config.repeat);
        });

        // Create collectors in separate threads (pass config to LibraryCollector)
        ouroboros::collectors::LibraryCollector lib_collector(publisher, config);
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
        
        // Add track to queue (Two Stacks: push to future)
        event_bus.subscribe(ouroboros::events::Event::Type::AddTrackToQueue,
            [publisher](const ouroboros::events::Event& evt) {
                publisher->update([evt](ouroboros::model::Snapshot& snap) {
                    // Defensive: Check library exists
                    if (!snap.library) {
                        ouroboros::util::Logger::error("AddTrackToQueue: Library is null!");
                        return;
                    }

                    // Defensive: Bounds check
                    if (evt.index < 0 || evt.index >= static_cast<int>(snap.library->tracks.size())) {
                        ouroboros::util::Logger::error("AddTrackToQueue: Index out of bounds! index=" +
                            std::to_string(evt.index) + ", library size=" +
                            std::to_string(snap.library->tracks.size()));
                        return;
                    }

                    const auto& track = snap.library->tracks[evt.index];

                    // Defensive: Check queue exists
                    if (!snap.queue) {
                        ouroboros::util::Logger::error("AddTrackToQueue: Queue is null! Creating new queue.");
                        snap.queue = std::make_shared<ouroboros::model::QueueState>();
                    }

                    // Copy-On-Write: Two Stacks model
                    auto new_queue = std::make_shared<ouroboros::model::QueueState>(*snap.queue);

                    // Add to future stack (next track at back)
                    new_queue->future.push_back(evt.index);

                    // If nothing playing, start playback immediately
                    if (!new_queue->current.has_value()) {
                        new_queue->current = new_queue->future.back();
                        new_queue->future.pop_back();
                        snap.player.current_track_index = evt.index;
                        ouroboros::util::Logger::debug("AddTrackToQueue: Started playback with track " +
                            std::to_string(evt.index));
                    }

                    snap.queue = new_queue;
                    ouroboros::util::Logger::info("Added to queue: " + track.title +
                        " [Queue: " + std::to_string(new_queue->history.size()) + " played, " +
                        (new_queue->current.has_value() ? "1 current, " : "0 current, ") +
                        std::to_string(new_queue->future.size()) + " upcoming]");
                });
            });
        
        // Next track (Two Stacks: push current to history, pop from future)
        event_bus.subscribe(ouroboros::events::Event::Type::NextTrack,
            [publisher](const ouroboros::events::Event&) {
                publisher->update([](ouroboros::model::Snapshot& snap) {
                    auto new_queue = std::make_shared<ouroboros::model::QueueState>(*snap.queue);

                    // Push current to history
                    if (new_queue->current.has_value()) {
                        new_queue->history.push_back(*new_queue->current);
                    }

                    if (!new_queue->future.empty()) {
                        if (snap.player.shuffle_enabled) {
                            // Shuffle: CSPRNG pick random from future
                            uint64_t rand_val;
                            getrandom(&rand_val, sizeof(rand_val), 0);
                            size_t idx = rand_val % new_queue->future.size();
                            new_queue->current = new_queue->future[idx];
                            new_queue->future.erase(new_queue->future.begin() + idx);
                            ouroboros::util::Logger::info("NextTrack (shuffle): Random pick, " +
                                std::to_string(new_queue->future.size()) + " remaining in future");
                        } else {
                            // Linear: pop from FRONT of future (FIFO order)
                            new_queue->current = new_queue->future.front();
                            new_queue->future.erase(new_queue->future.begin());
                            ouroboros::util::Logger::info("NextTrack: Advanced to next, " +
                                std::to_string(new_queue->future.size()) + " remaining");
                        }
                    } else if (snap.player.repeat_mode == ouroboros::model::RepeatMode::All) {
                        // Repeat All: recycle history back to future (maintain add order)
                        new_queue->future = new_queue->history;
                        new_queue->history.clear();
                        if (!new_queue->future.empty()) {
                            if (snap.player.shuffle_enabled) {
                                uint64_t rand_val;
                                getrandom(&rand_val, sizeof(rand_val), 0);
                                size_t idx = rand_val % new_queue->future.size();
                                new_queue->current = new_queue->future[idx];
                                new_queue->future.erase(new_queue->future.begin() + idx);
                            } else {
                                new_queue->current = new_queue->future.front();
                                new_queue->future.erase(new_queue->future.begin());
                            }
                            ouroboros::util::Logger::info("NextTrack: Repeat All - recycled " +
                                std::to_string(new_queue->future.size() + 1) + " tracks");
                        }
                    } else {
                        // No more tracks, stop
                        new_queue->current = std::nullopt;
                        snap.player.state = ouroboros::model::PlaybackState::Stopped;
                        ouroboros::util::Logger::info("NextTrack: End of queue, stopped");
                    }

                    snap.queue = new_queue;
                });
            });
        
        // Previous track (Two Stacks: ALWAYS deterministic - pop from history)
        event_bus.subscribe(ouroboros::events::Event::Type::PrevTrack,
            [publisher](const ouroboros::events::Event&) {
                ouroboros::util::Logger::info("PrevTrack: Event received");
                publisher->update([](ouroboros::model::Snapshot& snap) {
                    auto new_queue = std::make_shared<ouroboros::model::QueueState>(*snap.queue);

                    ouroboros::util::Logger::info("PrevTrack: history=" +
                        std::to_string(new_queue->history.size()) + ", current=" +
                        (new_queue->current.has_value() ? std::to_string(*new_queue->current) : "none") +
                        ", future=" + std::to_string(new_queue->future.size()));

                    // Push current to FRONT of future (so it becomes "next" if we go forward again)
                    if (new_queue->current.has_value()) {
                        new_queue->future.insert(new_queue->future.begin(), *new_queue->current);
                    }

                    if (!new_queue->history.empty()) {
                        // Pop from history - ALWAYS deterministic, even in shuffle
                        new_queue->current = new_queue->history.back();
                        new_queue->history.pop_back();
                        ouroboros::util::Logger::info("PrevTrack: Back to previous track " +
                            std::to_string(*new_queue->current) + ", " +
                            std::to_string(new_queue->history.size()) + " remaining in history");
                    } else {
                        // At beginning - restore current from future
                        if (!new_queue->future.empty()) {
                            new_queue->current = new_queue->future.front();
                            new_queue->future.erase(new_queue->future.begin());
                        }
                        ouroboros::util::Logger::warn("PrevTrack: At beginning of history");
                    }

                    snap.queue = new_queue;
                });
            });

        // Clear queue (Two Stacks: reset all stacks)
        event_bus.subscribe(ouroboros::events::Event::Type::ClearQueue,
            [publisher](const ouroboros::events::Event&) {
                publisher->update([](ouroboros::model::Snapshot& snap) {
                    auto new_queue = std::make_shared<ouroboros::model::QueueState>();
                    // All vectors default empty, current defaults to nullopt
                    snap.queue = new_queue;
                    snap.player.state = ouroboros::model::PlaybackState::Stopped;
                    snap.player.current_track_index = std::nullopt;
                    ouroboros::util::Logger::info("ClearQueue: Queue cleared, playback stopped");
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

        // Shuffle Toggle (Two Stacks: history IS the play order, no reset needed)
        event_bus.subscribe(ouroboros::events::Event::Type::ShuffleToggle,
            [publisher](const ouroboros::events::Event&) {
                publisher->update([](ouroboros::model::Snapshot& snap) {
                    snap.player.shuffle_enabled = !snap.player.shuffle_enabled;
                    ouroboros::util::Logger::info("Shuffle: " +
                        std::string(snap.player.shuffle_enabled ? "ON" : "OFF"));
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

            // Check if artwork finished loading (unified ArtworkWindow)
            auto& artwork_window = ouroboros::ui::ArtworkWindow::instance();
            bool artwork_updated = false;

            if (artwork_window.has_updates()) {
                artwork_window.clear_updates();
                // Always trigger render when artwork updates - NowPlaying and AlbumBrowser both need it
                // Priority < 1000 means visible artwork (NowPlaying priority=0, visible albums < 1000)
                needs_render = true;
                artwork_updated = true;
            }

            // Force continuous rendering while library is scanning (for loading animation)
            if (snap && snap->library && snap->library->is_scanning) {
                needs_render = true;
            }

            // Track index change detection (for logging)
            if (snap && snap->player.current_track_index != last_track_index) {
                ouroboros::util::Logger::debug("Track changed detected!");
                last_track_index = snap->player.current_track_index;
                // Artwork loading is handled on-demand by NowPlaying via ArtworkWindow
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
                    ouroboros::util::Logger::debug("Main: Input available, draining queue...");

                    // DRAIN LOOP: Consume all buffered input events to prevent queue backlog
                    // This fixes the Ghostty terminal runaway key repeat issue
                    ouroboros::ui::InputEvent last_event;
                    int events_drained = 0;
                    auto& terminal = ouroboros::ui::Terminal::instance();

                    while (true) {
                        auto event = terminal.read_input();

                        // Empty event means queue is drained
                        if (event.key_name.empty() && event.key == 0) {
                            break;
                        }

                        // Keep only the last event
                        last_event = event;
                        events_drained++;

                        // Safety: warn if drain count is unusually high (possible bug in empty-event detection)
                        if (events_drained > 1000) {
                            ouroboros::util::Logger::warn("Main: Input drain exceeded 1000 events - possible terminal bug or stuck key");
                            break;
                        }
                    }

                    // Process only the LAST event from the drained queue
                    if (events_drained > 0) {
                        if (events_drained > 5) {
                            ouroboros::util::Logger::debug("Main: Drained " + std::to_string(events_drained) +
                                                          " buffered input events, processing last event");
                        }

                        ouroboros::util::Logger::debug("Main: Processing event key=" + std::to_string(last_event.key) +
                                                      " name=" + last_event.key_name);
                        renderer.handle_input_event(last_event);
                        ouroboros::util::Logger::debug("Main: Event processed, rendering...");
                        renderer.render(); // Update UI immediately after input
                        ouroboros::util::Logger::debug("Main: Post-input render complete");
                    }
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

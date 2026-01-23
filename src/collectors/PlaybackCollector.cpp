#include "collectors/PlaybackCollector.hpp"
#include "audio/MP3Decoder.hpp"
#include "audio/FLACDecoder.hpp"
#include "audio/OGGDecoder.hpp"
#include "audio/M4ADecoder.hpp"
#include "audio/PipeWireOutput.hpp"
#include "backend/MetadataParser.hpp"
#include "util/Logger.hpp"
#include "events/EventBus.hpp"
#include <thread>
#include <memory>
#include <filesystem>
#include <cmath>
#include <sys/random.h>

namespace ouroboros::collectors {

namespace {
    // Pick random unplayed index for shuffle mode using getrandom() directly
    std::optional<size_t> pick_shuffle_next(const model::QueueState& queue) {
        std::vector<size_t> candidates;
        for (size_t i = 0; i < queue.track_indices.size(); i++) {
            if (queue.played_indices.find(i) == queue.played_indices.end()) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) return std::nullopt;

        // Get random bytes directly from kernel CSPRNG
        uint64_t rand_val;
        getrandom(&rand_val, sizeof(rand_val), 0);

        // Convert to index in range [0, candidates.size())
        size_t idx = rand_val % candidates.size();
        return candidates[idx];
    }
}

PlaybackCollector::PlaybackCollector(std::shared_ptr<backend::SnapshotPublisher> publisher)
    : publisher_(publisher) {
    auto& bus = events::EventBus::instance();

    subscriptions_.push_back(bus.subscribe(events::Event::Type::PlayPause,
        [this](const events::Event&) {
            paused_ = !paused_;
        }));

    // Subscribe to ClearQueue for immediate stop
    subscriptions_.push_back(bus.subscribe(events::Event::Type::ClearQueue,
        [this](const events::Event&) {
            clear_requested_.store(true, std::memory_order_release);
            util::Logger::debug("PlaybackCollector: Clear requested (atomic flag set)");
        }));

    // Initialize global audio context
    if (!audio_context_.init()) {
        util::Logger::error("Failed to initialize PipeWire context!");
    }
}

PlaybackCollector::~PlaybackCollector() {
    auto& bus = events::EventBus::instance();
    for (auto id : subscriptions_) {
        bus.unsubscribe(id);
    }
}

void PlaybackCollector::run(std::stop_token stop_token) {
    bool last_queue_empty_logged = false;

    while (!stop_token.stop_requested()) {
        auto snap_ptr = publisher_->get_current();
        if (!snap_ptr) {
            util::Logger::debug("PlaybackCollector: No snapshot available");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Working with shared pointers now
        const auto& snap = *snap_ptr;

        if (snap.queue->track_indices.empty() || snap.queue->current_index >= snap.queue->track_indices.size()) {
            if (!last_queue_empty_logged) {
                util::Logger::debug("PlaybackCollector: Queue empty or index out of bounds (size=" +
                    std::to_string(snap.queue->track_indices.size()) + ", idx=" +
                    std::to_string(snap.queue->current_index) + ")");
                last_queue_empty_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Reset flag since we have a valid queue
        last_queue_empty_logged = false;

        // Get track index from queue, then resolve to actual Track via Library
        int track_index = snap.queue->track_indices[snap.queue->current_index];
        if (track_index < 0 || track_index >= static_cast<int>(snap.library->tracks.size())) {
            // DIAGNOSTIC: Enhanced logging for corruption detection
            util::Logger::error("PlaybackCollector: CORRUPTION DETECTED - Track index out of bounds (idx=" +
                std::to_string(track_index) + " (0x" +
                ([](int v) { char buf[16]; snprintf(buf, sizeof(buf), "%x", v); return std::string(buf); })(track_index) +
                "), lib_size=" + std::to_string(snap.library->tracks.size()) +
                ", queue_size=" + std::to_string(snap.queue->track_indices.size()) +
                ", current_index=" + std::to_string(snap.queue->current_index) + ")");

            // DIAGNOSTIC: Dump first 10 queue entries
            std::string queue_dump = "Queue contents: [";
            for (size_t j = 0; j < snap.queue->track_indices.size() && j < 10; ++j) {
                if (j > 0) queue_dump += ", ";
                queue_dump += std::to_string(snap.queue->track_indices[j]);
            }
            if (snap.queue->track_indices.size() > 10) queue_dump += ", ...";
            queue_dump += "]";
            util::Logger::error("PlaybackCollector: " + queue_dump);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const auto& track = snap.library->tracks[track_index];
        util::Logger::debug("PlaybackCollector: Starting track: " + track.title);

        // DIAGNOSTIC: Log track details for debugging empty-metadata tracks
        util::Logger::debug("PlaybackCollector: Track details - path=" + track.path +
            ", is_valid=" + std::string(track.is_valid ? "true" : "false") +
            ", format=" + std::to_string(static_cast<int>(track.format)) +
            ", error=" + track.error_message);

        size_t starting_index = snap.queue->current_index;

        // VALIDATION: Check if track is valid before attempting playback
        if (!track.is_valid) {
            util::Logger::warn("PlaybackCollector: SKIPPING invalid track - path=" + track.path +
                ", error=" + track.error_message);
            publisher_->update([&](model::Snapshot& s) {
                s.alerts.push_back({
                    "error",
                    "Cannot play: " + track.title + " - " + track.error_message,
                    std::chrono::steady_clock::now()
                });

                // Advance queue (COW) - shuffle-aware
                auto new_queue = std::make_shared<model::QueueState>(*s.queue);
                if (s.player.shuffle_enabled) {
                    new_queue->played_indices.insert(new_queue->current_index);
                    auto next = pick_shuffle_next(*new_queue);
                    new_queue->current_index = next.value_or(0);
                } else {
                    new_queue->current_index++;
                    if (new_queue->current_index >= new_queue->track_indices.size()) {
                        new_queue->current_index = 0;
                    }
                }
                s.queue = new_queue;
            });
            continue;
        }
        
        // METADATA-DRIVEN CODEC SELECTION
        std::unique_ptr<audio::AudioDecoder> decoder = create_decoder_for_track(track);
        
        if (!decoder) {
            publisher_->update([&](model::Snapshot& s) {
                s.alerts.push_back({
                    "error",
                    "Unsupported format: " + track.title,
                    std::chrono::steady_clock::now()
                });
            });
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        
        // Open decoder
        if (!decoder->open(track.path)) {
            publisher_->update([&](model::Snapshot& s) {
                s.alerts.push_back({
                    "error",
                    "Failed to open: " + track.title,
                    std::chrono::steady_clock::now()
                });
            });
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // CRITICAL: Use decoder's ACTUAL sample rate, not metadata
        // Decoder determines the real format after opening the file
        int actual_sample_rate = decoder->get_sample_rate();
        int actual_channels = decoder->get_channels();

        util::Logger::debug("PlaybackCollector: Initializing PipeWire (" +
            std::to_string(actual_sample_rate) + "Hz, " +
            std::to_string(actual_channels) + "ch)");

        // Create PipeWire output with ACTUAL format from decoder
        audio::PipeWireOutput output;
        if (!output.init(audio_context_, actual_sample_rate, actual_channels)) {
            util::Logger::error("Failed to initialize PipeWire output");
            decoder->close();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        util::Logger::debug("PlaybackCollector: PipeWire initialized successfully");

        // Update snapshot with playing state and track index
        publisher_->update([&track_index](model::Snapshot& s) {
            s.player.state = model::PlaybackState::Playing;
            s.player.current_track_index = track_index;
        });
        
        // Playback loop
        constexpr int BUFFER_FRAMES = 16384; // Increased buffer size for high sample rates (85ms @ 192kHz)
        std::vector<float> buffer(BUFFER_FRAMES * decoder->get_channels(), 0.0f);

        bool track_finished = false;

        // STATE TRACKING: Only update snapshot when state actually changes
        bool last_reported_paused = false;  // Track last pause state we reported
        int last_reported_volume = -1;      // Track last volume we reported (-1 = not set)

        while (!stop_token.stop_requested()) {
            // Early exit check - prevents blocking on audio write during shutdown
            if (stop_token.stop_requested()) {
                break;
            }

            // Check if clear requested (Ctrl+d)
            if (clear_requested_.load(std::memory_order_acquire)) {
                util::Logger::debug("PlaybackCollector: Clear detected in main loop - breaking");
                break;
            }

            // Check if paused
            if (paused_) {
                output.pause(true);
                // GUARD: Only update snapshot when pause state changes
                if (last_reported_paused != true) {
                    publisher_->update([](model::Snapshot& s) {
                        s.player.state = model::PlaybackState::Paused;
                    });
                    last_reported_paused = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            } else {
                output.pause(false);
                // GUARD: Only update snapshot when pause state changes
                if (last_reported_paused != false) {
                    publisher_->update([](model::Snapshot& s) {
                        s.player.state = model::PlaybackState::Playing;
                    });
                    last_reported_paused = false;
                }
            }

            // Check if track changed externally
            auto current_snap_ptr = publisher_->get_current();
            if (current_snap_ptr) {
                // Handle seek
                if (current_snap_ptr->player.seek_request_ms >= 0) {
                    int64_t target = current_snap_ptr->player.seek_request_ms;
                    if (decoder->seek_to_ms(target)) {
                        publisher_->update([target](model::Snapshot& s) {
                             s.player.playback_position_ms = static_cast<int>(target);
                             s.player.seek_request_ms = -1; // Clear request
                        });
                    } else {
                         publisher_->update([](model::Snapshot& s) {
                             s.player.seek_request_ms = -1; // Clear request even if failed
                        });
                    }
                }

                // Update volume - GUARD: Only call set_volume when it actually changes
                int current_volume = current_snap_ptr->player.volume_percent;
                if (last_reported_volume != current_volume) {
                    output.set_volume(current_volume);
                    last_reported_volume = current_volume;
                }
                
                // Check if track changed
                if (current_snap_ptr->queue->current_index != starting_index ||
                    current_snap_ptr->queue->track_indices.empty()) {
                    // Track changed by user
                    util::Logger::debug("PlaybackCollector: Detected track change. Breaking loop.");
                    track_finished = false; // Not finished naturally
                    break;
                }
            }
            
            // Update position BEFORE blocking write (10Hz for smooth time display)
            // Placed here so PipeWire buffer stalls don't delay position updates
            static auto last_position_update = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_position_update).count();
            if (elapsed >= 100) {
                // if (elapsed > 500) {
                //     util::Logger::debug("PlaybackCollector: Position update delayed by " +
                //                        std::to_string(elapsed) + "ms (possible PipeWire stall)");
                // }
                publisher_->update([&](model::Snapshot& s) {
                    s.player.playback_position_ms = decoder->get_position_ms();
                });
                last_position_update = now;
            }

            // Clear buffer
            std::fill(buffer.begin(), buffer.end(), 0.0f);

            // Read PCM
            int frames_read = decoder->read_pcm(buffer.data(), BUFFER_FRAMES);

            if (frames_read <= 0) {
                track_finished = true;
                break;
            }

            // Check stop BEFORE blocking write operation
            if (stop_token.stop_requested()) {
                break;
            }

            // Write loop to handle partial writes
            size_t frames_written_total = 0;
            size_t frames_remaining = static_cast<size_t>(frames_read);
            const int channels = decoder->get_channels();
            bool write_error = false;

            while (frames_remaining > 0 && !stop_token.stop_requested() &&
                   !clear_requested_.load(std::memory_order_acquire)) {
                // Calculate offset in buffer
                const float* data_ptr = buffer.data() + (frames_written_total * channels);

                // Write to PipeWire (can block for buffer drain)
                size_t written = output.write(data_ptr, frames_remaining);

                if (written == 0) {
                    util::Logger::error("Failed to write audio data (0 frames) - Skipping track");
                    write_error = true;
                    break;
                }

                frames_written_total += written;
                frames_remaining -= written;
            }

            // Check if clear was requested during write
            if (clear_requested_.load(std::memory_order_acquire)) {
                util::Logger::debug("PlaybackCollector: Clear detected in write loop - breaking");
                break;
            }

            if (write_error) {
                publisher_->update([&](model::Snapshot& s) {
                    s.alerts.push_back({
                        "error",
                        "Audio output failed. Skipping track.",
                        std::chrono::steady_clock::now()
                    });

                    // Advance queue to avoid infinite retry loop - shuffle-aware
                    auto new_queue = std::make_shared<model::QueueState>(*s.queue);
                    if (s.player.shuffle_enabled) {
                        new_queue->played_indices.insert(new_queue->current_index);
                        auto next = pick_shuffle_next(*new_queue);
                        if (next.has_value()) {
                            new_queue->current_index = *next;
                        } else {
                            new_queue->current_index = 0;
                            if (s.player.repeat_mode != model::RepeatMode::All) {
                                s.player.state = model::PlaybackState::Stopped;
                            }
                        }
                    } else {
                        new_queue->current_index++;
                        if (new_queue->current_index >= new_queue->track_indices.size()) {
                            new_queue->current_index = 0;
                            if (s.player.repeat_mode != model::RepeatMode::All) {
                                s.player.state = model::PlaybackState::Stopped;
                            }
                        }
                    }
                    s.queue = new_queue;
                });

                std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Prevent CPU spin
                break;
            }
        }
        
        util::Logger::debug("PlaybackCollector: Loop finished. Closing output...");

        // Reset clear flag (it was consumed)
        if (clear_requested_.load(std::memory_order_acquire)) {
            clear_requested_.store(false, std::memory_order_release);
            util::Logger::debug("PlaybackCollector: Clear flag reset");
        }

        // Handle track finish
        if (track_finished) {
             publisher_->update([&](model::Snapshot& s) {
                if (s.player.repeat_mode == model::RepeatMode::One) {
                     // Will restart loop next iteration because index didn't change
                     s.player.playback_position_ms = 0;
                } else {
                    // Advance queue
                    auto new_queue = std::make_shared<model::QueueState>(*s.queue);

                    if (s.player.shuffle_enabled) {
                        // Shuffle mode: mark current as played, pick random unplayed
                        new_queue->played_indices.insert(new_queue->current_index);

                        auto next = pick_shuffle_next(*new_queue);

                        if (next.has_value()) {
                            new_queue->current_index = *next;
                        } else {
                            // All tracks played - reset cycle
                            new_queue->played_indices.clear();
                            if (s.player.repeat_mode == model::RepeatMode::All) {
                                next = pick_shuffle_next(*new_queue);
                                new_queue->current_index = next.value_or(0);
                            } else {
                                s.player.state = model::PlaybackState::Stopped;
                                new_queue->current_index = 0;
                            }
                        }
                    } else {
                        // Linear mode
                        new_queue->current_index++;

                        if (new_queue->current_index >= new_queue->track_indices.size()) {
                            if (s.player.repeat_mode == model::RepeatMode::All) {
                                 new_queue->current_index = 0;
                            } else {
                                s.player.state = model::PlaybackState::Stopped;
                                new_queue->current_index = 0;
                            }
                        }
                    }
                    s.queue = new_queue;
                }
             });
        }
        
        // Cleanup
        output.close();
        util::Logger::debug("PlaybackCollector: Output closed. Closing decoder...");
        decoder->close();
        decoder.reset();
        util::Logger::debug("PlaybackCollector: Decoder closed. Ready for next track.");
        
        // If we broke out because track changed, the loop will restart and pick up new track
    }
}

std::unique_ptr<audio::AudioDecoder> PlaybackCollector::create_decoder_for_track(const model::Track& track) {
    switch (track.format) {
        case model::AudioFormat::MP3:
            return std::make_unique<audio::MP3Decoder>();
            
        case model::AudioFormat::FLAC:
        case model::AudioFormat::WAV:
            return std::make_unique<audio::FLACDecoder>();
            
        case model::AudioFormat::OGG:
            return std::make_unique<audio::OGGDecoder>();

        case model::AudioFormat::M4A:
            return std::make_unique<audio::M4ADecoder>();

        default:
            return nullptr;
    }
}

std::string PlaybackCollector::format_to_string(model::AudioFormat format) {
    switch (format) {
        case model::AudioFormat::MP3: return "MP3";
        case model::AudioFormat::FLAC: return "FLAC";
        case model::AudioFormat::OGG: return "OGG/Vorbis";
        case model::AudioFormat::WAV: return "WAV";
        case model::AudioFormat::M4A: return "M4A/AAC";
        default: return "Unknown";
    }
}

}  // namespace ouroboros::collectors
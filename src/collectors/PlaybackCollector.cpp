#include "collectors/PlaybackCollector.hpp"
#include "audio/MP3Decoder.hpp"
#include "audio/FLACDecoder.hpp"
#include "audio/OGGDecoder.hpp"
#include "audio/M4ADecoder.hpp"
#include "audio/DSDDecoder.hpp"
#include "audio/PipeWireOutput.hpp"
#include "backend/MetadataParser.hpp"
#include "util/Logger.hpp"
#include "util/Platform.hpp"
#include "events/EventBus.hpp"
#include <thread>
#include <memory>
#include <filesystem>
#include <cmath>
#include <sys/random.h>

namespace ouroboros::collectors {

PlaybackCollector::PlaybackCollector(std::shared_ptr<backend::SnapshotPublisher> publisher)
    : publisher_(publisher) {
    auto& bus = events::EventBus::instance();

    subscriptions_.push_back(bus.subscribe(events::Event::Type::PlayPause,
        [this](const events::Event&) {
            paused_ = !paused_;
        }));

    subscriptions_.push_back(bus.subscribe(events::Event::Type::ClearQueue,
        [this](const events::Event&) {
            clear_requested_.store(true, std::memory_order_release);
            util::Logger::debug("PlaybackCollector: Clear requested (atomic flag set)");
        }));

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

// Position interpolation methods

void PlaybackCollector::update_position_anchor(audio::PipeWireOutput& output) {
    size_t consumed = output.frames_consumed();
    size_t delta_frames = consumed - anchor_consumed_frames_;

    if (anchor_sample_rate_ > 0 && delta_frames > 0) {
        int64_t delta_ms = (static_cast<int64_t>(delta_frames) * 1000) / anchor_sample_rate_;
        anchor_position_ms_ += delta_ms;
    }

    anchor_consumed_frames_ = consumed;
    anchor_time_ = std::chrono::steady_clock::now();
}

int64_t PlaybackCollector::get_interpolated_position_ms() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - anchor_time_).count();
    return anchor_position_ms_ + elapsed;
}

void PlaybackCollector::reset_position_anchor(int64_t position_ms, audio::PipeWireOutput& output) {
    anchor_position_ms_ = position_ms;
    anchor_time_ = std::chrono::steady_clock::now();
    anchor_consumed_frames_ = output.frames_consumed();
}

void PlaybackCollector::run(std::stop_token stop_token) {
    bool last_queue_empty_logged = false;

    // GAPLESS PLAYBACK: Persistent output - lives across tracks
    audio::PipeWireOutput output;
    int output_sample_rate = 0;
    int output_channels = 0;

    while (!stop_token.stop_requested()) {
        auto snap_ptr = publisher_->get_current();
        if (!snap_ptr) {
            util::Logger::debug("PlaybackCollector: No snapshot available");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const auto& snap = *snap_ptr;

        if (!snap.queue->current.has_value()) {
            if (!last_queue_empty_logged) {
                util::Logger::debug("PlaybackCollector: No current track (queue empty or stopped)");
                last_queue_empty_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        last_queue_empty_logged = false;

        int track_index = *snap.queue->current;
        if (track_index < 0 || track_index >= util::narrow_cast<int>(snap.library->tracks.size())) {
            util::Logger::error("PlaybackCollector: Track index out of bounds (idx=" +
                std::to_string(track_index) + ", lib_size=" +
                std::to_string(snap.library->tracks.size()) + ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const auto& track = snap.library->tracks[track_index];
        util::Logger::debug("PlaybackCollector: Starting track: " + track.title);

        util::Logger::debug("PlaybackCollector: Track details - path=" + track.path +
            ", is_valid=" + std::string(track.is_valid ? "true" : "false") +
            ", format=" + std::to_string(static_cast<int>(track.format)) +
            ", error=" + track.error_message);

        int starting_track = track_index;

        // VALIDATION: Skip invalid tracks
        if (!track.is_valid) {
            util::Logger::warn("PlaybackCollector: SKIPPING invalid track - path=" + track.path +
                ", error=" + track.error_message);
            publisher_->update([&](model::Snapshot& s) {
                s.alerts.push_back({
                    "error",
                    "Cannot play: " + track.title + " - " + track.error_message,
                    std::chrono::steady_clock::now()
                });

                auto new_queue = std::make_shared<model::QueueState>(*s.queue);
                if (new_queue->current.has_value()) {
                    new_queue->history.push_back(*new_queue->current);
                }
                if (!new_queue->future.empty()) {
                    if (s.player.shuffle_enabled) {
                        uint64_t rand_val;
                        getrandom(&rand_val, sizeof(rand_val), 0);
                        size_t idx = rand_val % new_queue->future.size();
                        new_queue->current = new_queue->future[idx];
                        new_queue->future.erase(new_queue->future.begin() + idx);
                    } else {
                        new_queue->current = new_queue->future.front();
                        new_queue->future.erase(new_queue->future.begin());
                    }
                } else {
                    new_queue->current = std::nullopt;
                }
                s.queue = new_queue;
            });
            continue;
        }

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

        int actual_sample_rate = decoder->get_sample_rate();
        int actual_channels = decoder->get_channels();

        // GAPLESS: Only reinit output if format changed
        bool format_changed = (actual_sample_rate != output_sample_rate ||
                               actual_channels != output_channels);

        if (format_changed) {
            if (output.is_initialized()) {
                util::Logger::debug("PlaybackCollector: Format change detected (" +
                    std::to_string(output_sample_rate) + "Hz/" + std::to_string(output_channels) + "ch -> " +
                    std::to_string(actual_sample_rate) + "Hz/" + std::to_string(actual_channels) + "ch), reinitializing");
                output.close();
            }

            util::Logger::debug("PlaybackCollector: Initializing PipeWire (" +
                std::to_string(actual_sample_rate) + "Hz, " +
                std::to_string(actual_channels) + "ch)");

            if (!output.init(audio_context_, actual_sample_rate, actual_channels)) {
                util::Logger::error("Failed to initialize PipeWire output");
                decoder->close();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            output_sample_rate = actual_sample_rate;
            output_channels = actual_channels;
            util::Logger::debug("PlaybackCollector: PipeWire initialized successfully");
        } else {
            util::Logger::debug("PlaybackCollector: Reusing PipeWire stream (gapless transition)");
        }

        // Initialize position anchor for this track
        anchor_sample_rate_ = actual_sample_rate;
        reset_position_anchor(0, output);

        // Update snapshot with playing state
        publisher_->update([&track_index](model::Snapshot& s) {
            s.player.state = model::PlaybackState::Playing;
            s.player.current_track_index = track_index;
        });

        // Decode loop: push PCM into ring buffer
        constexpr int DECODE_CHUNK = 4096;
        std::vector<float> buffer(DECODE_CHUNK * decoder->get_channels(), 0.0f);
        auto& ring = output.ring_buffer();

        bool track_finished = false;
        bool was_paused = false;
        auto last_position_update = std::chrono::steady_clock::now();

        while (!stop_token.stop_requested()) {
            if (stop_token.stop_requested()) break;

            // Check clear request
            if (clear_requested_.load(std::memory_order_acquire)) {
                util::Logger::debug("PlaybackCollector: Clear detected - flushing and breaking");
                output.pause(true);
                output.flush_ring();
                output.pause(false);
                break;
            }

            // Handle pause
            if (paused_) {
                if (!was_paused) {
                    output.pause(true);
                    update_position_anchor(output);
                    frozen_position_ms_ = get_interpolated_position_ms();
                    publisher_->update([this](model::Snapshot& s) {
                        s.player.state = model::PlaybackState::Paused;
                        s.player.playback_position_ms = static_cast<int>(frozen_position_ms_);
                    });
                    was_paused = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            } else if (was_paused) {
                // Resuming from pause
                reset_position_anchor(frozen_position_ms_, output);
                output.pause(false);
                publisher_->update([](model::Snapshot& s) {
                    s.player.state = model::PlaybackState::Playing;
                });
                was_paused = false;
            }

            // Check track change
            auto current_snap_ptr = publisher_->get_current();
            if (current_snap_ptr) {
                // Handle seek
                if (current_snap_ptr->player.seek_request_ms >= 0) {
                    int64_t target = current_snap_ptr->player.seek_request_ms;

                    output.pause(true);
                    output.flush_ring();

                    if (decoder->seek_to_ms(target)) {
                        reset_position_anchor(target, output);
                        publisher_->update([target](model::Snapshot& s) {
                            s.player.playback_position_ms = static_cast<int>(target);
                            s.player.seek_request_ms = -1;
                        });
                    } else {
                        publisher_->update([](model::Snapshot& s) {
                            s.player.seek_request_ms = -1;
                        });
                    }

                    output.pause(false);
                }

                // Volume update
                int current_volume = current_snap_ptr->player.volume_percent;
                output.set_volume(current_volume);

                // Track change detection
                if (!current_snap_ptr->queue->current.has_value() ||
                    *current_snap_ptr->queue->current != starting_track) {
                    util::Logger::debug("PlaybackCollector: Detected track change. Breaking loop.");
                    track_finished = false;
                    break;
                }
            }

            // Decode into ring buffer
            if (ring.write_available_frames() >= static_cast<size_t>(DECODE_CHUNK)) {
                std::fill(buffer.begin(), buffer.end(), 0.0f);
                int frames_read = decoder->read_pcm(buffer.data(), DECODE_CHUNK);

                if (frames_read <= 0) {
                    track_finished = true;
                    break;
                }

                // Clamp NaN/Inf on producer side (keeps on_process RT-safe)
                size_t total_samples = static_cast<size_t>(frames_read) * decoder->get_channels();
                for (size_t i = 0; i < total_samples; ++i) {
                    float val = buffer[i];
                    if (!std::isfinite(val)) {
                        buffer[i] = 0.0f;
                    } else if (val > 1.0f) {
                        buffer[i] = 1.0f;
                    } else if (val < -1.0f) {
                        buffer[i] = -1.0f;
                    }
                }

                ring.write(buffer.data(), frames_read);
            } else {
                // Ring buffer full - PipeWire will drain it
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // Position update at ~30Hz (interpolated from consumed frames)
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_position_update).count();
            if (elapsed >= 33) {
                update_position_anchor(output);
                int64_t display_ms = get_interpolated_position_ms();
                publisher_->update([display_ms](model::Snapshot& s) {
                    s.player.playback_position_ms = static_cast<int>(display_ms);
                });
                last_position_update = now;
            }
        }

        util::Logger::debug("PlaybackCollector: Loop finished.");

        // Reset clear flag
        if (clear_requested_.load(std::memory_order_acquire)) {
            clear_requested_.store(false, std::memory_order_release);
            util::Logger::debug("PlaybackCollector: Clear flag reset");
        }

        // Handle track finish (auto-advance)
        // No drain wait: ring buffer carries track N's tail samples while we
        // set up track N+1. PipeWire plays continuously from the ring buffer,
        // giving true gapless transitions (same format) without silence gaps.
        if (track_finished) {
            publisher_->update([&](model::Snapshot& s) {
                if (s.player.repeat_mode == model::RepeatMode::One) {
                    s.player.playback_position_ms = 0;
                } else {
                    auto new_queue = std::make_shared<model::QueueState>(*s.queue);

                    if (new_queue->current.has_value()) {
                        new_queue->history.push_back(*new_queue->current);
                    }

                    if (!new_queue->future.empty()) {
                        if (s.player.shuffle_enabled) {
                            uint64_t rand_val;
                            getrandom(&rand_val, sizeof(rand_val), 0);
                            size_t idx = rand_val % new_queue->future.size();
                            new_queue->current = new_queue->future[idx];
                            new_queue->future.erase(new_queue->future.begin() + idx);
                        } else {
                            new_queue->current = new_queue->future.front();
                            new_queue->future.erase(new_queue->future.begin());
                        }
                    } else if (s.player.repeat_mode == model::RepeatMode::All) {
                        new_queue->future = new_queue->history;
                        new_queue->history.clear();
                        if (!new_queue->future.empty()) {
                            if (s.player.shuffle_enabled) {
                                uint64_t rand_val;
                                getrandom(&rand_val, sizeof(rand_val), 0);
                                size_t idx = rand_val % new_queue->future.size();
                                new_queue->current = new_queue->future[idx];
                                new_queue->future.erase(new_queue->future.begin() + idx);
                            } else {
                                new_queue->current = new_queue->future.front();
                                new_queue->future.erase(new_queue->future.begin());
                            }
                        }
                    } else {
                        new_queue->current = std::nullopt;
                        s.player.state = model::PlaybackState::Stopped;
                    }
                    s.queue = new_queue;
                }
            });
        }

        decoder->close();
        decoder.reset();
        util::Logger::debug("PlaybackCollector: Decoder closed. Ready for next track (gapless).");

        // Handle clear request - close output to release audio device
        if (clear_requested_.load(std::memory_order_acquire)) {
            output.close();
            output_sample_rate = 0;
            output_channels = 0;
            util::Logger::debug("PlaybackCollector: Output closed due to clear request.");
        }
    }

    // Final cleanup
    if (output.is_initialized()) {
        output.close();
        util::Logger::debug("PlaybackCollector: Final output cleanup on shutdown.");
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

        case model::AudioFormat::DSD:
            return std::make_unique<audio::DSDDecoder>();

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
        case model::AudioFormat::DSD: return "DSD";
        default: return "Unknown";
    }
}

}  // namespace ouroboros::collectors

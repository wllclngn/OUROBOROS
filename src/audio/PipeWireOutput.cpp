#include "audio/PipeWireOutput.hpp"
#include "util/Logger.hpp"
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>

namespace audio {

static void on_process(void* userdata) {
    auto* output = static_cast<PipeWireOutput*>(userdata);
    (void)output;
    // Process callback - will be handled by PipeWire
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = nullptr,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = nullptr,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_process,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr,
};

PipeWireOutput::PipeWireOutput() {
}

PipeWireOutput::~PipeWireOutput() {
    close();
}

bool PipeWireOutput::init(PipeWireContext& context, int sample_rate, int channels) {
    ouroboros::util::Logger::debug("PipeWireOutput: Initializing (" +
                                   std::to_string(sample_rate) + "Hz, " +
                                   std::to_string(channels) + "ch)");

    if (stream_) {
        ouroboros::util::Logger::debug("PipeWireOutput: Already initialized, skipping");
        return false;  // Already initialized
    }

    context_ = &context;
    sample_rate_ = sample_rate;
    channels_ = channels;

    struct pw_thread_loop* loop = context_->get_loop();
    if (!loop) {
        ouroboros::util::Logger::error("PipeWireOutput: Context loop is null");
        return false;
    }

    // CRITICAL: Lock the thread loop for all PipeWire operations
    pw_thread_loop_lock(loop);

    // Set stream properties
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        nullptr
    );

    // Create stream
    stream_ = pw_stream_new_simple(
        pw_thread_loop_get_loop(loop),
        "Ouroboros Music Player",
        props,
        &stream_events,
        this
    );

    if (!stream_) {
        ouroboros::util::Logger::error("PipeWireOutput: Failed to create stream");
        pw_thread_loop_unlock(loop);
        return false;
    }

    // Build audio format parameters
    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = static_cast<uint32_t>(channels_);
    info.rate = static_cast<uint32_t>(sample_rate_);

    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    // Connect stream
    int result = pw_stream_connect(
        stream_,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS
        ),
        params, 1
    );

    pw_thread_loop_unlock(loop);

    if (result < 0) {
        ouroboros::util::Logger::error("PipeWireOutput: Stream connect failed (result=" +
                                       std::to_string(result) + ")");
        close();
        return false;
    }

    ouroboros::util::Logger::info("PipeWireOutput: Initialized successfully");
    return true;
}

void PipeWireOutput::close() {
    ouroboros::util::Logger::debug("PipeWireOutput: Closing output");
    if (stream_ && context_ && context_->get_loop()) {
        struct pw_thread_loop* loop = context_->get_loop();

        pw_thread_loop_lock(loop);

        // Drain: wait for all queued audio to play out before destroying
        // This prevents the pop/click from cutting off buffered samples
        pw_stream_flush(stream_, true);

        pw_stream_destroy(stream_);
        pw_thread_loop_unlock(loop);

        stream_ = nullptr;
    } else if (stream_) {
        // Fallback if context is gone (unsafe but trying to cleanup)
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }

    // Reset format tracking for clean reinit
    sample_rate_ = 0;
    channels_ = 0;
}

size_t PipeWireOutput::write(const float* data, size_t frames) {
    if (!stream_ || !context_ || !context_->get_loop() || !data || frames == 0) {
        return 0;
    }

    // Throttled logging - log every 100th call to avoid spam
    static int write_call_count = 0;
    write_call_count++;
    bool should_log = (write_call_count % 100 == 0);

    if (should_log) {
        ouroboros::util::Logger::debug("PipeWireOutput: Writing " + std::to_string(frames) + " frames");
    }

    struct pw_thread_loop* loop = context_->get_loop();
    struct pw_buffer* pw_buf = nullptr;

    // First, wait for stream to be in STREAMING state
    const int max_state_retries = 100;  // Up to 2 seconds
    enum pw_stream_state final_state = PW_STREAM_STATE_UNCONNECTED;
    for (int i = 0; i < max_state_retries; ++i) {
        pw_thread_loop_lock(loop);
        enum pw_stream_state state = pw_stream_get_state(stream_, nullptr);
        pw_thread_loop_unlock(loop);

        final_state = state;

        if (state == PW_STREAM_STATE_STREAMING) {
            break;  // Stream is active!
        }

        if (state == PW_STREAM_STATE_ERROR) {
            ouroboros::util::Logger::error("PipeWireOutput: Stream in ERROR state");
            return 0;
        }

        // Log state waits periodically
        if (i % 10 == 0 && i > 0) {
            const char* state_names[] = {"ERROR", "UNCONNECTED", "CONNECTING", "PAUSED", "STREAMING"};
            ouroboros::util::Logger::debug("PipeWireOutput: Waiting for STREAMING state (current=" +
                                          std::string(state_names[state + 1]) + ", attempt=" +
                                          std::to_string(i) + ")");
        }

        // Wait for activation (suspended sinks take time)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Log what state we ended up in
    if (final_state != PW_STREAM_STATE_STREAMING) {
        const char* state_names[] = {"ERROR", "UNCONNECTED", "CONNECTING", "PAUSED", "STREAMING"};
        ouroboros::util::Logger::error("PipeWireOutput: Stream never reached STREAMING (stuck in state=" +
                                      std::string(state_names[final_state + 1]) + ")");
        return 0;
    }

    // Now try to get a buffer (stream should be ready)
    const int max_retries = 50;
    for (int i = 0; i < max_retries; ++i) {
        pw_thread_loop_lock(loop);
        pw_buf = pw_stream_dequeue_buffer(stream_);
        if (pw_buf) {
            break; // Got one!
        }
        pw_thread_loop_unlock(loop);

        // Log buffer retry waits periodically
        if (i % 10 == 0 && i > 0) {
            int delay_ms = std::min(2 << std::min(i, 4), 50);
            ouroboros::util::Logger::debug("PipeWireOutput: Waiting for buffer (attempt=" +
                                          std::to_string(i) + ", delay=" + std::to_string(delay_ms) + "ms)");
        }

        // Exponential backoff: 2ms, 4ms, 8ms, 16ms, capped at 50ms
        int delay_ms = std::min(2 << std::min(i, 4), 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    if (!pw_buf) {
        // Log for diagnostics
        ouroboros::util::Logger::error("PipeWireOutput: Failed to acquire buffer after " +
                                      std::to_string(max_retries) + " retries - sink may be suspended");
        return 0;
    }
    
    struct spa_buffer* buf = pw_buf->buffer;
    if (!buf->datas[0].data) {
        pw_stream_queue_buffer(stream_, pw_buf);
        pw_thread_loop_unlock(loop);
        return 0;
    }
    
    // Calculate how much we can write
    size_t bytes_per_frame = channels_ * sizeof(float);
    size_t max_frames = buf->datas[0].maxsize / bytes_per_frame;
    size_t frames_to_write = std::min(frames, max_frames);
    size_t bytes_to_write = frames_to_write * bytes_per_frame;
    
    // Apply volume and copy with CLAMPING + NaN/Inf checking
    float* dst = static_cast<float*>(buf->datas[0].data);
    float scale = volume_ / 100.0f;
    size_t total_samples = frames_to_write * channels_;

    static int nan_count = 0;
    for (size_t i = 0; i < total_samples; ++i) {
        float val = data[i] * scale;

        if (!std::isfinite(val)) {
            if (nan_count % 100 == 0) {
                ouroboros::util::Logger::warn(std::string("PipeWireOutput: NaN/Inf sample detected and clamped ") +
                                             "(count=" + std::to_string(nan_count) + ")");
            }
            nan_count++;
            val = 0.0f;
        } else if (val > 1.0f) {
            val = 1.0f;
        } else if (val < -1.0f) {
            val = -1.0f;
        }

        dst[i] = val;
    }
    
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = bytes_per_frame;
    buf->datas[0].chunk->size = bytes_to_write;
    
    pw_stream_queue_buffer(stream_, pw_buf);
    pw_thread_loop_unlock(loop);
    
    return frames_to_write;
}

void PipeWireOutput::set_volume(int percent) {
    int new_volume = std::clamp(percent, 0, 100);

    // GUARD: Only log/update if volume actually changed
    if (volume_ == new_volume) {
        return;  // Already at this volume, skip
    }

    volume_ = new_volume;
    ouroboros::util::Logger::debug("PipeWireOutput: Volume set to " + std::to_string(volume_) + "%");
}

void PipeWireOutput::pause(bool paused) {
    // GUARD: Only log/update if state actually changed
    if (paused_ == paused) {
        return;  // Already in this state, skip
    }

    ouroboros::util::Logger::info("PipeWireOutput: Pause state changed to " +
                                  std::string(paused ? "true" : "false"));

    if (!stream_ || !context_ || !context_->get_loop()) return;

    struct pw_thread_loop* loop = context_->get_loop();

    pw_thread_loop_lock(loop);
    pw_stream_set_active(stream_, !paused);
    pw_thread_loop_unlock(loop);

    paused_ = paused;
}

} // namespace audio
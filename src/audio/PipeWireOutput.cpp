#include "audio/PipeWireOutput.hpp"
#include "util/Logger.hpp"
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/audio/format-utils.h>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace audio {

// PipeWire RT callback: pulls audio from ring buffer, fills PipeWire buffer.
// Runs on PipeWire's real-time thread. No allocations, no logging, no locks.
void on_process_callback(void* userdata) {
    auto* output = static_cast<PipeWireOutput*>(userdata);

    struct pw_buffer* pw_buf = pw_stream_dequeue_buffer(output->stream_);
    if (!pw_buf) return;

    struct spa_buffer* buf = pw_buf->buffer;
    float* dst = static_cast<float*>(buf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(output->stream_, pw_buf);
        return;
    }

    size_t bytes_per_frame = output->channels_ * sizeof(float);
    size_t max_frames = buf->datas[0].maxsize / bytes_per_frame;
    if (pw_buf->requested > 0)
        max_frames = std::min(max_frames, static_cast<size_t>(pw_buf->requested));

    // Pull from ring buffer
    size_t frames_read = output->ring_.read(dst, max_frames);

    // Apply volume scaling in-place
    int vol = output->volume_.load(std::memory_order_relaxed);
    if (vol != 100 && frames_read > 0) {
        float scale = vol / 100.0f;
        size_t total_samples = frames_read * output->channels_;
        for (size_t i = 0; i < total_samples; ++i) {
            dst[i] *= scale;
        }
    }

    // Underrun: fill remainder with silence
    if (frames_read < max_frames) {
        size_t silence_start = frames_read * output->channels_;
        size_t silence_count = (max_frames - frames_read) * output->channels_;
        std::memset(dst + silence_start, 0, silence_count * sizeof(float));
        frames_read = max_frames;
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = bytes_per_frame;
    buf->datas[0].chunk->size = frames_read * bytes_per_frame;

    pw_buf->size = frames_read;
    pw_stream_queue_buffer(output->stream_, pw_buf);
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
    .process = on_process_callback,
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
        return false;
    }

    context_ = &context;
    sample_rate_ = sample_rate;
    channels_ = channels;

    // Initialize ring buffer: 8192 frames capacity
    ring_.init(8192, channels);

    struct pw_thread_loop* loop = context_->get_loop();
    if (!loop) {
        ouroboros::util::Logger::error("PipeWireOutput: Context loop is null");
        return false;
    }

    pw_thread_loop_lock(loop);

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        nullptr
    );

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

    uint8_t buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = static_cast<uint32_t>(channels_);
    info.rate = static_cast<uint32_t>(sample_rate_);

    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

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

    ouroboros::util::Logger::info("PipeWireOutput: Initialized successfully (pull model, ring buffer 8192 frames)");
    return true;
}

void PipeWireOutput::close() {
    ouroboros::util::Logger::debug("PipeWireOutput: Closing output");
    if (stream_ && context_ && context_->get_loop()) {
        struct pw_thread_loop* loop = context_->get_loop();

        pw_thread_loop_lock(loop);
        pw_stream_flush(stream_, true);
        pw_stream_destroy(stream_);
        pw_thread_loop_unlock(loop);

        stream_ = nullptr;
    } else if (stream_) {
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }

    ring_.reset();

    sample_rate_ = 0;
    channels_ = 0;
}

void PipeWireOutput::flush_ring() {
    ring_.reset();
}

void PipeWireOutput::set_volume(int percent) {
    int new_volume = std::clamp(percent, 0, 100);
    int old_volume = volume_.load(std::memory_order_relaxed);
    if (old_volume == new_volume) return;

    volume_.store(new_volume, std::memory_order_relaxed);
    ouroboros::util::Logger::debug("PipeWireOutput: Volume set to " + std::to_string(new_volume) + "%");
}

void PipeWireOutput::pause(bool paused) {
    if (paused_ == paused) return;

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

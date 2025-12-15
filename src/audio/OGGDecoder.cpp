#include "audio/OGGDecoder.hpp"
#include "util/Logger.hpp"
#include <cstring>
#include <cstdio>

namespace audio {

OGGDecoder::OGGDecoder() {
    std::memset(&vf_, 0, sizeof(vf_));
}

OGGDecoder::~OGGDecoder() {
    close();
}

bool OGGDecoder::open(const std::string& filepath) {
    ouroboros::util::Logger::debug("OGGDecoder: Opening file: " + filepath);

    FILE* f = fopen(filepath.c_str(), "rb");
    if (!f) {
        ouroboros::util::Logger::error("OGGDecoder: Failed to fopen file: " + filepath);
        return false;
    }

    if (ov_open(f, &vf_, nullptr, 0) < 0) {
        ouroboros::util::Logger::error("OGGDecoder: Failed to open OGG stream: " + filepath);
        fclose(f);
        return false;
    }

    vorbis_info* info = ov_info(&vf_, -1);
    if (!info) {
        ouroboros::util::Logger::error("OGGDecoder: Failed to get vorbis info for: " + filepath);
        ov_clear(&vf_);
        return false;
    }

    sample_rate_ = info->rate;
    channels_ = info->channels;
    total_frames_ = static_cast<long>(ov_pcm_total(&vf_, -1));
    position_frames_ = 0;
    is_open_ = true;

    ouroboros::util::Logger::info("OGGDecoder: Opened successfully - " +
                       std::to_string(sample_rate_) + "Hz, " +
                       std::to_string(channels_) + "ch, " +
                       std::to_string(total_frames_) + " frames");

    return true;
}

void OGGDecoder::close() {
    ouroboros::util::Logger::debug("OGGDecoder: Closing decoder");
    if (is_open_) {
        ov_clear(&vf_);
        is_open_ = false;
    }
    sample_rate_ = 0;
    channels_ = 0;
    total_frames_ = 0;
    position_frames_ = 0;
}

int OGGDecoder::read_pcm(float* buffer, int max_frames) {
    if (!is_open_ || !buffer) return 0;

    // Throttled logging - log every 1000th call to avoid spam
    static int read_call_count = 0;
    read_call_count++;
    bool should_log = (read_call_count % 1000 == 0);

    float** pcm;
    int frames_read = 0;

    while (frames_read < max_frames) {
        long ret = ov_read_float(&vf_, &pcm, max_frames - frames_read, nullptr);
        if (ret <= 0) break;  // EOF or error

        // Interleave channels
        for (int i = 0; i < ret; i++) {
            for (int ch = 0; ch < channels_; ch++) {
                buffer[(frames_read + i) * channels_ + ch] = pcm[ch][i];
            }
        }

        frames_read += ret;
    }

    position_frames_ += frames_read;

    if (should_log) {
        ouroboros::util::Logger::debug("OGGDecoder: Read " + std::to_string(frames_read) +
                           " frames from " + std::to_string(max_frames) + " requested " +
                           "(position: " + std::to_string(position_frames_) + ")");
    }

    return frames_read;
}

bool OGGDecoder::seek(long frame) {
    ouroboros::util::Logger::debug("OGGDecoder: Seeking to frame " + std::to_string(frame));

    if (!is_open_) return false;

    int result = ov_pcm_seek(&vf_, static_cast<ogg_int64_t>(frame));
    if (result != 0) {
        ouroboros::util::Logger::error("OGGDecoder: Seek failed (code=" + std::to_string(result) + ")");
        return false;
    }

    position_frames_ = frame;
    return true;
}

} // namespace audio

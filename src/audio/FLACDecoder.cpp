#include "audio/FLACDecoder.hpp"
#include "util/Logger.hpp"
#include <cstring>

namespace audio {

FLACDecoder::FLACDecoder() {
    std::memset(&info_, 0, sizeof(info_));
}

FLACDecoder::~FLACDecoder() {
    close();
}

bool FLACDecoder::open(const std::string& filepath) {
    ouroboros::util::Logger::debug("FLACDecoder: Opening file: " + filepath);

    file_ = sf_open(filepath.c_str(), SFM_READ, &info_);
    if (!file_) {
        ouroboros::util::Logger::error("FLACDecoder: Failed to open file: " + filepath);
        return false;
    }

    sample_rate_ = info_.samplerate;
    channels_ = info_.channels;
    total_frames_ = static_cast<long>(info_.frames);
    position_frames_ = 0;

    ouroboros::util::Logger::info("FLACDecoder: Opened successfully - " +
                       std::to_string(sample_rate_) + "Hz, " +
                       std::to_string(channels_) + "ch, " +
                       std::to_string(total_frames_) + " frames");

    return true;
}

void FLACDecoder::close() {
    ouroboros::util::Logger::debug("FLACDecoder: Closing decoder");
    if (file_) {
        sf_close(file_);
        file_ = nullptr;
    }
    sample_rate_ = 0;
    channels_ = 0;
    total_frames_ = 0;
    position_frames_ = 0;
}

int FLACDecoder::read_pcm(float* buffer, int max_frames) {
    if (!file_ || !buffer) {
        ouroboros::util::Logger::error("FLACDecoder: Invalid state - file or buffer is null");
        return 0;
    }

    // Throttled logging - log every 1000th call to avoid spam
    static int read_call_count = 0;
    read_call_count++;
    bool should_log = (read_call_count % 1000 == 0);

    sf_count_t frames_read = sf_readf_float(file_, buffer, max_frames);
    position_frames_ += static_cast<long>(frames_read);

    if (should_log) {
        ouroboros::util::Logger::debug("FLACDecoder: Read " + std::to_string(frames_read) +
                           " frames (position: " + std::to_string(position_frames_) + ")");
    }

    return static_cast<int>(frames_read);
}

bool FLACDecoder::seek(long frame) {
    ouroboros::util::Logger::debug("FLACDecoder: Seeking to frame " + std::to_string(frame));

    if (!file_) return false;

    sf_count_t result = sf_seek(file_, static_cast<sf_count_t>(frame), SEEK_SET);
    if (result < 0) {
        ouroboros::util::Logger::error("FLACDecoder: Seek failed to frame " + std::to_string(frame));
        return false;
    }

    position_frames_ = static_cast<long>(result);
    return true;
}

} // namespace audio

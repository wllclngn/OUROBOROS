#include "audio/MP3Decoder.hpp"
#include "util/Logger.hpp"
#include <cstring>
#include <vector>

namespace audio {

MP3Decoder::MP3Decoder() {
    ouroboros::util::Logger::debug("MP3Decoder: Constructor called");
    mpg123_init();
    handle_ = mpg123_new(nullptr, nullptr);
}

MP3Decoder::~MP3Decoder() {
    ouroboros::util::Logger::debug("MP3Decoder: Destructor called");
    close();
    if (handle_) {
        mpg123_delete(handle_);
        handle_ = nullptr;
    }
    mpg123_exit();
}

bool MP3Decoder::open(const std::string& filepath) {
    ouroboros::util::Logger::debug("MP3Decoder: Opening file: " + filepath);

    if (!handle_) {
        ouroboros::util::Logger::error("MP3Decoder: Handle is null");
        return false;
    }

    if (mpg123_open(handle_, filepath.c_str()) != MPG123_OK) {
        ouroboros::util::Logger::error("MP3Decoder: Failed to open file: " + filepath +
                           " (error: " + std::string(mpg123_strerror(handle_)) + ")");
        return false;
    }

    // Get format info
    long rate;
    int channels, encoding;
    if (mpg123_getformat(handle_, &rate, &channels, &encoding) != MPG123_OK) {
        ouroboros::util::Logger::error("MP3Decoder: Failed to get format for: " + filepath);
        mpg123_close(handle_);
        return false;
    }

    sample_rate_ = static_cast<int>(rate);
    channels_ = channels;

    // Force signed 16-bit output for compatibility
    mpg123_format_none(handle_);
    if (mpg123_format(handle_, rate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) {
        ouroboros::util::Logger::error("MP3Decoder: Failed to set output format for: " + filepath);
        mpg123_close(handle_);
        return false;
    }

    // Get total length
    off_t length = mpg123_length(handle_);
    total_frames_ = (length == MPG123_ERR) ? 0 : static_cast<long>(length);

    position_frames_ = 0;

    ouroboros::util::Logger::info("MP3Decoder: Opened successfully - " +
                       std::to_string(sample_rate_) + "Hz, " +
                       std::to_string(channels_) + "ch, " +
                       std::to_string(total_frames_) + " frames");

    return true;
}

void MP3Decoder::close() {
    ouroboros::util::Logger::debug("MP3Decoder: Closing decoder");
    if (handle_) {
        mpg123_close(handle_);
    }
    sample_rate_ = 0;
    channels_ = 0;
    total_frames_ = 0;
    position_frames_ = 0;
}

int MP3Decoder::read_pcm(float* buffer, int max_frames) {
    if (!handle_ || !buffer) return 0;

    // Throttled logging - log every 1000th call to avoid spam
    static int read_call_count = 0;
    read_call_count++;
    bool should_log = (read_call_count % 1000 == 0);

    // We configured mpg123 for S16, but we need to output floats
    // Calculate bytes needed for S16
    size_t s16_bytes_wanted = max_frames * channels_ * sizeof(short);
    std::vector<short> s16_buffer(max_frames * channels_);

    size_t bytes_read = 0;

    int result = mpg123_read(handle_,
                             reinterpret_cast<unsigned char*>(s16_buffer.data()),
                             s16_bytes_wanted,
                             &bytes_read);

    // CRITICAL FIX: Check for errors BEFORE using data
    if (result == MPG123_ERR) {
        ouroboros::util::Logger::error("MP3Decoder: Read error detected (MPG123_ERR), returning 0 frames");
        // Error occurred - do NOT use any data, even if bytes_read > 0
        return 0;
    }

    if (result == MPG123_NEW_FORMAT) {
        ouroboros::util::Logger::debug("MP3Decoder: Format changed mid-stream, retrying read");
        // Format changed - try one more time

        result = mpg123_read(handle_,
                             reinterpret_cast<unsigned char*>(s16_buffer.data()),
                             s16_bytes_wanted,
                             &bytes_read);

        // Check again for errors after retry
        if (result == MPG123_ERR) {
            ouroboros::util::Logger::error("MP3Decoder: Read error after format change retry");
            return 0;
        }
    }

    if (result == MPG123_DONE && bytes_read == 0) {
        return 0;
    }

    // Convert S16 to Float [-1.0, 1.0]
    int samples_read = bytes_read / sizeof(short);
    for (int i = 0; i < samples_read; ++i) {
        buffer[i] = s16_buffer[i] / 32768.0f;
    }

    int frames_read = samples_read / channels_;
    position_frames_ += frames_read;

    if (should_log) {
        ouroboros::util::Logger::debug("MP3Decoder: Read " + std::to_string(frames_read) +
                           " frames (position: " + std::to_string(position_frames_) + ")");
    }

    return frames_read;
}

bool MP3Decoder::seek(long frame) {
    ouroboros::util::Logger::debug("MP3Decoder: Seeking to frame " + std::to_string(frame));

    if (!handle_) return false;

    off_t result = mpg123_seek(handle_, static_cast<off_t>(frame), SEEK_SET);
    if (result < 0) {
        ouroboros::util::Logger::error("MP3Decoder: Seek failed to frame " + std::to_string(frame));
        return false;
    }

    position_frames_ = static_cast<long>(result);
    return true;
}

} // namespace audio

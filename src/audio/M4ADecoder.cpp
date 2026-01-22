#include "audio/M4ADecoder.hpp"
#include "util/Logger.hpp"
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace audio {

M4ADecoder::M4ADecoder() {
    ouroboros::util::Logger::debug("M4ADecoder: Constructor called");
}

M4ADecoder::~M4ADecoder() {
    ouroboros::util::Logger::debug("M4ADecoder: Destructor called");
    close();
}

bool M4ADecoder::open(const std::string& filepath) {
    ouroboros::util::Logger::debug("M4ADecoder: Opening file: " + filepath);

    // Open input file
    int ret = avformat_open_input(&format_ctx_, filepath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        ouroboros::util::Logger::error("M4ADecoder: Failed to open file: " + filepath + " (" + errbuf + ")");
        return false;
    }

    // Find stream info
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to find stream info");
        close();
        return false;
    }

    // Find audio stream
    audio_stream_index_ = -1;
    for (unsigned i = 0; i < format_ctx_->nb_streams; i++) {
        if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index_ = i;
            break;
        }
    }

    if (audio_stream_index_ < 0) {
        ouroboros::util::Logger::error("M4ADecoder: No audio stream found");
        close();
        return false;
    }

    AVStream* audio_stream = format_ctx_->streams[audio_stream_index_];
    AVCodecParameters* codecpar = audio_stream->codecpar;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        ouroboros::util::Logger::error("M4ADecoder: Decoder not found for codec ID " +
            std::to_string(codecpar->codec_id));
        close();
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to allocate codec context");
        close();
        return false;
    }

    // Copy codec parameters
    ret = avcodec_parameters_to_context(codec_ctx_, codecpar);
    if (ret < 0) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to copy codec parameters");
        close();
        return false;
    }

    // Open codec
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to open codec");
        close();
        return false;
    }

    // Store format info
    sample_rate_ = codec_ctx_->sample_rate;
    channels_ = codec_ctx_->ch_layout.nb_channels;

    // Calculate total frames from duration
    if (audio_stream->duration != AV_NOPTS_VALUE) {
        double duration_sec = audio_stream->duration * av_q2d(audio_stream->time_base);
        total_frames_ = static_cast<long>(duration_sec * sample_rate_);
    } else if (format_ctx_->duration != AV_NOPTS_VALUE) {
        double duration_sec = format_ctx_->duration / static_cast<double>(AV_TIME_BASE);
        total_frames_ = static_cast<long>(duration_sec * sample_rate_);
    } else {
        total_frames_ = 0;
    }

    // Set up resampler to convert to float planar -> float interleaved
    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to allocate resampler");
        close();
        return false;
    }

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, channels_);

    av_opt_set_chlayout(swr_ctx_, "in_chlayout", &codec_ctx_->ch_layout, 0);
    av_opt_set_chlayout(swr_ctx_, "out_chlayout", &out_ch_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", sample_rate_, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", sample_rate_, 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", codec_ctx_->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    ret = swr_init(swr_ctx_);
    if (ret < 0) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to initialize resampler");
        close();
        return false;
    }

    av_channel_layout_uninit(&out_ch_layout);

    // Allocate packet and frame
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!packet_ || !frame_) {
        ouroboros::util::Logger::error("M4ADecoder: Failed to allocate packet/frame");
        close();
        return false;
    }

    position_frames_ = 0;

    ouroboros::util::Logger::info("M4ADecoder: Opened successfully - " +
                       std::to_string(sample_rate_) + "Hz, " +
                       std::to_string(channels_) + "ch, " +
                       std::to_string(total_frames_) + " frames");

    return true;
}

void M4ADecoder::close() {
    ouroboros::util::Logger::debug("M4ADecoder: Closing decoder");

    if (residual_buffer_) {
        delete[] residual_buffer_;
        residual_buffer_ = nullptr;
    }
    residual_frames_ = 0;
    residual_offset_ = 0;

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }

    audio_stream_index_ = -1;
    sample_rate_ = 0;
    channels_ = 0;
    total_frames_ = 0;
    position_frames_ = 0;
}

int M4ADecoder::read_pcm(float* buffer, int max_frames) {
    if (!format_ctx_ || !codec_ctx_ || !buffer) return 0;

    int frames_written = 0;

    // First, drain any residual frames from previous decode
    if (residual_frames_ > 0) {
        int to_copy = std::min(residual_frames_, max_frames);
        std::memcpy(buffer, residual_buffer_ + residual_offset_ * channels_,
                    to_copy * channels_ * sizeof(float));
        frames_written += to_copy;
        residual_offset_ += to_copy;
        residual_frames_ -= to_copy;

        if (frames_written >= max_frames) {
            position_frames_ += frames_written;
            return frames_written;
        }
    }

    // Read and decode packets
    while (frames_written < max_frames) {
        int ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            // EOF or error
            break;
        }

        if (packet_->stream_index != audio_stream_index_) {
            av_packet_unref(packet_);
            continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) {
            continue;
        }

        // Receive decoded frames
        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }

            // Convert to float interleaved
            int out_samples = frame_->nb_samples;
            int buffer_size = out_samples * channels_;

            // Allocate temporary buffer for conversion
            float* temp_buffer = new float[buffer_size];
            uint8_t* out_ptr = reinterpret_cast<uint8_t*>(temp_buffer);

            int converted = swr_convert(swr_ctx_, &out_ptr, out_samples,
                                        const_cast<const uint8_t**>(frame_->extended_data),
                                        frame_->nb_samples);

            if (converted > 0) {
                int frames_available = converted;
                int frames_needed = max_frames - frames_written;
                int to_copy = std::min(frames_available, frames_needed);

                std::memcpy(buffer + frames_written * channels_, temp_buffer,
                            to_copy * channels_ * sizeof(float));
                frames_written += to_copy;

                // Store residual if we have extra frames
                if (frames_available > to_copy) {
                    int leftover = frames_available - to_copy;
                    if (residual_buffer_) {
                        delete[] residual_buffer_;
                    }
                    residual_buffer_ = new float[leftover * channels_];
                    std::memcpy(residual_buffer_, temp_buffer + to_copy * channels_,
                                leftover * channels_ * sizeof(float));
                    residual_frames_ = leftover;
                    residual_offset_ = 0;
                }
            }

            delete[] temp_buffer;
            av_frame_unref(frame_);

            if (frames_written >= max_frames) {
                break;
            }
        }

        if (frames_written >= max_frames) {
            break;
        }
    }

    position_frames_ += frames_written;
    return frames_written;
}

bool M4ADecoder::seek(long frame) {
    ouroboros::util::Logger::debug("M4ADecoder: Seeking to frame " + std::to_string(frame));

    if (!format_ctx_ || audio_stream_index_ < 0) return false;

    // Clear residual buffer
    if (residual_buffer_) {
        delete[] residual_buffer_;
        residual_buffer_ = nullptr;
    }
    residual_frames_ = 0;
    residual_offset_ = 0;

    // Convert frame to timestamp
    AVStream* stream = format_ctx_->streams[audio_stream_index_];
    int64_t timestamp = av_rescale_q(frame, {1, sample_rate_}, stream->time_base);

    int ret = av_seek_frame(format_ctx_, audio_stream_index_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ouroboros::util::Logger::error("M4ADecoder: Seek failed");
        return false;
    }

    // Flush codec buffers
    avcodec_flush_buffers(codec_ctx_);

    position_frames_ = frame;
    return true;
}

} // namespace audio

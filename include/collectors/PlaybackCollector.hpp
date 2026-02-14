#include "backend/SnapshotPublisher.hpp"
#include "audio/AudioDecoder.hpp"
#include "audio/PipeWireContext.hpp"
#include "audio/PipeWireOutput.hpp"
#include "events/EventBus.hpp"
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

namespace ouroboros::collectors {

class PlaybackCollector {
public:
    explicit PlaybackCollector(std::shared_ptr<backend::SnapshotPublisher> publisher);
    ~PlaybackCollector();

    void run(std::stop_token stop_token);

private:
    std::shared_ptr<backend::SnapshotPublisher> publisher_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> clear_requested_{false};

    // Persistent Audio Context
    audio::PipeWireContext audio_context_;

    // Event subscriptions (for cleanup)
    std::vector<events::EventBus::SubscriptionId> subscriptions_;

    // Position interpolation state
    std::chrono::steady_clock::time_point anchor_time_;
    int64_t anchor_position_ms_ = 0;
    size_t anchor_consumed_frames_ = 0;
    int anchor_sample_rate_ = 0;
    int64_t frozen_position_ms_ = 0;

    void update_position_anchor(audio::PipeWireOutput& output);
    int64_t get_interpolated_position_ms() const;
    void reset_position_anchor(int64_t position_ms, audio::PipeWireOutput& output);

    std::unique_ptr<audio::AudioDecoder> create_decoder_for_track(const model::Track& track);
    std::string format_to_string(model::AudioFormat format);
};

}  // namespace ouroboros::collectors

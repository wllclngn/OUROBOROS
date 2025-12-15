#include "backend/SnapshotPublisher.hpp"
#include "audio/AudioDecoder.hpp"
#include "audio/PipeWireContext.hpp"
#include <memory>
#include <string>
#include <thread>
#include <atomic>

namespace ouroboros::collectors {

class PlaybackCollector {
public:
    explicit PlaybackCollector(std::shared_ptr<backend::SnapshotPublisher> publisher);
    ~PlaybackCollector() = default;

    void run(std::stop_token stop_token);

private:
    std::shared_ptr<backend::SnapshotPublisher> publisher_;
    std::atomic<bool> paused_{false};
    
    // Persistent Audio Context
    audio::PipeWireContext audio_context_;

    std::unique_ptr<audio::AudioDecoder> create_decoder_for_track(const model::Track& track);
    std::string format_to_string(model::AudioFormat format);
};

}  // namespace ouroboros::collectors

#pragma once

#include <functional>
#include <map>
#include <vector>
#include <string>
#include <mutex>

namespace ouroboros::events {

struct Event {
    enum class Type {
        PlaybackStateChanged,
        VolumeChanged,
        LibraryUpdated,
        SearchQuery,
        TrackChanged,
        QueueUpdated,
        AddTrackToQueue,
        ClearQueue,
        PlayPause,
        NextTrack,
        PrevTrack,
        SeekForward,
        SeekBackward,
        VolumeUp,
        VolumeDown,
        RepeatToggle,
    };
    Type type;
    int index = -1;           // For track selection
    std::string data;         // For misc string data
    int seek_seconds = 5;     // For seeking (default 5s)
    int volume_delta = 5;     // For volume (default 5%)
};

class EventBus {
public:
    using Handler = std::function<void(const Event&)>;
    using SubscriptionId = uint64_t;

    static EventBus& instance() {
        static EventBus instance;
        return instance;
    }

    SubscriptionId subscribe(Event::Type type, Handler handler);
    void unsubscribe(SubscriptionId id);
    void publish(const Event& event);

private:
    EventBus() = default;
    struct Subscription {
        SubscriptionId id;
        Handler handler;
    };
    std::map<Event::Type, std::vector<Subscription>> subscribers_;
    std::mutex mutex_;
    SubscriptionId next_id_ = 1;
};

}  // namespace ouroboros::events
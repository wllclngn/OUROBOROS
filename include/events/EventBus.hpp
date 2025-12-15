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

    static EventBus& instance() {
        static EventBus instance;
        return instance;
    }

    void subscribe(Event::Type type, Handler handler);
    void publish(const Event& event);

private:
    EventBus() = default;
    std::map<Event::Type, std::vector<Handler>> subscribers_;
    std::mutex mutex_;
};

}  // namespace ouroboros::events
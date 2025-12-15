#include "events/EventBus.hpp"
#include "util/Logger.hpp"

namespace ouroboros::events {

void EventBus::subscribe(Event::Type type, Handler handler) {
    ouroboros::util::Logger::debug("EventBus: Subscribing to event type");

    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_[type].push_back(handler);
}

void EventBus::publish(const Event& event) {
    ouroboros::util::Logger::debug("EventBus: Publishing event");

    // Copy handlers to avoid holding lock during execution
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(event.type);
        if (it != subscribers_.end()) {
            handlers = it->second;
        }
    }
    
    // Execute handlers outside the lock
    for (const auto& handler : handlers) {
        handler(event);
    }
}

}  // namespace ouroboros::events
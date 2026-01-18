#include "events/EventBus.hpp"
#include "util/Logger.hpp"
#include <algorithm>

namespace ouroboros::events {

EventBus::SubscriptionId EventBus::subscribe(Event::Type type, Handler handler) {
    ouroboros::util::Logger::debug("EventBus: Subscribing to event type");

    std::lock_guard<std::mutex> lock(mutex_);
    SubscriptionId id = next_id_++;
    subscribers_[type].push_back({id, std::move(handler)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    ouroboros::util::Logger::debug("EventBus: Unsubscribing id " + std::to_string(id));

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [type, subs] : subscribers_) {
        auto it = std::remove_if(subs.begin(), subs.end(),
            [id](const Subscription& s) { return s.id == id; });
        subs.erase(it, subs.end());
    }
}

void EventBus::publish(const Event& event) {
    ouroboros::util::Logger::debug("EventBus: Publishing event");

    // Copy handlers to avoid holding lock during execution
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(event.type);
        if (it != subscribers_.end()) {
            for (const auto& sub : it->second) {
                handlers.push_back(sub.handler);
            }
        }
    }

    // Execute handlers outside the lock
    for (const auto& handler : handlers) {
        handler(event);
    }
}

}  // namespace ouroboros::events
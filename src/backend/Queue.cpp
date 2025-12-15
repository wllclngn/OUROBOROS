#include "backend/Queue.hpp"
#include "util/Logger.hpp"

namespace ouroboros::backend {

Queue::Queue() {}

void Queue::add_track(const model::Track& track) {
    ouroboros::util::Logger::info("Queue: Adding track");

    tracks_.push_back(track);
}

void Queue::add_tracks(const std::vector<model::Track>& tracks) {
    ouroboros::util::Logger::info("Queue: Adding multiple tracks");

    tracks_.insert(tracks_.end(), tracks.begin(), tracks.end());
}

void Queue::remove_track(size_t index) {
    ouroboros::util::Logger::info("Queue: Removing track at index");

    if (index < tracks_.size()) {
        tracks_.erase(tracks_.begin() + index);
    }
}

void Queue::clear() {
    ouroboros::util::Logger::info("Queue: Clearing queue");

    tracks_.clear();
    current_index_ = 0;
}

const std::vector<model::Track>& Queue::get_tracks() const {
    return tracks_;
}

std::optional<model::Track> Queue::get_current_track() const {
    if (current_index_ < tracks_.size()) {
        return tracks_[current_index_];
    }
    return std::nullopt;
}

bool Queue::next() {
    ouroboros::util::Logger::debug("Queue: Moving to next track");

    if (current_index_ + 1 < tracks_.size()) {
        current_index_++;
        return true;
    }
    return false;
}

bool Queue::prev() {
    ouroboros::util::Logger::debug("Queue: Moving to previous track");

    if (current_index_ > 0) {
        current_index_--;
        return true;
    }
    return false;
}

bool Queue::set_current_index(size_t index) {
    ouroboros::util::Logger::info("Queue: Setting current index");

    if (index < tracks_.size()) {
        current_index_ = index;
        return true;
    }
    return false;
}

size_t Queue::get_current_index() const {
    return current_index_;
}

}  // namespace ouroboros::backend

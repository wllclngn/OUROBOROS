#pragma once

#include "model/Snapshot.hpp"
#include <deque>

namespace ouroboros::backend {

class Queue {
public:
    Queue();

    void add_track(const model::Track& track);
    void add_tracks(const std::vector<model::Track>& tracks);
    void remove_track(size_t index);
    void clear();

    const std::vector<model::Track>& get_tracks() const;
    std::optional<model::Track> get_current_track() const;
    
    bool next();
    bool prev();
    bool set_current_index(size_t index);
    size_t get_current_index() const;

private:
    std::vector<model::Track> tracks_;
    size_t current_index_ = 0;
};

}  // namespace ouroboros::backend

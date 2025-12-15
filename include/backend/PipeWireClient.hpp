#pragma once

#include "model/Snapshot.hpp"
#include <memory>

namespace ouroboros::backend {

class PipeWireClient {
public:
    PipeWireClient();
    ~PipeWireClient();

    bool connect(const std::string& server_name = "");
    bool disconnect();
    bool is_connected() const;

    bool play();
    bool pause();
    bool next();
    bool prev();
    bool set_volume(int percent);
    bool seek(int position_ms);

    model::PlayerState get_state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ouroboros::backend

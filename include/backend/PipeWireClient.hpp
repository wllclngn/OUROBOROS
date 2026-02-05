#pragma once

#include "model/Snapshot.hpp"
#include <memory>

namespace ouroboros::backend {

class PipeWireClient {
public:
    PipeWireClient();
    ~PipeWireClient();

    [[nodiscard]] bool connect(const std::string& server_name = "");
    [[nodiscard]] bool disconnect();
    [[nodiscard]] bool is_connected() const;

    [[nodiscard]] bool play();
    [[nodiscard]] bool pause();
    [[nodiscard]] bool next();
    [[nodiscard]] bool prev();
    [[nodiscard]] bool set_volume(int percent);
    [[nodiscard]] bool seek(int position_ms);

    [[nodiscard]] model::PlayerState get_state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ouroboros::backend

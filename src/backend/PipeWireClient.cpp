#include "backend/PipeWireClient.hpp"

namespace ouroboros::backend {

struct PipeWireClient::Impl {
    // PipeWire implementation details
};

PipeWireClient::PipeWireClient() : impl_(std::make_unique<Impl>()) {}
PipeWireClient::~PipeWireClient() = default;

bool PipeWireClient::connect(const std::string&) { return true; }
bool PipeWireClient::disconnect() { return true; }
bool PipeWireClient::is_connected() const { return true; }

bool PipeWireClient::play() { return true; }
bool PipeWireClient::pause() { return true; }
bool PipeWireClient::next() { return true; }
bool PipeWireClient::prev() { return true; }
bool PipeWireClient::set_volume(int) { return true; }
bool PipeWireClient::seek(int) { return true; }

model::PlayerState PipeWireClient::get_state() const {
    return model::PlayerState();
}

}  // namespace ouroboros::backend

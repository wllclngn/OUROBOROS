#include "audio/PipeWireContext.hpp"
#include <pipewire/pipewire.h>
#include "util/Logger.hpp"

namespace audio {

PipeWireContext::PipeWireContext() {
    ouroboros::util::Logger::info("PipeWireContext: Initializing PipeWire context");
}

PipeWireContext::~PipeWireContext() {
    if (loop_) {
        ouroboros::util::Logger::info("PipeWireContext: Stopping thread loop");
        pw_thread_loop_stop(loop_);
        ouroboros::util::Logger::debug("PipeWireContext: Destroying thread loop");
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    // Safe to leave initialized until process exit
    // pw_deinit();
}

bool PipeWireContext::init() {
    if (loop_) {
        ouroboros::util::Logger::debug("PipeWireContext: Already initialized, skipping");
        return true;
    }

    ouroboros::util::Logger::info("PipeWireContext: Calling pw_init()");
    pw_init(nullptr, nullptr);
    ouroboros::util::Logger::debug("PipeWireContext: pw_init() completed successfully");

    ouroboros::util::Logger::info("PipeWireContext: Creating thread loop 'ouroboros-main'");
    loop_ = pw_thread_loop_new("ouroboros-main", nullptr);

    if (loop_) {
        ouroboros::util::Logger::info("PipeWireContext: Thread loop created successfully");
        ouroboros::util::Logger::info("PipeWireContext: Starting thread loop");
        if (pw_thread_loop_start(loop_) < 0) {
            ouroboros::util::Logger::error("PipeWireContext: Failed to start thread loop");
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return false;
        }
        ouroboros::util::Logger::info("PipeWireContext: Thread loop started successfully");
        return true;
    }

    ouroboros::util::Logger::error("PipeWireContext: Failed to create thread loop");
    return false;
}

} // namespace audio

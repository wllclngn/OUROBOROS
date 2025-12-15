#include "audio/PipeWireContext.hpp"
#include <pipewire/pipewire.h>
#include <iostream>

namespace audio {

PipeWireContext::PipeWireContext() {
}

PipeWireContext::~PipeWireContext() {
    if (loop_) {
        pw_thread_loop_stop(loop_);
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    // Safe to leave initialized until process exit
    // pw_deinit(); 
}

bool PipeWireContext::init() {
    if (loop_) return true; // Already initialized
    
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("ouroboros-main", nullptr);
    
    if (loop_) {
        if (pw_thread_loop_start(loop_) < 0) {
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            return false;
        }
        return true;
    }
    return false;
}

} // namespace audio

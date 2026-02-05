#pragma once

struct pw_thread_loop;

namespace audio {

class PipeWireContext {
public:
    PipeWireContext();
    ~PipeWireContext();
    
    [[nodiscard]] bool init();
    struct pw_thread_loop* get_loop() const { return loop_; }
    
private:
    struct pw_thread_loop* loop_ = nullptr;
};

} // namespace audio

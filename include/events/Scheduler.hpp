#pragma once

#include <chrono>
#include <functional>
#include <map>

namespace ouroboros::events {

class Scheduler {
public:
    using Task = std::function<void()>;

    void schedule(const std::string& name, std::chrono::milliseconds interval, Task task);
    void unschedule(const std::string& name);
    void process();

private:
    struct ScheduledTask {
        Task task;
        std::chrono::milliseconds interval;
        std::chrono::steady_clock::time_point last_run;
    };

    std::map<std::string, ScheduledTask> tasks_;
};

}  // namespace ouroboros::events

#include "events/Scheduler.hpp"
#include "util/Logger.hpp"

namespace ouroboros::events {

void Scheduler::schedule(const std::string& name, std::chrono::milliseconds interval, Task task) {
    ouroboros::util::Logger::debug("Scheduler: Scheduling task");

    tasks_[name] = {task, interval, std::chrono::steady_clock::now()};
}

void Scheduler::unschedule(const std::string& name) {
    ouroboros::util::Logger::debug("Scheduler: Unscheduling task");

    tasks_.erase(name);
}

void Scheduler::process() {
    auto now = std::chrono::steady_clock::now();
    for (auto& [name, task] : tasks_) {
        if (now - task.last_run >= task.interval) {
            task.task();
            task.last_run = now;
        }
    }
}

}  // namespace ouroboros::events

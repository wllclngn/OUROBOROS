#pragma once

#include "ui/Component.hpp"
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

#ifdef __linux__
#include <termios.h>
#endif

namespace ouroboros::ui {

class Terminal {
public:
    static Terminal& instance();

    void init();
    void shutdown();
    bool is_initialized() const;

    void clear_screen();
    void move_cursor(int x, int y);
    void print(int x, int y, const std::string& text);
    void flush();

    // Enqueue raw data for asynchronous writing to stdout
    void write_raw(const std::string& text);

    InputEvent read_input();
    int get_terminal_width() const;
    int get_terminal_height() const;

private:
    Terminal();
    ~Terminal();

    void writer_loop();

    bool initialized_ = false;
    int width_ = 80;
    int height_ = 24;

    // Async writer components
    std::thread writer_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> write_queue_;
    std::atomic<bool> running_{false};
    
#ifdef __linux__
    ::termios original_termios_;
#endif
};

}  // namespace ouroboros::ui

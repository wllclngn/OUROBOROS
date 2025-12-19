#include "ui/Terminal.hpp"
#include "util/Logger.hpp"
#include <iostream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <poll.h>
#include <format>
#include <atomic>

namespace ouroboros::ui {

// PHASE #3: Signal Safety
// Use volatile sig_atomic_t for signal flag - NEVER call ioctl in a handler!
static volatile std::sig_atomic_t g_resize_pending = 0;

static void sigwinch_handler(int) {
    g_resize_pending = 1;
}

Terminal& Terminal::instance() {
    static Terminal instance;
    return instance;
}

Terminal::Terminal() {}
Terminal::~Terminal() {
    shutdown();
}

void Terminal::init() {
    if (!initialized_) {
#ifdef __linux__
        tcgetattr(STDIN_FILENO, &original_termios_);
        
        ::termios raw = original_termios_;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_iflag &= ~(IXON | ICRNL); // Disable flow control and CR->NL
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        
        // Install SIGWINCH handler for resize
        std::signal(SIGWINCH, sigwinch_handler);
#endif
        
        // Start writer thread
        running_ = true;
        writer_thread_ = std::thread(&Terminal::writer_loop, this);

        write_raw("\033[?1049h"); // Enter alternate screen buffer
        write_raw("\033[?25l");   // Hide cursor
        initialized_ = true;
    }
}

void Terminal::shutdown() {
    if (initialized_) {
        write_raw("\033[?25h");   // Show cursor
        write_raw("\033[?1049l"); // Exit alternate screen buffer
        
        // Stop writer thread
        running_ = false;
        queue_cv_.notify_all();
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }

#ifdef __linux__
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios_);
#endif
        initialized_ = false;
    }
}

bool Terminal::is_initialized() const {
    return initialized_;
}

void Terminal::writer_loop() {
    while (running_) {
        std::string chunk;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !write_queue_.empty() || !running_; });
            
            if (!write_queue_.empty()) {
                chunk = std::move(write_queue_.front());
                write_queue_.pop_front();
            }
        }

        if (!chunk.empty()) {
            size_t written = 0;
            while (written < chunk.size()) {
                ssize_t n = write(STDOUT_FILENO, chunk.data() + written, chunk.size() - written);
                if (n > 0) {
                    written += n;
                } else if (n < 0) {
                    if (errno == EINTR) continue;
                    
                    // Handle non-blocking stdout (likely sharing status with stdin)
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        struct pollfd pfd = {STDOUT_FILENO, POLLOUT, 0};
                        poll(&pfd, 1, 100); // Wait up to 100ms
                        continue;
                    }

                    // If we error, we can't do much. Just drop it to avoid infinite loop.
                    ouroboros::util::Logger::error("Terminal writer error: " + std::string(strerror(errno)));
                    break;
                }
            }
            if (chunk.size() > 100) {
                // PHASE #3: Modern format for logging
                ouroboros::util::Logger::debug(std::format("Terminal: Wrote {} bytes to stdout", written));
            }
        } else if (!running_ && write_queue_.empty()) {
            break; 
        }
    }
}

void Terminal::write_raw(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        write_queue_.push_back(text);
    }
    queue_cv_.notify_one();
}

void Terminal::clear_screen() {
    write_raw("\033[2J\033[H");
}

void Terminal::move_cursor(int x, int y) {
    // PHASE #3: Performance Optimization
    // Use std::format for efficient string construction.
    // This avoids multiple allocations from the '+' operator used previously.
    write_raw(std::format("\033[{};{}H", y + 1, x + 1));
}

void Terminal::print(int x, int y, const std::string& text) {
    // Construct the full sequence to atomicize the cursor move + text
    // This reduces cursor jumping artifacts and unnecessary intermediate strings
    write_raw(std::format("\033[{};{}H{}", y + 1, x + 1, text));
}

void Terminal::flush() {
    // No-op in async model
}

InputEvent Terminal::read_input() {
    // PHASE #3: Check safe flag
    if (g_resize_pending) {
        g_resize_pending = 0;
        // Signal event loop to re-render
        return {InputEvent::Type::Resize, 0, "resize"};
    }

    char c;
    ssize_t n;

    // Retry on EINTR (signal interruption)
    do {
        n = read(STDIN_FILENO, &c, 1);
    } while (n < 0 && errno == EINTR);

    // Debug: log what read() returned (only on error/weirdness)
    if (n != 1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
             ouroboros::util::Logger::debug(std::format("read_input: read() returned {}, errno={} ({})", n, errno, EAGAIN));
        }
    }

    if (n == 1) {
        if (c == '\033') {
            char seq[3];
            ssize_t n2;
            // Read with EINTR retry
            do {
                n2 = read(STDIN_FILENO, &seq[0], 1);
            } while (n2 < 0 && errno == EINTR);

            if (n2 == 1) {
                if (seq[0] == '[') {
                    ssize_t n3;
                    do {
                        n3 = read(STDIN_FILENO, &seq[1], 1);
                    } while (n3 < 0 && errno == EINTR);

                    if (n3 == 1) {
                        switch (seq[1]) {
                            case 'A': return {InputEvent::Type::KeyPress, 0, "up"};
                            case 'B': return {InputEvent::Type::KeyPress, 0, "down"};
                            case 'C': return {InputEvent::Type::KeyPress, 0, "right"};
                            case 'D': return {InputEvent::Type::KeyPress, 0, "left"};
                        }
                    }
                }
            }
            return {InputEvent::Type::KeyPress, 27, "escape"};
        }
        
        if (c == '\n' || c == '\r') {
            return {InputEvent::Type::KeyPress, c, "enter"};
        }
        
        std::string key_name(1, c);
        return {InputEvent::Type::KeyPress, c, key_name};
    }
    
    return {InputEvent::Type::KeyPress, 0, ""};
}

int Terminal::get_terminal_width() const {
    winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_col;
}

int Terminal::get_terminal_height() const {
    winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_row;
}

}  // namespace ouroboros::ui
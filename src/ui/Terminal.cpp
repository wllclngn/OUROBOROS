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

namespace ouroboros::ui {

static Terminal* g_terminal_instance = nullptr;

static void sigwinch_handler(int) {
    if (g_terminal_instance) {
        winsize w;
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        // Size will be picked up on next get_terminal_width/height call
    }
}

Terminal& Terminal::instance() {
    static Terminal instance;
    g_terminal_instance = &instance;
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

        write_raw("\033[?25l"); // Hide cursor
        initialized_ = true;
    }
}

void Terminal::shutdown() {
    if (initialized_) {
        write_raw("\033[?25h"); // Show cursor
        
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
                ouroboros::util::Logger::debug("Terminal: Wrote " + std::to_string(written) + " bytes to stdout");
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
    write_raw("\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H");
}

void Terminal::print(int x, int y, const std::string& text) {
    // Construct the full sequence to atomicize the cursor move + text
    // This reduces cursor jumping artifacts
    std::string seq = "\033[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H" + text;
    write_raw(seq);
}

void Terminal::flush() {
    // No-op in async model, or could wait for empty queue. 
    // For performance, we rely on the writer thread to keep up.
}

InputEvent Terminal::read_input() {
    char c;
    ssize_t n;

    // Retry on EINTR (signal interruption)
    do {
        n = read(STDIN_FILENO, &c, 1);
    } while (n < 0 && errno == EINTR);

    // Debug: log what read() returned
    if (n != 1) {
        ouroboros::util::Logger::debug("read_input: read() returned " + std::to_string(n) +
                                       ", errno=" + std::to_string(errno) +
                                       " (EAGAIN=" + std::to_string(EAGAIN) + ")");
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

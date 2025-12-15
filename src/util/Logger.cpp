#include "util/Logger.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <mutex>

namespace ouroboros::util {

static std::mutex log_mutex;
static std::ofstream log_file;  // Keep file open for performance

void Logger::init() {
    std::lock_guard<std::mutex> lock(log_mutex);
    // Close existing file if open
    if (log_file.is_open()) {
        log_file.close();
    }
    // Open log file once
    log_file.open("/tmp/ouroboros_debug.log", std::ios::trunc);
}

void Logger::log(Level level, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (!log_file.is_open()) {
        // Fallback: open if not initialized
        log_file.open("/tmp/ouroboros_debug.log", std::ios::app);
    }
    if (!log_file) return;

    // Timestamp
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    log_file << std::put_time(&tm, "[%H:%M:%S] ");

    switch (level) {
        case Level::Debug: log_file << "[DEBUG] "; break;
        case Level::Info:  log_file << "[INFO]  "; break;
        case Level::Warn:  log_file << "[WARN]  "; break;
        case Level::Error: log_file << "[ERROR] "; break;
    }

    log_file << message << std::endl;
    log_file.flush();  // Ensure writes are visible immediately
}

void Logger::debug(const std::string& message) { log(Level::Debug, message); }
void Logger::info(const std::string& message) { log(Level::Info, message); }
void Logger::warn(const std::string& message) { log(Level::Warn, message); }
void Logger::error(const std::string& message) { log(Level::Error, message); }

}  // namespace ouroboros::util

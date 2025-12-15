#pragma once

#include <string>

namespace ouroboros::util {

class Logger {
public:
    enum class Level { Debug, Info, Warn, Error };

    static void init();
    static void log(Level level, const std::string& message);
    static void debug(const std::string& message);
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message);
};

}  // namespace ouroboros::util

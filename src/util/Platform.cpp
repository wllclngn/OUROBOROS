#include "util/Platform.hpp"
#include "util/Logger.hpp"
#include <cstdlib>
#include <algorithm>

namespace ouroboros::util {

std::filesystem::path Platform::get_music_directory() {
    ouroboros::util::Logger::debug("Platform: Detecting music directory");
    auto home = std::getenv("HOME");
    if (home) {
        auto path = std::filesystem::path(home) / "MUSIC";
        if (std::filesystem::exists(path)) {
            ouroboros::util::Logger::info("Platform: Music directory found: " + path.string());
            return path;
        }
        path = std::filesystem::path(home) / "Music";
        ouroboros::util::Logger::info("Platform: Using default music directory: " + path.string());
        return path;
    }
    ouroboros::util::Logger::warn("Platform: HOME env var not set, using fallback: ./Music");
    return "./Music";
}

std::filesystem::path Platform::get_config_directory() {
    ouroboros::util::Logger::debug("Platform: Detecting config directory");
    auto home = std::getenv("HOME");
    if (home) {
        auto path = std::filesystem::path(home) / ".config" / "ouroboros";
        ouroboros::util::Logger::info("Platform: Config directory: " + path.string());
        return path;
    }
    ouroboros::util::Logger::warn("Platform: HOME env var not set, using fallback: .config/ouroboros");
    return ".config/ouroboros";
}

std::filesystem::path Platform::get_cache_directory() {
    ouroboros::util::Logger::debug("Platform: Detecting cache directory");
    auto home = std::getenv("HOME");
    if (home) {
        auto path = std::filesystem::path(home) / ".cache" / "ouroboros";
        ouroboros::util::Logger::info("Platform: Cache directory: " + path.string());
        return path;
    }
    ouroboros::util::Logger::warn("Platform: HOME env var not set, using fallback: .cache/ouroboros");
    return ".cache/ouroboros";
}

bool Platform::is_audio_file(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    static const std::string extensions[] = {
        ".mp3", ".flac", ".ogg", ".wav", ".m4a", ".aac", ".wma"
    };

    for (const auto& e : extensions) {
        if (ext == e) return true;
    }
    return false;
}

std::string Platform::get_audio_format(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (!ext.empty() && ext[0] == '.') {
        return ext.substr(1);
    }
    return "";
}

}  // namespace ouroboros::util

#include "util/Platform.hpp"
#include <cstdlib>
#include <algorithm>

namespace ouroboros::util {

std::filesystem::path Platform::get_music_directory() {
    auto home = std::getenv("HOME");
    if (home) {
        auto path = std::filesystem::path(home) / "MUSIC";
        if (std::filesystem::exists(path)) {
            return path;
        }
        return std::filesystem::path(home) / "Music";
    }
    return "./Music";
}

std::filesystem::path Platform::get_config_directory() {
    auto home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "ouroboros";
    }
    return ".config/ouroboros";
}

std::filesystem::path Platform::get_cache_directory() {
    auto home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".cache" / "ouroboros";
    }
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

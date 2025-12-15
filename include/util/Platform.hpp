#pragma once

#include <filesystem>

namespace ouroboros::util {

class Platform {
public:
    static std::filesystem::path get_music_directory();
    static std::filesystem::path get_config_directory();
    static std::filesystem::path get_cache_directory();

    static bool is_audio_file(const std::filesystem::path& path);
    static std::string get_audio_format(const std::filesystem::path& path);
};

}  // namespace ouroboros::util

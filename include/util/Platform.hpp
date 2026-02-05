#pragma once

#include <filesystem>
#include <cassert>

namespace ouroboros::util {

// Checked narrowing cast - documents intent and catches overflow in debug builds
template<typename To, typename From>
constexpr To narrow_cast(From value) noexcept {
    auto result = static_cast<To>(value);
    assert(static_cast<From>(result) == value && "narrowing conversion lost data");
    return result;
}


class Platform {
public:
    [[nodiscard]] static std::filesystem::path get_music_directory();
    [[nodiscard]] static std::filesystem::path get_config_directory();
    [[nodiscard]] static std::filesystem::path get_cache_directory();

    [[nodiscard]] static bool is_audio_file(const std::filesystem::path& path);
    [[nodiscard]] static std::string get_audio_format(const std::filesystem::path& path);
};

}  // namespace ouroboros::util

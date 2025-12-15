#pragma once

#include <cstdint>

namespace ouroboros::ui {

enum class Color : uint32_t {
    Default = 0,
    Black = 1,
    Red = 2,
    Green = 3,
    Yellow = 4,
    Blue = 5,
    Magenta = 6,
    Cyan = 7,
    White = 8,
    
    // Bright/Bold variants
    BrightBlack = 9,
    BrightRed = 10,
    BrightGreen = 11,
    BrightYellow = 12,
    BrightBlue = 13,
    BrightMagenta = 14,
    BrightCyan = 15,
    BrightWhite = 16
};

enum class Attribute : uint8_t {
    None = 0,
    Bold = 1 << 0,
    Dim = 1 << 1,
    Underline = 1 << 2,
    Blink = 1 << 3,
    Reverse = 1 << 4,
    Hidden = 1 << 5
};

inline Attribute operator|(Attribute a, Attribute b) {
    return static_cast<Attribute>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline bool has_attribute(Attribute set, Attribute check) {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(check)) != 0;
}

struct Style {
    Color fg = Color::Default;
    Color bg = Color::Default;
    Attribute attr = Attribute::None;

    bool operator==(const Style& other) const = default;
};

} // namespace ouroboros::ui

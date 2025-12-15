#pragma once

#include <string>

namespace ouroboros::ui {

struct InputEvent {
    enum class Type {
        KeyPress,
        Resize,
        Mouse
    };

    Type type;
    int key; // char code or special key code
    std::string key_name; // "up", "down", "enter", "a", "B", etc.

    bool is_key(const std::string& name_to_check) const {
        return type == Type::KeyPress && key_name == name_to_check;
    }
};

} // namespace ouroboros::ui

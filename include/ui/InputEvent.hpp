#pragma once

#include <string>
#include "backend/Config.hpp"

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

// Check if an input event matches a keybind from TOML config
// Keybind values: "q", "space", "tab", "right", "left", "+", "-", "?", "ctrl+a", etc.
inline bool matches_keybind(const InputEvent& event, const std::string& keybind_name) {
    const auto& keybinds = backend::Config::instance().keybinds;
    auto it = keybinds.find(keybind_name);
    if (it == keybinds.end()) return false;

    const std::string& bind = it->second;
    if (bind.empty()) return false;

    // Handle Ctrl+ combinations
    if (bind.size() > 5 && bind.substr(0, 5) == "ctrl+") {
        char ctrl_char = bind[5];
        int ctrl_code = ctrl_char - 'a' + 1;  // ctrl+a = 1, ctrl+d = 4, etc.
        return event.key == ctrl_code;
    }

    // Handle special key names (space, tab, right, left, etc.)
    if (bind == "space") return event.key_name == "space" || event.key == ' ';
    if (bind == "tab") return event.key_name == "tab" || event.key == '\t' || event.key == 9;
    if (bind == "right") return event.key_name == "right";
    if (bind == "left") return event.key_name == "left";
    if (bind == "up") return event.key_name == "up";
    if (bind == "down") return event.key_name == "down";
    if (bind == "enter") return event.key_name == "enter" || event.key == '\r' || event.key == '\n';
    if (bind == "escape") return event.key_name == "escape" || event.key == 27;

    // Single character keybinds
    if (bind.size() == 1) {
        char c = bind[0];
        // Case-sensitive matching for single characters
        return event.key == c;
    }

    return false;
}

} // namespace ouroboros::ui

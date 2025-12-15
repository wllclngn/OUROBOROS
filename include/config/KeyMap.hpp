#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace ouroboros::config {

class KeyMap {
public:
    KeyMap();

    void load_default_keybinds();
    void add_binding(const std::string& action, const std::string& key_sequence);
    std::string lookup_action(const std::string& key_sequence) const;

private:
    std::unordered_map<std::string, std::string> bindings_;
};

}  // namespace ouroboros::config

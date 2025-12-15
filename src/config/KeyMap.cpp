#include "config/KeyMap.hpp"

namespace ouroboros::config {

KeyMap::KeyMap() {
    load_default_keybinds();
}

void KeyMap::load_default_keybinds() {
    bindings_["space"] = "play_pause";
    bindings_["n"] = "next";
    bindings_["N"] = "prev";
    bindings_["q"] = "quit";
    bindings_["+"] = "volume_up";
    bindings_["-"] = "volume_down";
}

void KeyMap::add_binding(const std::string& action, const std::string& key_sequence) {
    bindings_[key_sequence] = action;
}

std::string KeyMap::lookup_action(const std::string& key_sequence) const {
    auto it = bindings_.find(key_sequence);
    if (it != bindings_.end()) {
        return it->second;
    }
    return "";
}

}  // namespace ouroboros::config

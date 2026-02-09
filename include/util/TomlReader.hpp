#pragma once

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace ouroboros::util {

class TomlReader {
public:
  bool load(const std::string& path) {
    sections_.clear();
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string current_section;
    std::string line;
    while (std::getline(in, line)) {
      auto sv = trim(line);
      if (sv.empty() || sv[0] == '#') continue;
      if (sv.front() == '[' && sv.back() == ']') {
        current_section = std::string(sv.substr(1, sv.size() - 2));
        trim_inplace(current_section);
        ensure_section(current_section);
        continue;
      }
      auto eq = sv.find('=');
      if (eq == std::string_view::npos) continue;
      std::string key(trim(sv.substr(0, eq)));
      std::string val(trim(sv.substr(eq + 1)));
      // Strip surrounding quotes from string values
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
        val = val.substr(1, val.size() - 2);
      ensure_section(current_section).set(key, val);
    }
    return true;
  }

  bool save(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) return false;
    bool first = true;
    for (const auto& [name, sec] : sections_) {
      if (!first) out << '\n';
      first = false;
      if (!name.empty()) out << '[' << name << "]\n";
      for (const auto& [k, v] : sec.entries) {
        if (needs_quoting(v))
          out << k << " = \"" << v << "\"\n";
        else
          out << k << " = " << v << '\n';
      }
    }
    return out.good();
  }

  [[nodiscard]] std::string get_string(std::string_view section, std::string_view key,
                                        const std::string& def = "") const {
    const auto* s = find_section(section);
    return s ? s->get(key, def) : def;
  }

  [[nodiscard]] int get_int(std::string_view section, std::string_view key, int def = 0) const {
    const auto* s = find_section(section);
    if (!s) return def;
    auto val = s->get(key, "");
    if (val.empty()) return def;
    try { return std::stoi(val); } catch (...) { return def; }
  }

  [[nodiscard]] bool get_bool(std::string_view section, std::string_view key, bool def = false) const {
    const auto* s = find_section(section);
    if (!s) return def;
    auto val = s->get(key, "");
    if (val.empty()) return def;
    if (val == "true" || val == "True" || val == "TRUE" || val == "1") return true;
    if (val == "false" || val == "False" || val == "FALSE" || val == "0") return false;
    return def;
  }

  void set(const std::string& section, const std::string& key, const std::string& value) {
    ensure_section(section).set(key, value);
  }

  void set(const std::string& section, const std::string& key, int value) {
    ensure_section(section).set(key, std::to_string(value));
  }

  void set(const std::string& section, const std::string& key, bool value) {
    ensure_section(section).set(key, value ? "true" : "false");
  }

  [[nodiscard]] bool has(std::string_view section, std::string_view key) const {
    const auto* s = find_section(section);
    return s && s->has(key);
  }

private:
  struct Section {
    std::vector<std::pair<std::string, std::string>> entries;

    [[nodiscard]] std::string get(std::string_view key, const std::string& def) const {
      for (const auto& [k, v] : entries)
        if (k == key) return v;
      return def;
    }

    void set(const std::string& key, const std::string& val) {
      for (auto& [k, v] : entries) {
        if (k == key) { v = val; return; }
      }
      entries.emplace_back(key, val);
    }

    [[nodiscard]] bool has(std::string_view key) const {
      for (const auto& [k, v] : entries)
        if (k == key) return true;
      return false;
    }
  };

  std::vector<std::pair<std::string, Section>> sections_;

  Section& ensure_section(const std::string& name) {
    for (auto& [n, s] : sections_)
      if (n == name) return s;
    sections_.emplace_back(name, Section{});
    return sections_.back().second;
  }

  [[nodiscard]] const Section* find_section(std::string_view name) const {
    for (const auto& [n, s] : sections_)
      if (n == name) return &s;
    return nullptr;
  }

  static std::string_view trim(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return sv;
  }

  static void trim_inplace(std::string& s) {
    auto sv = trim(std::string_view(s));
    s = std::string(sv);
  }

  static bool needs_quoting(const std::string& val) {
    if (val.empty()) return true;
    if (val == "true" || val == "false") return false;
    size_t start = (val[0] == '-') ? 1 : 0;
    bool all_digits = (start < val.size());
    for (size_t i = start; i < val.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(val[i]))) { all_digits = false; break; }
    }
    if (all_digits) return false;
    return true;
  }
};

} // namespace ouroboros::util

#include "config.hpp"

#include <cstdlib>
#include <fstream>

#include "default_config.hpp"

namespace ghost {

fs::path home_dir() {
  const char* h = std::getenv("HOME");
  return h ? fs::path(h) : fs::path("/tmp");
}

fs::path ghost_home() { return home_dir() / ".ghostctl"; }

fs::path config_path() {
  const char* x = std::getenv("XDG_CONFIG_HOME");
  fs::path base = (x && *x) ? fs::path(x) : home_dir() / ".config";
  return base / "ghostctl" / "config.toml";
}

fs::path expand_user(const std::string& p) {
  if (p.starts_with("~/")) return home_dir() / p.substr(2);
  if (p == "~") return home_dir();
  return p;
}

static void merge(toml::table& base, const toml::table& over) {
  for (auto&& [k, v] : over) {
    if (auto* sub = v.as_table(); sub && base.contains(k) && base[k].is_table())
      merge(*base[k].as_table(), *sub);
    else
      base.insert_or_assign(k, v);
  }
}

Config Config::load() {
  Config c;
  c.data_ = toml::parse(DEFAULT_TOML);
  auto path = config_path();
  if (fs::exists(path)) {
    try {
      merge(c.data_, toml::parse_file(path.string()));
    } catch (const toml::parse_error& e) {
      c.error = path.string() + ": " + std::string(e.description()) + " (using defaults)";
    }
  }
  return c;
}

bool Config::write_default() {
  auto path = config_path();
  if (fs::exists(path)) return false;
  fs::create_directories(path.parent_path());
  std::ofstream(path) << DEFAULT_TOML;
  return true;
}

std::string Config::str(std::string_view section, std::string_view key) const {
  if (auto v = data_[section][key].value<std::string>()) return *v;
  return {};
}

bool Config::flag(std::string_view section, std::string_view key) const {
  return data_[section][key].value_or(false);
}

double Config::num(std::string_view section, std::string_view key) const {
  if (auto v = data_[section][key].value<double>()) return *v;
  if (auto v = data_[section][key].value<int64_t>()) return double(*v);
  return 0;
}

std::vector<std::string> Config::keys(std::string_view action) const {
  std::vector<std::string> out;
  std::string all = str("keys", action);
  size_t start = 0;
  while (start <= all.size()) {
    size_t comma = all.find(',', start);
    std::string k = all.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    while (!k.empty() && k.front() == ' ') k.erase(k.begin());
    while (!k.empty() && k.back() == ' ') k.pop_back();
    if (!k.empty()) out.push_back(k);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

}  // namespace ghost

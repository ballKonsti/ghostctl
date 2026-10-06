// User configuration: ~/.config/ghostctl/config.toml merged over DEFAULT_TOML.
#pragma once

#include <filesystem>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

namespace ghost {

namespace fs = std::filesystem;

fs::path home_dir();
fs::path ghost_home();   // ~/.ghostctl
fs::path config_path();  // $XDG_CONFIG_HOME/ghostctl/config.toml
fs::path expand_user(const std::string& p);

class Config {
 public:
  static Config load();
  // Create the config file from the defaults if it's missing. True if created.
  static bool write_default();

  std::string str(std::string_view section, std::string_view key) const;
  bool flag(std::string_view section, std::string_view key) const;
  double num(std::string_view section, std::string_view key) const;
  std::vector<std::string> keys(std::string_view action) const;  // [keys] split on ','
  fs::path profile_dir() const { return expand_user(str("browser", "profile_dir")); }

  std::string error;  // problem reading the user file (shown in the UI)

 private:
  toml::table data_;
};

}  // namespace ghost

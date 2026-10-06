// Optional saved login in the system keyring (Secret Service via libsecret) —
// never in a file. Compatible with what the Python version stored with `keyring`.
#pragma once

#include <optional>
#include <string>

namespace ghost::creds {

struct Creds {
  std::string username, password;
  bool operator==(const Creds&) const = default;
};

bool available();
std::optional<Creds> load();
bool save(const Creds& c);
bool forget();

}  // namespace ghost::creds

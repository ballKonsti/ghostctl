// Optional saved login in the system keyring (Secret Service via libsecret) —
// never in a file. Uses the same attributes as Python's `keyring`, so logins saved by
// the old Python version (and other keyring tools) are found.
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

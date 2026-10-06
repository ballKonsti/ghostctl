// Log in by relaying Snapchat's login screens to a UI (terminal or TUI).
// Saved credentials answer the username/password questions once; the password
// is only typed into Snapchat's own form. Screens that need a human (an
// interactive captcha, an unknown step) are handed to a visible window.
#pragma once

#include <optional>
#include <string>

#include "browser.hpp"
#include "creds.hpp"

namespace ghost {

class LoginUI {
 public:
  virtual ~LoginUI() = default;
  virtual std::optional<std::string> ask(const std::string& prompt, bool secret) = 0;
  virtual void say(const std::string& text) = 0;
  virtual bool confirm(const std::string& question) = 0;
};

class TerminalLoginUI : public LoginUI {
 public:
  std::optional<std::string> ask(const std::string& prompt, bool secret) override;
  void say(const std::string& text) override;
  bool confirm(const std::string& question) override;
};

// The browser must already be on Snapchat Web. Returns the login that worked
// (for "remember me"), or nullopt.
std::optional<creds::Creds> run_login(Browser& b, LoginUI& ui, std::optional<creds::Creds> saved = std::nullopt);

}  // namespace ghost

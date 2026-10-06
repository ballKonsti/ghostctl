#include "login.hpp"

#include <termios.h>
#include <unistd.h>

#include <iostream>
#include <tuple>

#include "util.hpp"

namespace ghost {

constexpr double STUCK_AFTER = 45;  // seconds on an unrecognised screen before offering the window
constexpr int TYPE_DELAY_MS = 70;   // per keystroke, human-ish

// --- terminal UI ---

std::optional<std::string> TerminalLoginUI::ask(const std::string& prompt, bool secret) {
  std::cout << prompt << ": " << std::flush;
  termios old{};
  bool hide = secret && isatty(0) && tcgetattr(0, &old) == 0;
  if (hide) {
    termios t = old;
    t.c_lflag &= ~ECHO;
    tcsetattr(0, TCSANOW, &t);
  }
  std::string line;
  bool ok = bool(std::getline(std::cin, line));
  if (hide) {
    tcsetattr(0, TCSANOW, &old);
    std::cout << "\n";
  }
  if (!ok) return std::nullopt;
  return trim(line);
}

void TerminalLoginUI::say(const std::string& text) { std::cout << "  snapchat: " << text << "\n"; }

bool TerminalLoginUI::confirm(const std::string& question) {
  auto a = ask(question + " [Y/n]", false);
  return a && !(a->starts_with("n") || a->starts_with("N"));
}

// --- flow ---

namespace {

using Sig = std::tuple<std::string, json, json, bool>;  // url, headings, messages, password visible

Sig signature(Browser& b) {
  return {b.url(), b.helper("texts", {sel::ACC_HEADING.css}), b.helper("texts", {sel::ACC_MESSAGE.css}),
          b.visible(sel::ACC_PASSWORD.css)};
}

void type_into(Browser& b, std::string_view css, const std::string& value) {
  b.click(css);
  b.key("Control+A");
  b.key("Backspace");
  b.type_slow(value, TYPE_DELAY_MS);
}

void submit(Browser& b) {
  if (!b.click(sel::ACC_SUBMIT.css)) b.key("Enter");
}

void wait_change(Browser& b, const Sig& before, double timeout = 20) {
  double deadline = now_epoch() + timeout;
  while (now_epoch() < deadline && !b.closed()) {
    sleep_ms(1000);
    if (b.probe() == Session::LoggedIn) return;
    try {
      if (signature(b) != before) return;
    } catch (const CdpError&) {
    }
  }
}

bool hand_over(Browser& b, LoginUI& ui, const std::string& reason) {
  if (!ui.confirm(reason + " Open a browser window to finish there?")) return false;
  bool was_headless = b.mode() == Mode::Headless;
  b.close();
  b.open_headed();
  ui.say("Finish logging in in the window; it closes by itself afterwards.");
  bool ok = false;
  while (!b.closed()) {
    if (b.session_state(5) == Session::LoggedIn) {
      ok = true;
      sleep_ms(3000);  // let the session save
      break;
    }
  }
  b.close();
  if (was_headless) b.start();
  return ok;
}

bool on_accounts(const std::string& url) {
  return url.find(std::string("//") + std::string(sel::ACCOUNTS_HOST)) != std::string::npos;
}

}  // namespace

std::optional<creds::Creds> run_login(Browser& b, LoginUI& ui, std::optional<creds::Creds> saved) {
  bool used_user = false, used_pw = false;
  std::string username, password;
  std::pair<json, json> shown;
  double last_change = now_epoch();
  Sig last_sig;

  auto result = [&]() -> std::optional<creds::Creds> { return creds::Creds{username, password}; };

  while (!b.closed()) {
    Session state = b.probe();
    if (state == Session::LoggedIn) return result();
    if (state == Session::Blocked) {
      ui.say("Snapchat refused this browser (\"Browser not supported\").");
      return std::nullopt;
    }
    if (b.visible(sel::COOKIE_ESSENTIAL)) {
      b.click(sel::COOKIE_ESSENTIAL.css);
      sleep_ms(1000);
      continue;
    }
    Sig sig = signature(b);
    if (sig != last_sig) last_sig = sig, last_change = now_epoch();
    bool accounts = on_accounts(std::get<0>(sig));

    // Pass on what Snapchat says (errors, instructions) once per screen.
    std::pair<json, json> now{std::get<1>(sig), std::get<2>(sig)};
    if (accounts && now != shown) {
      shown = now;
      for (auto* arr : {&now.first, &now.second})
        for (auto& line : *arr)
          if (line.get<std::string>() != username) ui.say(line);
    }

    if (b.visible(sel::ACC_VERIFYING)) {
      if (now_epoch() - last_change > STUCK_AFTER)
        return hand_over(b, ui, "The security check needs a human (captcha).") ? result() : std::nullopt;
      sleep_ms(1000);
      continue;
    }

    bool landing = b.visible(sel::LOGIN_FORM);
    bool pw_box = accounts && b.visible(sel::ACC_PASSWORD.css);
    bool acc_user = accounts && !pw_box && b.visible(sel::ACC_USERNAME);

    auto get_username = [&]() -> std::optional<std::string> {
      // With given credentials, a second username prompt means they were
      // rejected: give up so the caller can show Snapchat's message on its form.
      if (saved) {
        if (used_user) return std::nullopt;
        used_user = true;
        return saved->username;
      }
      return ui.ask("Username or email", false);
    };

    if (landing || acc_user) {
      auto u = get_username();
      if (!u || u->empty()) return std::nullopt;
      username = *u;
      type_into(b, landing ? sel::LOGIN_FORM.css : sel::ACC_USERNAME.css, username);
      if (landing)
        b.click(sel::LOGIN_SUBMIT.css);
      else
        submit(b);
      wait_change(b, sig);
    } else if (pw_box) {
      std::optional<std::string> pw;
      if (saved) {
        if (used_pw) return std::nullopt;  // wrong password: back to the caller's form
        used_pw = true;
        pw = saved->password;
      } else {
        pw = ui.ask("Password", true);
      }
      if (!pw || pw->empty()) return std::nullopt;
      password = *pw;
      type_into(b, sel::ACC_PASSWORD.css, password);
      submit(b);
      wait_change(b, sig);
    } else if (accounts && b.visible(sel::ACC_OTHER_INPUT.css)) {
      auto label = b.eval("(() => { const el = window.__gc.first(" + json(sel::ACC_OTHER_INPUT.css).dump() +
                          "); return el ? ((el.labels && el.labels[0] && el.labels[0].innerText) || "
                          "el.getAttribute('aria-label') || el.placeholder || el.name || 'Code') : 'Code'; })()");
      auto v = ui.ask(trim(label.get<std::string>()), false);
      if (!v || v->empty()) return std::nullopt;
      type_into(b, sel::ACC_OTHER_INPUT.css, *v);
      submit(b);
      wait_change(b, sig);
    } else {
      if (now_epoch() - last_change > STUCK_AFTER)
        return hand_over(b, ui, "ghostctl doesn't recognise this login screen.") ? result() : std::nullopt;
      sleep_ms(1000);
    }
  }
  return std::nullopt;
}

}  // namespace ghost

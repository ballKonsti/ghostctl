// ghostctl: terminal client for Snapchat Web (C++ version).
//   ghostctl                  the TUI (shows a login screen if logged out)
//   ghostctl login [--window|--remember]
//   ghostctl logout | forget | check | config [--edit] | inspect
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

#include "bridge.hpp"
#include "browser.hpp"
#include "config.hpp"
#include "creds.hpp"
#include "emoji.hpp"
#include "inspect.hpp"
#include "login.hpp"
#include "ui/app.hpp"
#include "ui/wtext.hpp"
#include "util.hpp"

using namespace ghost;

static int cmd_check() {
  Browser b(Config::load());
  auto st = b.start();
  std::printf("mode:    %s\nsession: %s\n", to_string(st.mode), to_string(st.session));
  if (!st.note.empty()) std::printf("note:    %s\n", st.note.c_str());
  b.close();
  return st.session == Session::LoggedIn ? 0 : 1;
}

static int remember_now() {
  // Save a login without logging in; it's checked the next time Snapchat asks
  // ghostctl to log in (wrong details just bring up the login form).
  if (!creds::available()) {
    std::puts("No system keyring available (Secret Service); nothing saved.");
    return 1;
  }
  TerminalLoginUI ui;
  auto user = ui.ask("Username or email", false);
  auto pw = ui.ask("Password (stored in your keyring)", true);
  if (!user || !pw || user->empty() || pw->empty()) return 1;
  std::puts(creds::save({*user, *pw}) ? "Saved to the keyring." : "Couldn't write to the keyring.");
  return 0;
}

static int cmd_login(bool remember) {
  Browser b(Config::load());
  auto st = b.start();
  if (st.session == Session::LoggedIn) {
    std::puts("Already logged in; the session is saved in the profile.");
    b.close();
    if (remember) return remember_now();
    std::puts("To save your login for automatic re-login: `ghostctl login --remember`.");
    std::puts("To switch accounts: `ghostctl logout` first.");
    return 0;
  }
  TerminalLoginUI ui;
  std::puts("Logging in to Snapchat Web (no browser window; your password is typed");
  std::puts("into Snapchat's own form).");
  std::optional<creds::Creds> got;
  if (auto saved = creds::load()) {
    std::printf("Using the saved login for %s...\n", saved->username.c_str());
    got = run_login(b, ui, saved);
    if (!got) {
      std::puts("The saved login didn't work; enter your details.");
      b.goto_web();
    }
  }
  if (!got) got = run_login(b, ui);
  if (!got) {
    std::puts("Login not completed.");
    b.close();
    return 1;
  }
  std::puts("Logged in. Saving session...");
  sleep_ms(3000);  // let cookies/IndexedDB flush to the profile
  b.close();
  if (!got->password.empty() && creds::available() && creds::load() != got &&
      ui.confirm("Remember this login in your system keyring, so ghostctl can log back in by itself if Snapchat "
                 "ever logs you out?"))
    std::puts(creds::save(*got) ? "Saved to the keyring." : "Couldn't write to the keyring.");
  std::puts("Done. Run `ghostctl`.");
  return 0;
}

static int cmd_login_window() {
  Browser b(Config::load());
  b.open_headed();
  std::puts("A browser window is open on Snapchat Web. Log in there yourself\n"
            "(QR code, 2FA, captcha, whatever it asks). ghostctl never sees your password.\n"
            "Waiting for login... (close the window to abort)");
  while (!b.closed()) {
    if (b.session_state(5) == Session::LoggedIn) {
      std::puts("Logged in. Saving session...");
      sleep_ms(3000);
      b.close();
      std::puts("Done. Run `ghostctl`.");
      return 0;
    }
  }
  std::puts("Window closed before login was detected.");
  return 1;
}

static int cmd_logout() {
  auto profile = Config::load().profile_dir();
  std::printf("Log out: delete the saved login and the session in %s? [y/N] ", profile.c_str());
  std::string a;
  std::getline(std::cin, a);
  if (a.empty() || (a[0] != 'y' && a[0] != 'Y')) return 1;
  creds::forget();
  std::error_code ec;
  fs::remove_all(profile, ec);
  std::puts("Logged out. Run `ghostctl` to log in again.");
  return 0;
}

static int cmd_config(bool edit) {
  bool created = Config::write_default();
  std::printf("%s%s\n", created ? "Created " : "", config_path().c_str());
  if (!edit) return 0;
  const char* ed = std::getenv("VISUAL");
  if (!ed || !*ed) ed = std::getenv("EDITOR");
  std::string cmd = std::string(ed && *ed ? ed : "nano") + " '" + config_path().string() + "'";
  return std::system(cmd.c_str());
}

static void usage() {
  std::puts(
      "usage: ghostctl [command]\n\n"
      "  (none)              start the TUI (shows a login screen if you're logged out)\n"
      "  login               log in from the terminal (username, password, 2FA code)\n"
      "  login --remember    already logged in: save your login for automatic re-login\n"
      "  login --window      log in yourself in a visible browser window\n"
      "  logout              delete the session and the saved login\n"
      "  forget              delete the saved login from the keyring (stay logged in)\n"
      "  check               verify the saved session\n"
      "  config [--edit]     create/edit ~/.config/ghostctl/config.toml\n"
      "  inspect             dump the live page to debug/ to fix selectors");
}

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  std::string opt = argc > 2 ? argv[2] : "";
  try {
    if (cmd.empty()) {
      auto cfg = Config::load();
      ui::App app(cfg);
      if (!cfg.error.empty()) std::fprintf(stderr, "%s\n", cfg.error.c_str());
      return app.run();
    }
    if (cmd == "login") return opt == "--window" ? cmd_login_window() : cmd_login(opt == "--remember");
    if (cmd == "logout") return cmd_logout();
    if (cmd == "forget") return std::puts(creds::forget() ? "Deleted the saved login from the keyring." : "No saved login."), 0;
    if (cmd == "check") return cmd_check();
    if (cmd == "config") return cmd_config(opt == "--edit");
    if (cmd == "inspect") return run_inspect(Config::load());
    if (cmd == "-h" || cmd == "--help" || cmd == "help") return usage(), 0;
    if (cmd == "debug-menu") {  // developer check: target a message and locate its body
      Browser b(Config::load());
      b.start();
      std::mutex mu;
      Conversation conv;
      Bridge br(b, [&](BridgeEvent ev) {
        std::lock_guard lk(mu);
        if (ev.kind == BridgeEvent::Conv) conv = ev.conv;
      });
      br.start();
      br.open_chat(opt);
      sleep_ms(1500);
      std::string key;
      {
        std::lock_guard lk(mu);
        for (auto& it : conv.items)
          if (it.kind == ConvItem::Msg && !it.msg.text.empty()) key = it.msg.key;
      }
      std::printf("key: %s\n", key.c_str());
      std::printf("target: %s\n", b.eval("window.__ghostctl.target(" + json(key).dump() + ")").dump().c_str());
      std::printf("tagged: %s\n", b.eval("[...document.querySelectorAll('[data-ghostctl-target]')].map(e => e.tagName + ' children=' + [...e.children].map(c => c.tagName + ':' + Math.round(c.getBoundingClientRect().width) + 'x' + Math.round(c.getBoundingClientRect().height)).join(','))").dump().c_str());
      std::printf("all: %s\n", b.eval("window.__gc.all('[data-ghostctl-target] > div').length").dump().c_str());
      std::printf("visible: %s\n", b.eval("window.__gc.all('[data-ghostctl-target] > div').map(e => [window.__gc.visible(e), getComputedStyle(e).opacity, getComputedStyle(e).visibility])").dump().c_str());
      std::printf("point: %s\n", b.helper("point", {"[data-ghostctl-target] > div"}).dump().c_str());
      std::printf("doc: %s\n", b.eval("({vis: document.visibilityState, focus: document.hasFocus(), raf: new Promise(r => { const t = setTimeout(() => r('raf TIMEOUT'), 1000); requestAnimationFrame(() => { clearTimeout(t); r('raf ok'); }); })})").dump().c_str());
      std::printf("raf: %s\n", b.eval("new Promise(r => { const t = setTimeout(() => r('raf TIMEOUT'), 1000); requestAnimationFrame(() => { clearTimeout(t); r('raf ok'); }); })").dump().c_str());
      sleep_ms(1000);
      std::printf("visible after 1s: %s\n", b.eval("window.__gc.all('[data-ghostctl-target] > div').map(e => getComputedStyle(e).visibility)").dump().c_str());
      b.close();
      return 0;
    }
    if (cmd == "debug-emoji") {  // developer check of emoji search ranking
      for (int i = 2; i < argc; ++i) {
        std::printf("%-8s ->", argv[i]);
        for (auto& [e, n] : emoji::search(argv[i], 8)) std::printf(" %s", e.c_str());
        std::printf("\n");
      }
      return 0;
    }
    if (cmd == "debug-width") {  // developer check of emoji widths
      for (int i = 2; i < argc; ++i) std::printf("%s -> %d cells\n", argv[i], ui::display_width(argv[i]));
      return 0;
    }
    usage();
    return 2;
  } catch (const ProfileLocked& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

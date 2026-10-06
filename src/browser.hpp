// Owns Chromium: one persistent profile, headless with a hidden-window fallback.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "cdp.hpp"
#include "config.hpp"
#include "selectors.hpp"

namespace ghost {

enum class Session { LoggedIn, LoggedOut, Blocked, Unknown };
enum class Mode { Headless, HeadedOffscreen, Headed };

const char* to_string(Session s);
const char* to_string(Mode m);

struct Status {
  Mode mode;
  Session session;
  std::string note;
};

struct Point {
  double x = 0, y = 0, w = 0, h = 0;
};

class Browser {
 public:
  explicit Browser(const Config& cfg);
  Browser(fs::path profile, std::string headless_pref, std::string user_agent, std::string exe);
  ~Browser();

  // Normal run: headless; a hidden window only if Snapchat refuses headless.
  Status start();
  // Visible window (login --window, inspect, captcha hand-over).
  void open_headed();
  void close();
  bool closed() const { return !cdp_ || !cdp_->alive(); }
  Mode mode() const { return mode_; }

  void goto_url(std::string_view url);
  void goto_web() { goto_url(sel::WEB_URL); }
  std::string url();

  Session probe();
  // Wait until the same recognisable state shows twice in a row (or time out).
  Session session_state(double timeout_s = 20);

  // --- JavaScript ---
  // Evaluate an expression in the page (promises awaited); returns its JSON value.
  json eval(const std::string& expr, std::chrono::milliseconds timeout = 30s);
  // Call window.__gc.<fn>(...args) from helpers.js (re-installed if missing).
  json helper(const std::string& fn, json args = json::array());
  bool visible(const sel::Sel& s) { return helper("isVisible", {s.css}).get<bool>(); }
  bool visible(std::string_view css) { return helper("isVisible", {css}).get<bool>(); }
  int count(std::string_view css) { return helper("count", {css}).get<int>(); }
  std::optional<Point> point(std::string_view css, int nth = 0);

  // Called (on the CDP reader thread) when page.js pushes data.
  void on_binding(std::function<void(const std::string& payload)> cb);
  // Called when the browser process goes away.
  void on_closed(std::function<void()> cb);

  // --- input ---
  void mouse(const char* type, double x, double y, const char* button = "left", int clicks = 1);
  void click_at(double x, double y, const char* button = "left", int hold_ms = 0);
  // Click the centre of the nth visible match. False if nothing visible matched.
  bool click(std::string_view css, int nth = 0, const char* button = "left");
  void key(std::string_view combo);  // "Enter", "Escape", "Backspace", "Control+A", "Shift+Enter"
  void insert_text(const std::string& text);
  void type_slow(const std::string& text, int delay_ms = 70);
  void set_files(std::string_view css, const std::string& path);
  std::string screenshot_png(const Point& clip);
  void grant_camera();

  std::string page_session() const { return session_; }
  Cdp& cdp() { return *cdp_; }

 private:
  void launch(Mode mode);
  void attach();
  std::string find_exe() const;
  std::string user_agent();
  static std::string note(Session s);

  fs::path profile_;
  std::string headless_pref_, user_agent_, exe_;
  std::unique_ptr<Cdp> cdp_;
  std::string session_;
  Mode mode_ = Mode::Headless;
  std::mutex cb_mu_;
  std::function<void(const std::string&)> binding_cb_;
  std::function<void()> closed_cb_;
};

}  // namespace ghost

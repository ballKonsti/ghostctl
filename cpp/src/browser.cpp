#include "browser.hpp"

#include <algorithm>
#include <regex>

#include "helpers_js.hpp"
#include "util.hpp"

namespace ghost {

const char* to_string(Session s) {
  switch (s) {
    case Session::LoggedIn: return "logged in";
    case Session::LoggedOut: return "logged out";
    case Session::Blocked: return "blocked";
    default: return "unknown";
  }
}

const char* to_string(Mode m) {
  switch (m) {
    case Mode::Headless: return "headless";
    case Mode::HeadedOffscreen: return "headed (off-screen)";
    default: return "headed";
  }
}

Browser::Browser(const Config& cfg)
    : Browser(cfg.profile_dir(), cfg.str("browser", "headless"), cfg.str("browser", "user_agent"),
              cfg.str("browser", "executable")) {}

Browser::Browser(fs::path profile, std::string headless_pref, std::string user_agent, std::string exe)
    : profile_(std::move(profile)),
      headless_pref_(headless_pref.empty() ? "auto" : std::move(headless_pref)),
      user_agent_(std::move(user_agent)),
      exe_(std::move(exe)) {}

Browser::~Browser() { close(); }

std::string Browser::find_exe() const {
  if (!exe_.empty()) return expand_user(exe_).string();
  // Playwright's Chromium (newest build first): what the Python version used.
  fs::path cache = home_dir() / ".cache" / "ms-playwright";
  std::vector<fs::path> found;
  std::error_code ec;
  for (auto& d : fs::directory_iterator(cache, ec)) {
    auto name = d.path().filename().string();
    if (!name.starts_with("chromium-")) continue;
    for (auto sub : {"chrome-linux64/chrome", "chrome-linux/chrome"})
      if (fs::exists(d.path() / sub)) found.push_back(d.path() / sub);
  }
  std::sort(found.begin(), found.end());
  if (!found.empty()) return found.back().string();
  for (auto n : {"chromium", "chromium-browser", "google-chrome", "google-chrome-stable"})
    if (on_path(n)) return n;
  throw CdpError("No Chromium found. Run `uv run playwright install chromium` or set [browser] executable.");
}

void Browser::launch(Mode mode) {
  close();
  fs::create_directories(profile_);
  std::vector<std::string> args = {
      "--user-data-dir=" + profile_.string(),
      "--no-first-run",
      "--no-default-browser-check",
      "--password-store=basic",
      "--use-mock-keychain",
      "--no-sandbox",
      "--disable-background-timer-throttling",
      "--disable-backgrounding-occluded-windows",
      "--disable-renderer-backgrounding",
      "--disable-features=Translate,MediaRouter,DialMediaRouteProvider,OptimizationHints",
      "--lang=en-US",
      "--accept-lang=en-US",
  };
  if (mode == Mode::Headless) {
    args.push_back("--headless=new");
    args.push_back("--window-size=1280,900");
  } else if (mode == Mode::HeadedOffscreen) {
    // Forced onto X11: Wayland compositors ignore requested window positions.
    args.push_back("--ozone-platform=x11");
    args.push_back("--window-position=-32000,-32000");
    args.push_back("--window-size=1280,900");
  }
  args.push_back("about:blank");

  cdp_ = std::make_unique<Cdp>();
  cdp_->set_event_handler([this](const std::string& method, const json& params, const std::string&) {
    if (method == "Runtime.bindingCalled" && params.value("name", "") == "__ghostctl_bind") {
      std::function<void(const std::string&)> cb;
      {
        std::lock_guard lk(cb_mu_);
        cb = binding_cb_;
      }
      if (cb) cb(params.value("payload", ""));
    } else if (method == "ghostctl.closed" || method == "Inspector.detached") {
      std::function<void()> cb;
      {
        std::lock_guard lk(cb_mu_);
        cb = closed_cb_;
      }
      if (cb) cb();
    }
  });
  cdp_->launch(find_exe(), args);
  try {
    cdp_->call("Browser.getVersion", json::object(), "", 20s);
  } catch (const CdpError&) {
    std::string err = cdp_->stderr_text();
    cdp_->close();
    if (err.find("existing browser session") != std::string::npos ||
        err.find("ProcessSingleton") != std::string::npos || err.find("SingletonLock") != std::string::npos)
      throw ProfileLocked("Profile " + profile_.string() +
                          " is in use by another ghostctl or Chromium process. Close it and retry.");
    throw CdpError("Chromium failed to start: " + err.substr(0, 400));
  }
  mode_ = mode;
  attach();
}

std::string Browser::user_agent() {
  if (!user_agent_.empty()) return user_agent_;
  // Snapchat refuses "HeadlessChrome"; present the same browser as normal Chrome.
  auto v = cdp_->call("Browser.getVersion");
  std::string ua = v.value("userAgent", "");
  auto pos = ua.find("HeadlessChrome");
  if (pos != std::string::npos) ua.replace(pos, 14, "Chrome");
  return ua;
}

void Browser::attach() {
  auto targets = cdp_->call("Target.getTargets");
  std::string target_id;
  for (auto& t : targets["targetInfos"])
    if (t.value("type", "") == "page") {
      target_id = t["targetId"];
      break;
    }
  if (target_id.empty()) target_id = cdp_->call("Target.createTarget", {{"url", "about:blank"}})["targetId"];
  session_ = cdp_->call("Target.attachToTarget", {{"targetId", target_id}, {"flatten", true}})["sessionId"];

  cdp_->call("Page.enable", {}, session_);
  cdp_->call("Runtime.enable", {}, session_);
  // Like Playwright: the page believes it has focus and is active even when
  // headless/hidden (Snapchat only reveals message bodies on a focused page).
  try {
    cdp_->call("Emulation.setFocusEmulationEnabled", {{"enabled", true}}, session_);
    cdp_->call("Page.setWebLifecycleState", {{"state", "active"}}, session_);
  } catch (const CdpError&) {
  }
  cdp_->call("Emulation.setUserAgentOverride",
             {{"userAgent", user_agent()}, {"acceptLanguage", "en-US,en;q=0.9"}, {"platform", "Linux"}}, session_);
  try {
    cdp_->call("Emulation.setLocaleOverride", {{"locale", "en-US"}}, session_);
  } catch (const CdpError&) {
  }
  if (mode_ != Mode::Headed)
    cdp_->call("Emulation.setDeviceMetricsOverride",
               {{"width", 1280}, {"height", 900}, {"deviceScaleFactor", 1}, {"mobile", false}}, session_);
  cdp_->call("Page.addScriptToEvaluateOnNewDocument", {{"source", std::string(js::helpers)}}, session_);
  cdp_->call("Runtime.addBinding", {{"name", "__ghostctl_bind"}}, session_);
}

void Browser::close() {
  if (cdp_) {
    cdp_->set_event_handler(nullptr);
    cdp_->close();
    cdp_.reset();
  }
  session_.clear();
}

Status Browser::start() {
  Session state = Session::Unknown;
  if (headless_pref_ != "never") {
    launch(Mode::Headless);
    goto_web();
    state = session_state();
    // Logged out is a real answer (log in headless); only a refusal or an
    // unrecognised page is a reason to try a window.
    if (state == Session::LoggedIn || state == Session::LoggedOut || headless_pref_ == "always")
      return {Mode::Headless, state, state == Session::LoggedIn ? "" : note(state)};
  }
  launch(Mode::HeadedOffscreen);
  goto_web();
  Session s2 = session_state();
  std::string n = note(s2);
  if (n.empty() && headless_pref_ != "never")
    n = std::string("Headless was ") + to_string(state) + "; using a hidden window.";
  return {Mode::HeadedOffscreen, s2, n};
}

std::string Browser::note(Session s) {
  if (s == Session::LoggedOut) return "Not logged in.";
  if (s == Session::LoggedIn) return "";
  return std::string("Could not confirm login (") + to_string(s) + "). Selector '" +
         std::string(sel::LOGGED_IN_MARKER.name) + "' may be stale; run `ghostctl inspect`.";
}

void Browser::open_headed() {
  launch(Mode::Headed);
  goto_web();
}

void Browser::goto_url(std::string_view url) {
  cdp_->call("Page.navigate", {{"url", url}}, session_);
  for (int i = 0; i < 300; ++i) {
    try {
      auto st = eval("document.readyState", 5s);
      if (st == "interactive" || st == "complete") return;
    } catch (const CdpError&) {
      // mid-navigation: the old context is gone
    }
    sleep_ms(100);
  }
}

std::string Browser::url() {
  auto u = eval("location.href");
  return u.is_string() ? u.get<std::string>() : "";
}

json Browser::eval(const std::string& expr, std::chrono::milliseconds timeout) {
  if (!cdp_) throw CdpError("browser is not running");
  auto r = cdp_->call("Runtime.evaluate",
                      {{"expression", expr}, {"returnByValue", true}, {"awaitPromise", true}}, session_, timeout);
  if (r.contains("exceptionDetails")) {
    auto& ex = r["exceptionDetails"];
    std::string msg = ex.contains("exception") ? ex["exception"].value("description", "") : "";
    if (msg.empty()) msg = ex.value("text", "script error");
    throw CdpError("page script: " + msg.substr(0, 300));
  }
  return r["result"].value("value", json());
}

json Browser::helper(const std::string& fn, json args) {
  std::string call = "(window.__gc ? window.__gc." + fn + "(..." + args.dump() + ") : '__nogc__')";
  json r = eval(call);
  if (r.is_string() && r.get<std::string>() == "__nogc__") {
    eval(std::string(js::helpers));
    r = eval(call);
  }
  return r;
}

std::optional<Point> Browser::point(std::string_view css, int nth) {
  auto p = helper("point", {css, nth});
  if (!p.is_object()) return std::nullopt;
  return Point{p["x"], p["y"], p["w"], p["h"]};
}

Session Browser::probe() {
  if (closed()) return Session::Unknown;
  try {
    if (visible(sel::BLOCKED_TEXT)) return Session::Blocked;
    if (visible(sel::LOGGED_IN_MARKER)) return Session::LoggedIn;
    if (visible(sel::LOGIN_HEADING) || visible(sel::LOGIN_FORM)) return Session::LoggedOut;
  } catch (const CdpError&) {
    // mid-navigation
  }
  return Session::Unknown;
}

Session Browser::session_state(double timeout_s) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(timeout_s * 1000));
  Session last = Session::Unknown;
  while (std::chrono::steady_clock::now() < deadline && !closed()) {
    Session s = probe();
    if (s != Session::Unknown && s == last) return s;
    last = s;
    sleep_ms(1000);
  }
  return Session::Unknown;
}

void Browser::on_binding(std::function<void(const std::string&)> cb) {
  std::lock_guard lk(cb_mu_);
  binding_cb_ = std::move(cb);
}

void Browser::on_closed(std::function<void()> cb) {
  std::lock_guard lk(cb_mu_);
  closed_cb_ = std::move(cb);
}

void Browser::mouse(const char* type, double x, double y, const char* button, int clicks) {
  cdp_->call("Input.dispatchMouseEvent",
             {{"type", type}, {"x", x}, {"y", y}, {"button", button}, {"clickCount", clicks}, {"buttons", 1}},
             session_);
}

void Browser::click_at(double x, double y, const char* button, int hold_ms) {
  cdp_->call("Input.dispatchMouseEvent", {{"type", "mouseMoved"}, {"x", x}, {"y", y}}, session_);
  mouse("mousePressed", x, y, button);
  if (hold_ms) sleep_ms(hold_ms);
  mouse("mouseReleased", x, y, button);
}

bool Browser::click(std::string_view css, int nth, const char* button) {
  auto p = point(css, nth);
  if (!p) return false;
  click_at(p->x, p->y, button);
  return true;
}

void Browser::key(std::string_view combo) {
  int modifiers = 0;
  std::string k(combo);
  for (auto [prefix, bit] : {std::pair{"Control+", 2}, {"Shift+", 8}, {"Alt+", 1}}) {
    if (k.starts_with(prefix)) {
      modifiers |= bit;
      k = k.substr(std::string_view(prefix).size());
    }
  }
  struct K {
    const char* key;
    const char* code;
    int vk;
    const char* text;
  };
  static const std::pair<const char*, K> named[] = {
      {"Enter", {"Enter", "Enter", 13, "\r"}},     {"Escape", {"Escape", "Escape", 27, ""}},
      {"Backspace", {"Backspace", "Backspace", 8, ""}}, {"Tab", {"Tab", "Tab", 9, ""}},
      {"Delete", {"Delete", "Delete", 46, ""}},
  };
  json down, up;
  bool found = false;
  for (auto& [name, d] : named) {
    if (k == name) {
      down = {{"type", *d.text && !modifiers ? "keyDown" : "rawKeyDown"}, {"key", d.key}, {"code", d.code},
              {"windowsVirtualKeyCode", d.vk}, {"modifiers", modifiers}};
      if (*d.text && !(modifiers & 2)) down["text"] = d.text;
      found = true;
    }
  }
  if (!found && k.size() == 1) {
    char c = k[0];
    char upper = char(std::toupper((unsigned char)c));
    down = {{"type", modifiers & 2 ? "rawKeyDown" : "keyDown"},
            {"key", std::string(1, c)},
            {"code", std::string("Key") + upper},
            {"windowsVirtualKeyCode", int(upper)},
            {"modifiers", modifiers}};
    if (!(modifiers & 2)) down["text"] = std::string(1, c);
    // Linux Chromium doesn't map ctrl+a to select-all by itself over CDP.
    if ((modifiers & 2) && (c == 'a' || c == 'A')) down["commands"] = {"selectAll"};
    found = true;
  }
  if (!found) throw CdpError("unknown key: " + std::string(combo));
  up = down;
  up["type"] = "keyUp";
  up.erase("text");
  up.erase("commands");
  cdp_->call("Input.dispatchKeyEvent", down, session_);
  cdp_->call("Input.dispatchKeyEvent", up, session_);
}

void Browser::insert_text(const std::string& text) { cdp_->call("Input.insertText", {{"text", text}}, session_); }

void Browser::type_slow(const std::string& text, int delay_ms) {
  // One key event per UTF-8 character.
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    std::string ch = text.substr(i, len);
    i += len;
    cdp_->call("Input.dispatchKeyEvent", {{"type", "keyDown"}, {"key", ch}, {"text", ch}}, session_);
    cdp_->call("Input.dispatchKeyEvent", {{"type", "keyUp"}, {"key", ch}}, session_);
    sleep_ms(delay_ms);
  }
}

void Browser::set_files(std::string_view css, const std::string& path) {
  helper("count", {css});  // make sure helpers exist
  json q = css;
  auto r = cdp_->call("Runtime.evaluate",
                      {{"expression", "window.__gc.first(" + q.dump() + ", false)"}, {"returnByValue", false}},
                      session_);
  if (!r["result"].contains("objectId")) throw CdpError("file input not found: " + std::string(css));
  cdp_->call("DOM.setFileInputFiles", {{"files", {path}}, {"objectId", r["result"]["objectId"]}}, session_);
}

std::string Browser::screenshot_png(const Point& clip) {
  auto r = cdp_->call("Page.captureScreenshot",
                      {{"format", "png"},
                       {"clip", {{"x", clip.x}, {"y", clip.y}, {"width", clip.w}, {"height", clip.h}, {"scale", 1}}}},
                      session_);
  return b64decode(r.value("data", ""));
}

void Browser::grant_camera() {
  cdp_->call("Browser.grantPermissions",
             {{"permissions", {"videoCapture", "audioCapture"}}, {"origin", "https://www.snapchat.com"}});
}

}  // namespace ghost

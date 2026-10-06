#include "ui/app.hpp"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/screen/string.hpp>

#include "creds.hpp"
#include "emoji.hpp"
#include "login.hpp"
#include "media.hpp"
#include "ui/modals.hpp"
#include "ui/wtext.hpp"
#include "util.hpp"

namespace ghost::ui {

using namespace ftxui;

namespace {

Event key_event(const std::string& spec) {
  std::string s = lower(spec);
  if (s == "enter" || s == "return") return Event::Return;
  if (s == "escape" || s == "esc") return Event::Escape;
  if (s == "tab") return Event::Tab;
  if (s == "space") return Event::Character(" ");
  if (s == "up") return Event::ArrowUp;
  if (s == "down") return Event::ArrowDown;
  if (s == "left") return Event::ArrowLeft;
  if (s == "right") return Event::ArrowRight;
  if (s == "slash") return Event::Character("/");
  if (s == "question_mark") return Event::Character("?");
  if (s == "backspace") return Event::Backspace;
  if (s.starts_with("ctrl+") && s.size() == 6) return Event::Special(std::string(1, char(s[5] - 'a' + 1)));
  return Event::Character(spec);  // a single letter, case kept ("C" = shift+c)
}

std::string fit(const std::string& s, int width) { return fit_width(s, width); }

// Uppercase ASCII and Latin-1 letters (ä -> Ä), leave everything else.
std::string upper_case(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    if (c < 0x80) {
      out += char(std::toupper(c));
      ++i;
    } else if (c == 0xC3 && i + 1 < s.size()) {  // U+00C0..U+00FF
      unsigned char d = s[i + 1];
      unsigned cp = 0xC0 + (d & 0x3F);
      if (cp >= 0xE0 && cp != 0xF7 && cp != 0xFF) d = (unsigned char)(d - 0x20);
      out += char(c);
      out += char(d);
      i += 2;
    } else {
      out += char(c);
      ++i;
    }
  }
  return out;
}

class AppLoginUI : public LoginUI {
 public:
  explicit AppLoginUI(App& app) : app_(app) {}
  std::optional<std::string> ask(const std::string& prompt, bool secret) override {
    return app_.ask(prompt, secret, messages);
  }
  void say(const std::string& text) override {
    messages.push_back(text);
    app_.post([this, text] { app_.status("Snapchat: " + text); });
  }
  bool confirm(const std::string& q) override { return app_.confirm(q); }
  std::vector<std::string> messages;

 private:
  App& app_;
};

}  // namespace

App::App(Config cfg)
    : cfg_(std::move(cfg)),
      theme_(theme_named(cfg_.str("ui", "theme"))),
      protocol_(img::choose(cfg_.str("images", "protocol"))) {}

App::~App() = default;

// --- plumbing ---

void App::post(std::function<void()> fn) {
  if (quitting_ || !screen_) return;
  screen_->Post(std::move(fn));
  screen_->PostEvent(Event::Custom);
}

void App::bg(std::function<void()> fn) {
  ++tasks_;
  std::thread([this, fn = std::move(fn)] {
    try {
      fn();
    } catch (const std::exception& e) {
      std::string msg = e.what();
      bool action = dynamic_cast<const ActionError*>(&e) != nullptr;
      post([this, msg, action] {
        status("✗ " + msg, true);
        if (action) notify(msg, true);
      });
    }
    --tasks_;
  }).detach();
}

void App::push(std::shared_ptr<Modal> m) { modals_.push_back(std::move(m)); }

void App::pop(Modal* m) {
  modals_.erase(std::remove_if(modals_.begin(), modals_.end(), [m](auto& x) { return x.get() == m; }),
                modals_.end());
}

void App::notify(const std::string& msg, bool warn) { toasts_.push_back({msg, warn, now_epoch() + 5}); }

void App::status(const std::string& msg, bool error) {
  status_ = msg;
  status_error_ = error;
}

void App::fail(const std::exception& e) {
  std::string msg = e.what();
  status("✗ " + msg.substr(0, msg.find('\n')), true);
  if (dynamic_cast<const ActionError*>(&e)) notify(msg, true);
}

std::string key_label(const std::string& spec) {
  static const std::pair<const char*, const char*> names[] = {
      {"slash", "/"}, {"question_mark", "?"}, {"escape", "esc"}, {"enter", "enter"}, {"down", "↓"},
      {"up", "↑"},    {"left", "←"},          {"right", "→"},    {"space", "space"}, {"tab", "tab"}};
  for (auto [k, v] : names)
    if (spec == k) return v;
  return spec;
}

bool App::key(const Event& e, const std::string& action) const {
  for (auto& k : cfg_.keys(action))
    if (e == key_event(k)) return true;
  return false;
}

std::optional<std::string> App::ask(const std::string& prompt, bool secret, std::vector<std::string> messages) {
  auto p = std::make_shared<std::promise<std::optional<std::string>>>();
  auto f = p->get_future();
  post([this, p, prompt, secret, messages] {
    push(std::make_shared<PromptModal>(prompt, secret, messages, [p](auto v) { p->set_value(v); }));
  });
  return f.get();
}

bool App::confirm(const std::string& question, const std::string& yes) {
  auto p = std::make_shared<std::promise<bool>>();
  auto f = p->get_future();
  post([this, p, question, yes] {
    push(std::make_shared<ConfirmModal>(question, yes, [p](bool v) { p->set_value(v); }));
  });
  return f.get();
}

Element App::dialog(Element body, int width) const {
  return body | size(WIDTH, EQUAL, width) | borderStyled(ROUNDED, theme_.primary) | bgcolor(theme_.surface) |
         color(theme_.fg) | clear_under;
}

Element App::hint(const std::string& k, const std::string& label) const {
  return hbox({wtext(" " + k + " ") | bold | color(theme_.primary), wtext(label.empty() ? "" : label + " ") |
                                                                        color(theme_.dim)});
}

std::string App::chat_name(const std::string& id) const {
  auto it = chats_.find(id);
  return it == chats_.end() ? "this chat" : it->second.name;
}

// --- main loop ---

int App::run(bool connect_now) {
  screen_ = std::make_unique<ftxui::App>(ftxui::App::Fullscreen());
  auto root = CatchEvent(Renderer([this] { return render(); }), [this](Event e) { return on_event(e); });
  Loop loop(screen_.get(), root);
  if (connect_now) connect();
  // Ticker: expire toasts and keep "5m ago" fresh.
  std::thread([this] {
    while (!quitting_) {
      for (int i = 0; i < 10 && !quitting_; ++i) sleep_ms(100);
      post([] {});
    }
  }).detach();
  while (!loop.HasQuitted()) {
    loop.RunOnceBlocking();
    flush_images();
  }
  quitting_ = true;
  std::cout << img::kitty_delete_all() << std::flush;
  stop_voice();
  // Closing the browser makes pending page calls fail fast; then let threads finish.
  if (bridge_) bridge_->stop();
  if (browser_) browser_->close();
  for (int i = 0; i < 50 && tasks_ > 0; ++i) sleep_ms(100);
  return 0;
}

void App::flush_images() {
  if (protocol_ != img::Protocol::Kitty) return;
  const Modal* top = modals_.empty() ? nullptr : modals_.back().get();
  if (!top || !top->image) {
    if (shown_modal_) {
      std::cout << img::kitty_delete(1) << std::flush;
      shown_modal_ = nullptr;
    }
    return;
  }
  const Box& b = top->image_box;
  int bw = b.x_max - b.x_min + 1, bh = b.y_max - b.y_min + 1;
  if (bw < 2 || bh < 2) return;
  auto [cols, rows] = img::fit_cells(top->image->w, top->image->h, bw, bh);
  int col = b.x_min + (bw - cols) / 2, row = b.y_min + (bh - rows) / 2;
  bool same_box = shown_box_.x_min == col && shown_box_.y_min == row && shown_box_.x_max == cols &&
                  shown_box_.y_max == rows;
  std::string out;
  if (top != shown_modal_ || top->image_version != shown_version_ || !same_box)
    out += img::kitty_transmit(*top->image, 1, cols, rows);
  out += img::kitty_put(1, col, row, cols, rows);
  std::cout << out << std::flush;
  shown_modal_ = top;
  shown_version_ = top->image_version;
  shown_box_ = {col, cols, row, rows};
}

// --- connection ---

void App::connect() {
  status("Starting browser…");
  bg([this] {
    browser_ = std::make_unique<Browser>(cfg_);
    Status st;
    try {
      st = browser_->start();
    } catch (const ProfileLocked& e) {
      std::string msg = e.what();
      post([this, msg] { status(msg, true); });
      return;
    }
    if (st.session == Session::LoggedOut) {
      if (!log_in()) {
        post([this] { status("Not logged in. Press q to quit, or restart ghostctl to try again.", true); });
        return;
      }
    } else if (st.session != Session::LoggedIn) {
      post([this, n = st.note] { status(n, true); });
      return;
    }
    std::string note = std::string(to_string(st.mode)) + " · " + img::name(protocol_);
    std::string warn = st.note;
    post([this, note, warn] {
      mode_note_ = note;
      if (!warn.empty()) notify(warn);
      status("Loading chats…");
    });
    bridge_ = std::make_unique<Bridge>(
        *browser_, [this](BridgeEvent ev) { post([this, ev = std::move(ev)]() mutable { on_bridge(std::move(ev)); }); },
        cfg_.num("behavior", "action_gap"));
    bridge_->start();
    post([this] {
      connected_ = true;
      live_ = bridge_->live();
      status("");
    });
    watch_session();
  });
}

bool App::log_in(std::string error) {
  AppLoginUI ui(*this);
  std::optional<creds::Creds> got;
  if (auto saved = creds::load()) {
    post([this, u = saved->username] { status("Logging in as " + u + " (saved login)…"); });
    got = run_login(*browser_, ui, saved);
  }
  while (!got) {
    auto p = std::make_shared<std::promise<std::optional<LoginForm>>>();
    auto f = p->get_future();
    post([this, p, error] {
      push(std::make_shared<LoginModal>(creds::available(), error, [p](auto v) { p->set_value(v); }));
    });
    auto form = f.get();
    if (!form) return false;
    post([this, u = form->user] { status("Logging in as " + u + "…"); });
    got = run_login(*browser_, ui, creds::Creds{form->user, form->password});
    if (!got) {
      error = ui.messages.empty() ? "Login didn't complete." : ui.messages.back();
      browser_->goto_web();
      continue;
    }
    if (form->remember && !got->password.empty()) {
      bool ok = creds::save(*got);
      post([this, ok] { notify(ok ? "Login saved to your keyring." : "Couldn't write to the keyring.", !ok); });
    }
  }
  post([this] { status("Logged in. Saving session…"); });
  sleep_ms(3000);  // let the session reach the profile
  return true;
}

void App::watch_session() {
  // If Snapchat ends the session, log back in (saved login or the login screen).
  while (!quitting_) {
    for (int i = 0; i < 1200 && !quitting_; ++i) sleep_ms(100);
    if (quitting_ || !browser_ || browser_->closed()) return;
    if (browser_->probe() != Session::LoggedOut) continue;
    post([this] { notify("Snapchat logged you out — logging back in…", true); });
    if (!log_in()) {
      post([this] { status("Logged out. Restart ghostctl to log in.", true); });
      return;
    }
    browser_->goto_web();
    bridge_->start();
    post([this] { status(""); });
  }
}

void App::on_bridge(BridgeEvent ev) {
  if (ev.kind == BridgeEvent::Feed) {
    notify_new(ev.feed);
    for (auto& c : ev.feed) chats_[c.id] = c;  // the list is virtualized: merge what is rendered
    feed_seen_ = true;
  } else if (ev.kind == BridgeEvent::Conv) {
    if (ev.conv.id == open_id_) {
      conv_ = std::move(ev.conv);
      settle_pending();
      if (follow_end_ || selected_msg_.empty() || !conv_->find(selected_msg_)) {
        auto msgs = messages();
        if (!msgs.empty()) selected_msg_ = msgs.back()->key;
      }
    }
  } else {
    status("page script error: " + ev.error, true);
  }
}

void App::notify_new(const std::vector<Chat>& rows) {
  if (!feed_seen_) return;
  for (auto& c : rows) {
    auto it = chats_.find(c.id);
    if (!c.unread() || c.id == open_id_) continue;
    if (it != chats_.end() && it->second.status == c.status && it->second.time == c.time) continue;
    if (cfg_.flag("notifications", "bell")) std::cout << '\a' << std::flush;
    if (cfg_.flag("notifications", "status_line")) {
      auto st = status_style(c.status, theme_);
      notify(std::string(st.icon) + " " + c.status + " from " + c.name);
    }
    if (cfg_.flag("notifications", "desktop")) media::desktop_notify(c.name, c.status);
  }
}

// --- rendering ---

Element App::render() {
  // Expire toasts.
  double now = now_epoch();
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [now](auto& t) { return t.until < now; }),
                toasts_.end());
  // The topmost full-screen modal replaces the main view; dialogs float above.
  int full = -1;
  for (int i = int(modals_.size()) - 1; i >= 0; --i)
    if (modals_[i]->fullscreen()) {
      full = i;
      break;
    }
  Element base = full >= 0 ? modals_[full]->render(*this) : render_main();
  Elements layers = {base};
  for (int i = full + 1; i < int(modals_.size()); ++i)
    layers.push_back(vbox({filler(), hbox({filler(), modals_[i]->render(*this), filler()}), filler()}));
  if (!toasts_.empty()) {
    Elements ts;
    for (auto& t : toasts_)
      ts.push_back(wtext(" " + t.text + " ") | color(t.warn ? theme_.warn : theme_.fg) |
                   borderStyled(ROUNDED, t.warn ? theme_.warn : theme_.panel) | bgcolor(theme_.surface) | clear_under);
    layers.push_back(vbox({filler(), hbox({filler(), vbox(std::move(ts))}), wtext(""), wtext("")}));
  }
  return dbox(std::move(layers));
}

Element App::render_main() {
  int width = std::max(24, int(cfg_.num("ui", "chat_list_width")));
  Element right = open_id_.empty() ? render_empty() : render_conv();
  return vbox({
             render_topbar(),
             hbox({render_chats() | size(WIDTH, EQUAL, width) | bgcolor(theme_.surface),
                   separatorStyled(HEAVY) | color(theme_.panel), right | flex}) |
                 flex,
             wtext(" " + status_) | color(status_error_ ? theme_.red : theme_.dim) | bgcolor(theme_.surface),
             render_footer(),
         }) |
         bgcolor(theme_.bg) | color(theme_.fg);
}

Element App::render_topbar() {
  int unread = 0;
  for (auto& [id, c] : chats_) unread += c.unread();
  Elements right;
  if (unread) right.push_back(wtext(" " + std::to_string(unread) + " new ") | bold | color(Color::Black) |
                              bgcolor(theme_.red));
  right.push_back(wtext("  "));
  if (connected_) {
    right.push_back(wtext("● ") | color(live_ ? theme_.green : theme_.primary));
    right.push_back(wtext(live_ ? "live" : "polling") | color(theme_.dim));
  } else {
    right.push_back(wtext("◌ connecting") | color(theme_.dim));
  }
  if (!mode_note_.empty()) right.push_back(wtext("  ·  " + mode_note_) | color(theme_.dim));
  right.push_back(wtext(" "));
  return hbox({wtext(" 👻 "), wtext("ghostctl") | bold | color(theme_.primary), filler(), hbox(std::move(right))}) |
         bgcolor(theme_.surface);
}

std::vector<const Chat*> App::visible_chats() const {
  std::vector<const Chat*> v;
  std::string f = lower(search_.value());
  for (auto& [id, c] : chats_)
    if (f.empty() || lower(c.name).find(f) != std::string::npos) v.push_back(&c);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->time > b->time; });
  return v;
}

Element App::render_chat_row(const Chat& c, bool selected) {
  auto st = status_style(c.status, theme_);
  int width = std::max(24, int(cfg_.num("ui", "chat_list_width"))) - 2;
  std::string when = ago(c.time);
  std::string name = fit(c.name, width - 4 - display_width(when));
  auto line1 = hbox({wtext(" "), wtext(std::string(st.icon) + " ") | bold | color(st.color),
                     wtext(name) | (c.unread() ? bold : nothing), filler(),
                     wtext(when + " ") | (c.unread() ? bold | color(st.color) : color(theme_.dim))});
  Element row;
  if (cfg_.flag("ui", "compact_chat_list")) {
    row = line1;
  } else {
    Elements l2 = {wtext("   "), wtext(c.status.empty() ? " " : c.status) | (st.bold ? bold | color(st.color) : color(theme_.dim))};
    if (!c.streak().empty() && cfg_.flag("ui", "show_streaks")) {
      std::string s = c.streak();
      s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
      l2.push_back(wtext("  " + s));
    }
    if (!c.badge.empty() && cfg_.flag("ui", "show_badges")) l2.push_back(wtext(" " + c.badge));
    if (c.group && cfg_.flag("ui", "show_group_tag")) l2.push_back(wtext("  group") | color(theme_.dim));
    row = vbox({line1, hbox(std::move(l2)), wtext("")});
  }
  if (selected) row = row | bgcolor(focus_ == Focus::Chats ? theme_.boost : theme_.panel) | focus;
  return row;
}

Element App::render_chats() {
  int unread = 0;
  for (auto& [id, c] : chats_) unread += c.unread();
  Elements head = {wtext(" Chats") | bold};
  if (unread) head.push_back(wtext("  " + std::to_string(unread)) | bold | color(theme_.red));
  Elements col = {hbox(std::move(head))};
  if (search_open_)
    col.push_back(search_.render(focus_ == Focus::Search, theme_.fg, theme_.dim) |
                  borderStyled(ROUNDED, focus_ == Focus::Search ? theme_.primary : theme_.panel));
  Elements rows;
  auto list = visible_chats();
  if (!list.empty() && (selected_chat_.empty() ||
                        std::none_of(list.begin(), list.end(), [&](auto* c) { return c->id == selected_chat_; })))
    selected_chat_ = list.front()->id;
  for (auto* c : list) rows.push_back(render_chat_row(*c, c->id == selected_chat_));
  if (rows.empty()) rows.push_back(wtext(connected_ ? "  no chats match" : "  loading…") | color(theme_.dim));
  col.push_back(vbox(std::move(rows)) | yframe | flex);
  return vbox(std::move(col));
}

Element App::render_empty() {
  Elements art;
  for (auto line : GHOST_ART) art.push_back(wtext(line) | color(theme_.primary));
  return vbox({filler(), vbox(std::move(art)) | hcenter, wtext(""), wtext("Select a chat") | bold | hcenter,
               hbox({hint("enter", "open"), hint(cfg_.keys("camera").empty() ? "" : cfg_.keys("camera")[0], "live snap"),
                     hint("s", "stories"), hint("?", "keys")}) |
                   hcenter,
               filler()});
}

std::vector<const Message*> App::messages() const {
  std::vector<const Message*> v;
  if (conv_)
    for (auto& it : conv_->items)
      if (it.kind == ConvItem::Msg) v.push_back(&it.msg);
  return v;
}

const Message* App::selected_message() const { return conv_ ? conv_->find(selected_msg_) : nullptr; }

Element App::render_message(const Message& m, bool header, bool selected) {
  Color who = parse_color(m.mine() ? cfg_.str("ui", "me_color") : cfg_.str("ui", "them_color"),
                          m.mine() ? theme_.blue : theme_.red);
  Elements parts;
  if (header) {
    std::string name = m.sender.empty() ? "?" : m.sender;
    std::string upper = upper_case(name);
    parts.push_back(hbox({wtext(upper) | bold | color(who),
                          wtext("  " + local_time(m.time, cfg_.str("ui", "time_format"))) | color(theme_.dim)}));
  }
  Elements body;
  if (!m.quote_text.empty()) {
    body.push_back(wtext("╭ " + m.quote_sender) | color(theme_.dim));
    body.push_back(hbox({wtext("│ ") | color(theme_.dim), wparagraph(m.quote_text) | italic | color(theme_.dim)}));
  }
  if (!m.text.empty())
    for (auto& line : split_lines(m.text)) body.push_back(wparagraph(line.empty() ? " " : line));
  for (auto& kind : m.media) {
    if (kind == "audio") {
      bool playing = playing_key_ == m.key;
      std::string len;
      if (m.duration > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, " · %d:%02d", int(m.duration) / 60, int(m.duration) % 60);
        len = buf;
      }
      body.push_back(hbox({wtext(playing ? "■ " : "▶ ") | bold | color(theme_.purple),
                           wtext("Voice note" + len) | bold | color(theme_.purple),
                           wtext("   ▁▂▅▇▅▃▂▁▃▅▂") | color(playing ? theme_.purple : theme_.dim),
                           wtext(playing ? "   o to stop" : "   o to play") | color(theme_.dim)}));
      continue;
    }
    Color c = kind == "video" ? theme_.purple : theme_.blue;
    body.push_back(hbox({wtext("▣ ") | bold | color(c), wtext(kind == "video" ? "Video" : "Photo") | bold | color(c),
                         wtext("   o to view") | color(theme_.dim)}));
  }
  if (m.snap_new) {
    body.push_back(hbox({wtext("■ ") | bold | color(theme_.red),
                         wtext(m.snap_status.empty() ? "New Snap" : m.snap_status) | bold | color(theme_.red),
                         wtext("   o to open") | color(theme_.dim)}));
  } else if (!m.snap_status.empty()) {
    auto st = status_style(m.snap_status, theme_);
    body.push_back(hbox({wtext(std::string(st.icon) + " ") | bold | color(st.color), wtext(m.snap_status) | color(st.color)}));
  }
  if (m.not_supported) body.push_back(wtext("◇ Not supported on web — check your phone") | color(theme_.dim));
  if (body.empty()) body.push_back(wtext("▣ Media") | bold | color(theme_.blue));
  if (saved_.count(m.key) && saved_[m.key]) body.push_back(wtext("◆ saved") | bold | color(theme_.dim));
  parts.push_back(hbox({separatorCharacter("▎") | color(who), wtext(" "), vbox(std::move(body)) | flex}));
  if (!m.reactions.empty() && cfg_.flag("ui", "show_reactions")) {
    std::string r;
    for (auto& x : m.reactions) r += (r.empty() ? "" : "   ") + reaction_label(x);
    parts.push_back(wtext("  " + r) | color(theme_.dim));
  }
  auto el = vbox(std::move(parts));
  if (selected) el = el | bgcolor(focus_ == Focus::Conv ? theme_.boost : theme_.surface) | focus;
  return el;
}

Element App::render_conv() {
  const Chat* chat = chats_.count(open_id_) ? &chats_.at(open_id_) : nullptr;
  // Header: name, then group · streak · status.
  Elements sub;
  if (chat) {
    auto st = status_style(chat->status, theme_);
    if (chat->group) sub.push_back(wtext("group  ·  ") | color(theme_.dim));
    if (!chat->streak().empty()) sub.push_back(wtext(chat->streak() + "  ·  "));
    sub.push_back(wtext(std::string(st.icon) + " " + chat->status) | color(st.color));
  }
  auto head = vbox({hbox({wtext(chat ? chat->name : "") | bold, wtext(chat && !chat->badge.empty() ? "  " + chat->badge : "")}),
                    hbox(std::move(sub))}) |
              borderStyled(EMPTY);

  Elements items;
  const Message* prev = nullptr;
  bool show_dates = cfg_.flag("ui", "show_date_separators");
  if (conv_) {
    for (auto& it : conv_->items) {
      if (it.kind == ConvItem::Date) {
        if (show_dates) {
          items.push_back(wtext(""));
          items.push_back(wtext("─────  " + it.label + "  ─────") | color(theme_.dim) | hcenter);
        }
        prev = nullptr;
      } else if (it.kind == ConvItem::Notice) {
        items.push_back(wtext(it.label) | italic | color(theme_.dim) | hcenter);
        prev = nullptr;
      } else {
        bool header = !prev || prev->sender != it.msg.sender || prev->time != it.msg.time;
        if (header && prev) items.push_back(wtext(""));
        items.push_back(render_message(it.msg, header, it.msg.key == selected_msg_));
        prev = &it.msg;
      }
    }
  } else {
    items.push_back(wtext("  loading…") | color(theme_.dim));
  }
  for (auto& p : pending_) {
    if (p.chat_id != open_id_) continue;
    Elements b = {wparagraph(p.text)};
    b.push_back(p.failed.empty() ? wtext("◌ sending…") | italic | color(theme_.dim)
                                 : wtext("✗ not sent: " + p.failed) | bold | color(theme_.red));
    items.push_back(hbox({separatorCharacter("▎") | color(theme_.dim), wtext(" "), vbox(std::move(b)) | flex}));
  }

  Elements act;
  if (conv_ && conv_->typing) {
    act.push_back(wtext("✎ ") | bold | color(theme_.primary));
    act.push_back(wtext(conv_->activity.empty() ? "typing…" : conv_->activity) | color(theme_.primary));
  } else if (conv_ && !conv_->activity.empty()) {
    act.push_back(wtext(conv_->activity));
  }
  if (conv_ && !conv_->seen_by.empty()) {
    std::string who;
    for (auto& s : conv_->seen_by) who += (who.empty() ? "" : ", ") + s;
    act.push_back(wtext("   seen by ") | color(theme_.dim));
    act.push_back(wtext(who));
  }

  Elements col = {head, separatorStyled(LIGHT) | color(theme_.panel),
                  vbox(std::move(items)) | yframe | flex, hbox(std::move(act))};
  if (!suggestions_.empty()) {
    Elements s = {wtext(" tab ") | bold | color(theme_.primary)};
    for (size_t i = 0; i < suggestions_.size(); ++i) {
      s.push_back(wtext(" " + suggestions_[i].first + " "));
      if (i == 0) s.push_back(wtext(suggestions_[i].second + " ") | color(theme_.dim));
    }
    col.push_back(hbox(std::move(s)));
  }
  bool typing = focus_ == Focus::Compose;
  std::string ph = reply_key_ ? "Reply…" : typing ? "Send a chat   (/send ~/pic.jpg · :fire + tab · ctrl+e emoji)" : "Send a chat";
  compose_.placeholder = ph;
  col.push_back(compose_.render(typing, theme_.fg, theme_.dim) |
                borderStyled(ROUNDED, typing ? theme_.primary : theme_.panel));
  return vbox(std::move(col));
}

Element App::render_footer() {
  static const std::pair<const char*, const char*> shown[] = {
      {"compose", "chat"}, {"camera", "snap"}, {"reply", "reply"}, {"menu", "react"},     {"open_media", "open"},
      {"send_file", "photo"}, {"emoji", "emoji"}, {"search", "search"}, {"help", "keys"}, {"quit", "quit"}};
  Elements f;
  for (auto [action, label] : shown) {
    auto ks = cfg_.keys(action);
    if (ks.empty()) continue;
    f.push_back(hint(key_label(ks[0]), label));
  }
  return hbox(std::move(f)) | bgcolor(theme_.surface);
}

// --- events ---

bool App::on_event(const Event& e) {
  if (e == Event::Custom) return true;
  if (e.is_mouse()) return false;
  if (!modals_.empty()) {
    auto m = modals_.back();  // keep alive while it handles the event
    return m->event(*this, e);
  }
  if (focus_ == Focus::Compose) return on_compose_key(e);
  if (focus_ == Focus::Search) return on_search_key(e);

  if (key(e, "quit")) {
    screen_->Exit();
    return true;
  }
  if (key(e, "help")) return push(std::make_shared<HelpModal>()), true;
  if (key(e, "down")) return move(1), true;
  if (key(e, "up")) return move(-1), true;
  if (key(e, "open")) {
    if (focus_ == Focus::Chats) open_selected_chat();
    else message_menu();
    return true;
  }
  if (key(e, "back")) return back(), true;
  if (key(e, "search")) {
    search_open_ = true;
    focus_ = Focus::Search;
    return true;
  }
  if (key(e, "compose")) return compose(), true;
  if (key(e, "reply")) {
    if (auto* m = selected_message(); m && focus_ == Focus::Conv) compose(m->key);
    else notify("Select a message in the chat to reply to.");
    return true;
  }
  if (key(e, "menu")) return message_menu(), true;
  if (key(e, "open_media")) return open_media(), true;
  if (key(e, "camera")) return camera(), true;
  if (key(e, "send_file")) return send_file_picker(), true;
  if (key(e, "emoji")) {
    compose();
    return on_compose_key(e);
  }
  if (key(e, "mark_read")) return mark_read(), true;
  if (key(e, "stories")) return stories(), true;
  if (key(e, "refresh")) {
    if (bridge_) bg([this] { bridge_->refresh(); });
    return true;
  }
  if (key(e, "call")) return notify("Voice/video calls are not supported in the terminal.", true), true;
  if (e == Event::Tab && !open_id_.empty()) {
    focus_ = focus_ == Focus::Chats ? Focus::Conv : Focus::Chats;
    return true;
  }
  return false;
}

bool App::on_search_key(const Event& e) {
  if (e == Event::Escape) {
    search_.clear();
    search_open_ = false;
    focus_ = Focus::Chats;
    return true;
  }
  if (e == Event::Return || e == Event::ArrowDown) {
    focus_ = Focus::Chats;
    if (search_.empty()) search_open_ = false;
    return true;
  }
  search_.handle(e);
  return true;
}

bool App::on_compose_key(const Event& e) {
  if (e == Event::Escape) return end_compose(true), true;
  if (e == Event::Return) return submit_compose(), true;
  if (e == Event::Tab && !suggestions_.empty()) return accept_emoji(), true;
  if (key(e, "emoji")) {
    push(std::make_shared<EmojiModal>([this](std::optional<std::string> v) {
      if (!v) return;
      emoji::remember(*v);
      compose_.insert(*v);
      draft_changed();
    }));
    return true;
  }
  if (compose_.handle(e)) {
    update_emoji();
    draft_changed();
  }
  return true;
}

void App::move(int delta) {
  if (focus_ == Focus::Chats) {
    auto list = visible_chats();
    if (list.empty()) return;
    int i = 0;
    for (int k = 0; k < int(list.size()); ++k)
      if (list[k]->id == selected_chat_) i = k;
    if (delta > 0 && i == int(list.size()) - 1 && bridge_) bg([this] { bridge_->load_more_chats(); });
    i = std::clamp(i + delta, 0, int(list.size()) - 1);
    selected_chat_ = list[i]->id;
  } else if (focus_ == Focus::Conv) {
    auto msgs = messages();
    if (msgs.empty()) return;
    int i = int(msgs.size()) - 1;
    for (int k = 0; k < int(msgs.size()); ++k)
      if (msgs[k]->key == selected_msg_) i = k;
    if (delta < 0 && i == 0 && bridge_) {
      status("Loading older messages…");
      bg([this] {
        bridge_->load_older();
        post([this] { status(""); });
      });
    }
    i = std::clamp(i + delta, 0, int(msgs.size()) - 1);
    selected_msg_ = msgs[i]->key;
    follow_end_ = i == int(msgs.size()) - 1;
  }
}

void App::open_selected_chat() {
  auto it = chats_.find(selected_chat_);
  if (it != chats_.end()) open_chat(it->second);
}

void App::open_chat(const Chat& c) {
  if (!bridge_) return;
  open_id_ = c.id;
  conv_.reset();
  selected_msg_.clear();
  follow_end_ = true;
  reply_key_.reset();
  status("Opening " + c.name + "…");
  std::string id = c.id;
  bg([this, id] {
    bridge_->open_chat(id);
    post([this] {
      focus_ = Focus::Conv;
      status("");
    });
  });
}

void App::back() {
  if (!search_.empty() && focus_ == Focus::Chats) {
    search_.clear();
    search_open_ = false;
    return;
  }
  if (focus_ == Focus::Conv) {
    focus_ = Focus::Chats;
    if (cfg_.flag("behavior", "close_chat_on_back") && bridge_) {
      open_id_.clear();
      conv_.reset();
      bg([this] { bridge_->close_chat(); });
    }
  }
}

// --- composing ---

void App::compose(std::optional<std::string> reply_key) {
  if (open_id_.empty()) return notify("Open a chat first.");
  reply_key_ = reply_key;
  focus_ = Focus::Compose;
  if (reply_key) {
    if (auto* m = conv_ ? conv_->find(*reply_key) : nullptr)
      status("↩ replying to " + m->sender + ": " + fit(m->text.empty() ? "media" : m->text, 60));
  }
}

void App::end_compose(bool clear_page) {
  compose_.clear();
  suggestions_.clear();
  reply_key_.reset();
  focus_ = open_id_.empty() ? Focus::Chats : Focus::Conv;
  ++draft_gen_;
  if (clear_page && cfg_.flag("behavior", "send_typing") && bridge_) bg([this] { bridge_->set_draft(""); });
  status("");
}

void App::draft_changed() {
  if (!cfg_.flag("behavior", "send_typing") || !bridge_ || compose_.value().starts_with("/")) return;
  int gen = ++draft_gen_;
  std::string text = compose_.value();
  if (text.empty()) return;
  std::thread([this, gen, text] {
    sleep_ms(600);  // debounce: mirror after a pause in typing
    if (gen != draft_gen_ || quitting_) return;
    try {
      bridge_->set_draft(text);
    } catch (...) {
      // best effort
    }
  }).detach();
}

void App::update_emoji() {
  const std::string& v = compose_.value();
  if (auto code = emoji::trailing_code(v)) {
    if (auto e = emoji::lookup(code->first)) {
      compose_.set(v.substr(0, code->second) + *e);
      emoji::remember(*e);
    }
  }
  if (auto part = emoji::trailing_partial(compose_.value()))
    suggestions_ = emoji::search(part->first, 8);
  else
    suggestions_.clear();
}

void App::accept_emoji() {
  auto part = emoji::trailing_partial(compose_.value());
  if (!part || suggestions_.empty()) return;
  std::string e = suggestions_[0].first;
  compose_.set(compose_.value().substr(0, part->second) + e);
  emoji::remember(e);
  suggestions_.clear();
  draft_changed();
}

void App::submit_compose() {
  std::string text = trim(compose_.value());
  suggestions_.clear();
  if (text.empty()) return;
  if (text.starts_with("/send ")) {
    fs::path p = expand_user(trim(text.substr(6)));
    end_compose(false);
    confirm_send_file(p);
    return;
  }
  if (text.starts_with("/call") || text.starts_with("/video") || text.starts_with("/lens")) {
    end_compose(true);
    notify("Calls are not supported in the terminal. Lenses: press c for the camera.", true);
    return;
  }
  text = emoji::emojize(text);
  auto key = reply_key_;
  std::string chat = open_id_;
  compose_.clear();
  reply_key_.reset();
  ++draft_gen_;
  status("");
  send_message(chat, text, key);  // stay in compose: keep chatting
}

void App::send_message(const std::string& chat_id, const std::string& text, std::optional<std::string> reply_key) {
  pending_.push_back({chat_id, text, ""});
  follow_end_ = true;
  bg([this, chat_id, text, reply_key] {
    try {
      if (reply_key)
        bridge_->reply(*reply_key, text);
      else
        bridge_->send_text(text);
    } catch (const std::exception& e) {
      std::string msg = e.what();
      post([this, chat_id, text, msg] {
        for (auto& p : pending_)
          if (p.chat_id == chat_id && p.text == text && p.failed.empty()) p.failed = msg.substr(0, msg.find('\n'));
      });
      throw;
    }
    // Accepted; the pending line goes when the message shows up (settle_pending) or after 10 s.
    sleep_ms(10000);
    post([this, chat_id, text] {
      pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                    [&](auto& p) { return p.chat_id == chat_id && p.text == text && p.failed.empty(); }),
                     pending_.end());
    });
  });
}

void App::settle_pending() {
  if (pending_.empty() || !conv_) return;
  std::vector<std::string> mine;
  for (auto* m : messages())
    if (m->mine()) mine.push_back(squash(m->text));
  size_t from = mine.size() > 15 ? mine.size() - 15 : 0;
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&](auto& p) {
                                  return p.failed.empty() &&
                                         std::find(mine.begin() + long(from), mine.end(), squash(p.text)) != mine.end();
                                }),
                 pending_.end());
}

// --- message menu ---

void App::message_menu() {
  const Message* m = selected_message();
  if (!m || !bridge_ || focus_ != Focus::Conv) return notify("Select a message first.");
  Message msg = *m;
  bg([this, msg] {
    auto entries = bridge_->open_menu(msg.key);
    post([this, msg, entries] {
      bool has_save = std::find(entries.begin(), entries.end(), "Save in Chat") != entries.end();
      bool has_unsave = std::find(entries.begin(), entries.end(), "Unsave in Chat") != entries.end();
      if (has_save || has_unsave) saved_[msg.key] = has_unsave;
      std::vector<MenuModal::Option> opts;
      for (auto& e : entries) {
        if (e.starts_with("react:")) {
          std::string name = e.substr(6);
          opts.push_back({e, hbox({wtext(std::string(reaction_emoji(name)) + "  "), wtext(name)})});
        } else {
          const char* icon = e == "Save in Chat" ? "◆" : e == "Unsave in Chat" ? "◇" : e == "Reply" ? "↩"
                             : e == "Copy Text" ? "⧉" : e == "Delete" ? "✗" : "•";
          opts.push_back({e, hbox({wtext(std::string(icon) + "  "), wtext(e) | bold | (e == "Delete" ? color(theme_.red) : nothing)})});
        }
      }
      Color who = parse_color(msg.mine() ? cfg_.str("ui", "me_color") : cfg_.str("ui", "them_color"), theme_.blue);
      auto title = hbox({wtext(msg.sender) | bold | color(who), wtext("  " + fit(msg.text.empty() ? "media" : msg.text, 36)),
                         wtext(saved_[msg.key] ? "   ◆ saved" : "   not saved") | color(theme_.dim)});
      push(std::make_shared<MenuModal>(title, opts, [this, msg](std::optional<std::string> choice) {
        if (!choice) return bg([this] { bridge_->close_menu(); });
        if (*choice == "Copy Text") {
          bg([this] { bridge_->close_menu(); });
          // OSC 52: the terminal puts it on the clipboard.
          std::cout << "\x1b]52;c;" << b64encode(msg.text) << "\x07" << std::flush;
          return notify("Copied");
        }
        if (*choice == "Reply") {
          bg([this] { bridge_->close_menu(); });
          return compose(msg.key);
        }
        if (*choice == "Delete") {
          bg([this] { bridge_->close_menu(); });
          push(std::make_shared<ConfirmModal>("Delete this message for everyone?", "delete", [this, msg](bool yes) {
            if (!yes) return;
            bg([this, msg] {
              bridge_->open_menu(msg.key);
              bridge_->choose_menu("Delete");
              sleep_ms(800);
              bridge_->choose_menu_confirm("Delete");
            });
          }));
          return;
        }
        std::string c = *choice;
        bg([this, msg, c] {
          bridge_->choose_menu(c);
          post([this, msg, c] {
            if (c == "Save in Chat" || c == "Unsave in Chat") {
              saved_[msg.key] = c == "Save in Chat";
              notify(saved_[msg.key] ? "Saved in chat" : "Unsaved");
            } else if (c.starts_with("react:")) {
              notify(std::string("Reacted ") + reaction_emoji(c.substr(6)));
            }
          });
        });
      }));
    });
  });
}

// --- media, snaps, camera ---

void App::open_media() {
  const Message* m = selected_message();
  if (!m || !bridge_ || focus_ != Focus::Conv) return notify("Select a snap or media message first.");
  Message msg = *m;
  if (msg.not_supported) return notify("Snapchat Web can't show this one; check your phone.");
  auto dump_dir = cfg_.flag("debug", "dump_viewer") && !dumped_viewer_ ? std::optional(ghost_home() / "debug")
                                                                        : std::nullopt;
  if (msg.snap_new) {
    auto open = [this, msg, dump_dir] {
      if (dump_dir) dumped_viewer_ = true;
      status("Opening snap…");
      bg([this, msg, dump_dir] {
        auto [media, info] = bridge_->open_snap(msg.key, dump_dir ? &*dump_dir : nullptr);
        if (info.sender.empty()) info.sender = msg.sender;
        post([this, media, info] {
          status("");
          push(std::make_shared<ViewerModal>(std::vector{std::pair{media, info}}, "Snap", true));
        });
      });
    };
    if (!cfg_.flag("behavior", "confirm_snap_open")) return open();
    push(std::make_shared<ConfirmModal>("■ Open the snap from " + msg.sender +
                                            "?\n\nOpening marks it as viewed; it can't be opened again.",
                                        "open", [open](bool yes) {
                                          if (yes) open();
                                        }));
    return;
  }
  if (std::find(msg.media.begin(), msg.media.end(), "audio") != msg.media.end()) return play_voice(msg);
  if (!msg.media.empty()) {
    status("Loading media…");
    bg([this, msg] {
      auto items = bridge_->message_media(msg.key);
      post([this, msg, items] {
        status("");
        if (items.empty()) return notify("No media found in that message.");
        std::vector<std::pair<Media, ViewerInfo>> v;
        for (auto& i : items) v.push_back({i, {msg.sender, ""}});
        push(std::make_shared<ViewerModal>(v, "Photo", false));
      });
    });
    return;
  }
  if (!msg.snap_status.empty())
    return notify("This snap was already " + lower(msg.snap_status) + "; snaps can only be viewed once.");
  notify("Nothing to open in this message.");
}

void App::stop_voice() {
  if (audio_pid_ > 0) media::stop(audio_pid_);
  audio_pid_ = -1;
  playing_key_.clear();
}

void App::play_voice(const Message& m) {
  if (playing_key_ == m.key) return stop_voice();
  stop_voice();
  status("Loading voice note…");
  std::string key = m.key, sender = m.sender;
  bg([this, key, sender] {
    std::optional<Media> audio;
    for (auto& i : bridge_->message_media(key))
      if (i.kind == "audio") audio = i;
    if (!audio) return post([this] { notify("Couldn't load that voice note (it may have expired)."); });
    auto path = media::save(*audio, "voice-" + sender + "-" + local_time(now_epoch(), "%Y%m%d-%H%M%S"));
    int pid = media::play_audio(path, cfg_.str("images", "video_player"));
    post([this, key, pid, path] {
      status("");
      if (pid < 0) return notify("No audio player found (install mpv). Saved to " + path.string(), true);
      audio_pid_ = pid;
      playing_key_ = key;
    });
    while (media::running(pid)) sleep_ms(300);
    post([this, pid] {
      if (audio_pid_ == pid) audio_pid_ = -1, playing_key_.clear();
    });
  });
}

void App::stories() {
  if (!bridge_) return;
  bool had_chat = !open_id_.empty();
  open_id_.clear();
  conv_.reset();
  focus_ = Focus::Chats;
  auto dump_dir = cfg_.flag("debug", "dump_viewer") && !dumped_viewer_ ? std::optional(ghost_home() / "debug")
                                                                        : std::nullopt;
  if (dump_dir) dumped_viewer_ = true;
  bg([this, had_chat, dump_dir] {
    if (had_chat) bridge_->close_chat();
    auto label = bridge_->stories_available();
    if (contains_ci(label, "no stories")) return post([this] { notify("No stories to view right now."); });
    auto [media, info] = bridge_->open_stories(dump_dir ? &*dump_dir : nullptr);
    post([this, media, info] { push(std::make_shared<ViewerModal>(std::vector{std::pair{media, info}}, "Story", true)); });
  });
}

void App::camera() {
  if (open_id_.empty() || !bridge_) return notify("Open a chat first.");
  auto cam = std::make_shared<CameraModal>(chat_name(open_id_));
  push(cam);
  cam->start(*this);
}

void App::send_file_picker() {
  if (open_id_.empty()) return notify("Open a chat first.");
  push(std::make_shared<FilePickerModal>([this](std::optional<fs::path> p) {
    if (p) confirm_send_file(*p);
  }));
}

void App::confirm_send_file(const fs::path& p) {
  if (open_id_.empty()) return notify("Open a chat first.");
  if (!fs::is_regular_file(p)) return notify("No such file: " + p.string(), true);
  push(std::make_shared<ConfirmModal>("Send " + p.filename().string() + " to " + chat_name(open_id_) + "?", "send",
                                      [this, p](bool yes) {
                                        if (!yes) return;
                                        status("Sending " + p.filename().string() + "…");
                                        bg([this, p] {
                                          auto r = bridge_->send_file(p);
                                          post([this, p, r] {
                                            status("");
                                            notify(p.filename().string() + ": " + r);
                                          });
                                        });
                                      }));
}

void App::mark_read() {
  if (focus_ != Focus::Chats) return notify("Select a chat in the list first.");
  auto it = chats_.find(selected_chat_);
  if (it == chats_.end() || !bridge_) return;
  if (contains_ci(it->second.status, "snap")) notify("Opening a chat doesn't open its snaps; they stay unopened.");
  std::string id = it->first, name = it->second.name;
  bg([this, id, name] {
    bridge_->mark_read(id);
    post([this, name] { notify("Marked " + name + " as read"); });
  });
}

}  // namespace ghost::ui

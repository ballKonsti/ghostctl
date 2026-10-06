#include "ui/modals.hpp"

#include <algorithm>
#include <thread>

#include "media.hpp"
#include "ui/wtext.hpp"
#include "util.hpp"

namespace ghost::ui {

using namespace ftxui;

static bool is(const Event& e, std::initializer_list<const char*> chars) {
  for (auto c : chars)
    if (e == Event::Character(c)) return true;
  return false;
}

// Keep `sel` on an index in [0, n).
static void clamp(int& sel, size_t n) { sel = n ? std::clamp(sel, 0, int(n) - 1) : 0; }

// --- help ---

Element HelpModal::render(App& app) {
  const auto& t = app.theme();
  static const std::pair<const char*, const char*> rows[] = {
      {"down", "move down (end of the list: load more chats)"},
      {"up", "move up (top of a chat: load older messages)"},
      {"open", "open chat / message menu"},
      {"back", "back / cancel / clear search"},
      {"compose", "write a message (/send <path> attaches an image)"},
      {"reply", "reply to the selected message"},
      {"menu", "react / save / copy / delete the selected message"},
      {"open_media", "open snap, photo or voice note (snaps: marks it viewed)"},
      {"camera", "take a live photo snap with your webcam"},
      {"send_file", "send an image from a file"},
      {"emoji", "emoji picker (while writing: :name + tab, e.g. :fire or :feuer)"},
      {"search", "search chats"},
      {"mark_read", "mark the selected chat as read"},
      {"stories", "view stories"},
      {"refresh", "re-read the page"},
      {"call", "start a call (not supported)"},
      {"help", "this help"},
      {"quit", "quit"},
  };
  Elements lines = {hbox({wtext("👻 ghostctl") | bold | color(t.primary), wtext("  keys") | color(t.dim)}), wtext("")};
  for (auto [action, desc] : rows) {
    std::string keys;
    for (auto& k : app.cfg().keys(action)) keys += (keys.empty() ? "" : " ") + key_label(k);
    lines.push_back(hbox({wtext("  "), app.pill(keys, t.bg, t.primary, true), filler()} ) | size(WIDTH, EQUAL, 16));
    lines.back() = hbox({lines.back(), wtext(desc)});
  }
  lines.push_back(wtext(""));
  lines.push_back(hbox({wtext("  Snap viewer  ") | bold, wtext("n next · 1-8 react · p play video · esc close") |
                                                            color(t.dim)}));
  lines.push_back(hbox({wtext("  Camera       ") | bold,
                        wtext("space take snap · ←/→ lens · enter send to · esc retake/close") | color(t.dim)}));
  lines.push_back(wtext(""));
  lines.push_back(wtext("  Config: " + config_path().string()) | color(t.dim));
  lines.push_back(wtext("  `ghostctl config --edit` changes keys, colours, theme and behaviour.") | color(t.dim));
  return app.dialog(vbox(std::move(lines)), 84);
}

bool HelpModal::event(App& app, const Event& e) {
  if (e == Event::Escape || is(e, {"q", "?"}) || e == Event::Return) app.pop(this);
  return true;
}

// --- confirm ---

Element ConfirmModal::render(App& app) {
  return app.dialog(vbox({wparagraph(question), wtext(""), hbox({app.hint("y", yes), wtext("    "), app.hint("n", "cancel")})}),
                    64);
}

bool ConfirmModal::event(App& app, const Event& e) {
  if (is(e, {"y", "Y"}) || e == Event::Return) {
    auto c = cb;
    app.pop(this);
    c(true);
  } else if (is(e, {"n", "N"}) || e == Event::Escape) {
    auto c = cb;
    app.pop(this);
    c(false);
  }
  return true;
}

// --- menu ---

Element MenuModal::render(App& app) {
  clamp(sel, options.size());
  Elements rows;
  for (int i = 0; i < int(options.size()); ++i) {
    auto row = app.row_select(hbox({wtext(" "), options[i].label, filler()}), i == sel);
    rows.push_back(i == sel ? row | focus : row);
  }
  return app.dialog(vbox({title, wtext(""), vbox(std::move(rows)) | yframe | size(HEIGHT, LESS_THAN, 20)}), 60);
}

bool MenuModal::event(App& app, const Event& e) {
  if (e == Event::ArrowDown || is(e, {"j"})) return ++sel, true;
  if (e == Event::ArrowUp || is(e, {"k"})) return --sel, true;
  if (e == Event::Return && !options.empty()) {
    auto c = cb;
    auto id = options[std::clamp(sel, 0, int(options.size()) - 1)].id;
    app.pop(this);
    c(id);
    return true;
  }
  if (e == Event::Escape || is(e, {"q"})) {
    auto c = cb;
    app.pop(this);
    c(std::nullopt);
  }
  return true;
}

// --- prompt (2FA code etc.) ---

PromptModal::PromptModal(std::string prompt, bool secret, std::vector<std::string> msgs,
                         std::function<void(std::optional<std::string>)> cb)
    : messages(std::move(msgs)), field(std::move(prompt), secret), cb(std::move(cb)) {}

Element PromptModal::render(App& app) {
  const auto& t = app.theme();
  Elements lines = {wtext("Snapchat") | bold | color(t.primary), wtext("")};
  size_t from = messages.size() > 3 ? messages.size() - 3 : 0;
  for (size_t i = from; i < messages.size(); ++i) lines.push_back(wparagraph(messages[i]) | color(t.dim));
  lines.push_back(wtext(""));
  lines.push_back(field.render(true, t.fg, t.dim) | borderStyled(ROUNDED, t.primary));
  return app.dialog(vbox(std::move(lines)), 64);
}

bool PromptModal::event(App& app, const Event& e) {
  if (e == Event::Return && !trim(field.value()).empty()) {
    auto c = cb;
    auto v = trim(field.value());
    app.pop(this);
    c(v);
    return true;
  }
  if (e == Event::Escape) {
    auto c = cb;
    app.pop(this);
    c(std::nullopt);
    return true;
  }
  field.handle(e);
  return true;
}

// --- login ---

LoginModal::LoginModal(bool can_rem, std::string err, std::function<void(std::optional<LoginForm>)> cb)
    : remember(can_rem), can_remember(can_rem), error(std::move(err)), cb(std::move(cb)) {}

Element LoginModal::render(App& app) {
  const auto& t = app.theme();
  Elements art;
  for (auto line : GHOST_ART) art.push_back(wtext(line) | color(t.primary));
  auto field = [&](TextField& f, int idx) {
    return f.render(focus == idx, t.fg, t.dim) | borderStyled(ROUNDED, focus == idx ? t.primary : t.panel);
  };
  auto box = hbox({wtext(remember ? "[x] " : "[ ] ") | color(focus == 2 ? t.primary : t.fg),
                   wtext(can_remember ? "Remember me — stored encrypted in your system keyring"
                                     : "Remember me (no system keyring available)") |
                       color(can_remember ? t.fg : t.dim)});
  auto body = vbox({
      vbox(std::move(art)) | hcenter,
      wtext(""),
      wtext("Log in to Snapchat") | bold | hcenter,
      error.empty() ? wtext("") : wparagraph(error) | color(t.red),
      wtext(""),
      field(user, 0),
      field(pw, 1),
      box,
      wtext(""),
      hbox({app.hint("enter", "log in"), wtext("   "), app.hint("tab", "next field"), wtext("   "),
            app.hint("esc", "quit")}),
      wtext("Your password goes only into Snapchat's own login form.") | color(t.dim),
  });
  return vbox({filler(), hbox({filler(), app.dialog(body, 64), filler()}), filler()}) | bgcolor(t.bg);
}

bool LoginModal::event(App& app, const Event& e) {
  if (e == Event::Escape) {
    auto c = cb;
    app.pop(this);
    c(std::nullopt);
    return true;
  }
  if (e == Event::Tab || e == Event::ArrowDown) return focus = (focus + 1) % 3, true;
  if (e == Event::TabReverse || e == Event::ArrowUp) return focus = (focus + 2) % 3, true;
  if (focus == 2 && e == Event::Character(" ")) {
    if (can_remember) remember = !remember;
    return true;
  }
  if (e == Event::Return) {
    if (focus == 0 || pw.empty()) return focus = trim(user.value()).empty() ? 0 : 1, true;
    if (trim(user.value()).empty()) return focus = 0, true;
    auto c = cb;
    LoginForm f{trim(user.value()), pw.value(), remember && can_remember};
    app.pop(this);
    c(f);
    return true;
  }
  if (focus == 0) user.handle(e);
  if (focus == 1) pw.handle(e);
  return true;
}

// --- emoji picker ---

EmojiModal::EmojiModal(std::function<void(std::optional<std::string>)> cb) : cb(std::move(cb)) {
  results = emoji::search("", 80);
}

Element EmojiModal::render(App& app) {
  const auto& t = app.theme();
  clamp(sel, results.size());
  Elements rows;
  if (query.empty()) rows.push_back(wtext(" recent") | bold | color(t.dim));
  for (int i = 0; i < int(results.size()); ++i) {
    auto row = app.row_select(hbox({wtext(" " + results[i].first), wtext("  " + results[i].second) | color(t.dim), filler()}),
                              i == sel);
    rows.push_back(i == sel ? row | focus : row);
  }
  return app.dialog(vbox({hbox({wtext("Emoji  ") | bold | color(t.primary), wtext("type to search · enter insert") |
                                                                             color(t.dim)}),
                          query.render(true, t.fg, t.dim) | borderStyled(ROUNDED, t.primary),
                          vbox(std::move(rows)) | yframe | size(HEIGHT, EQUAL, 18)}),
                    60);
}

bool EmojiModal::event(App& app, const Event& e) {
  if (e == Event::Escape) {
    auto c = cb;
    app.pop(this);
    c(std::nullopt);
    return true;
  }
  if (e == Event::Return) {
    if (results.empty()) return true;
    auto c = cb;
    auto v = results[std::clamp(sel, 0, int(results.size()) - 1)].first;
    app.pop(this);
    c(v);
    return true;
  }
  if (e == Event::ArrowDown) return ++sel, true;
  if (e == Event::ArrowUp) return --sel, true;
  if (query.handle(e)) {
    results = emoji::search(query.value(), 80);
    sel = 0;
  }
  return true;
}

// --- file picker ---

FilePickerModal::FilePickerModal(std::function<void(std::optional<fs::path>)> cb) : dir(home_dir()), cb(std::move(cb)) {
  for (auto sub : {"Pictures", "Bilder"})
    if (fs::is_directory(dir / sub)) dir /= sub;
  list();
}

void FilePickerModal::list() {
  entries.clear();
  std::error_code ec;
  for (auto& d : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    auto n = d.path().filename().string();
    if (n.starts_with(".")) continue;
    entries.push_back(d);
  }
  std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
    bool da = a.is_directory(), db = b.is_directory();
    if (da != db) return da;
    return lower(a.path().filename().string()) < lower(b.path().filename().string());
  });
  sel = 0;
}

Element FilePickerModal::render(App& app) {
  const auto& t = app.theme();
  clamp(sel, entries.size() + 1);
  Elements rows;
  for (int i = 0; i <= int(entries.size()); ++i) {
    std::string label = i == 0 ? "../" : entries[i - 1].path().filename().string() + (entries[i - 1].is_directory() ? "/" : "");
    bool image = i > 0 && !entries[i - 1].is_directory() &&
                 std::set<std::string>{".png", ".jpg", ".jpeg", ".gif"}.count(lower(entries[i - 1].path().extension().string()));
    bool dir = i == 0 || entries[i - 1].is_directory();
    const char* icon = app.rounded() ? (dir ? " 󰉋 " : image ? " 󰋩 " : "   ") : (dir ? " ▸ " : "   ");
    auto row = app.row_select(hbox({wtext(icon) | color(dir ? t.blue : t.primary),
                                    wtext(label) | color(dir ? t.blue : image ? t.fg : t.dim), filler()}),
                              i == sel && !typing);
    rows.push_back(i == sel && !typing ? row | focus : row);
  }
  return app.dialog(vbox({hbox({wtext("Send a photo  ") | bold | color(t.primary), wtext("png · jpeg · gif") | color(t.dim)}),
                          path.render(typing, t.fg, t.dim) | borderStyled(ROUNDED, typing ? t.primary : t.panel),
                          wtext(dir.string()) | color(t.dim),
                          vbox(std::move(rows)) | yframe | size(HEIGHT, EQUAL, 20),
                          hbox({app.hint("enter", "open/choose"), wtext("  "), app.hint("tab", "type a path"), wtext("  "),
                                app.hint("esc", "cancel")})}),
                    90);
}

bool FilePickerModal::event(App& app, const Event& e) {
  auto done = [&](std::optional<fs::path> p) {
    auto c = cb;
    app.pop(this);
    c(p);
  };
  if (e == Event::Escape) return done(std::nullopt), true;
  if (e == Event::Tab) return typing = !typing, true;
  if (typing) {
    if (e == Event::Return) {
      fs::path p = expand_user(trim(path.value()));
      if (fs::is_regular_file(p)) return done(p), true;
      if (fs::is_directory(p)) {
        dir = p;
        list();
        typing = false;
        return true;
      }
      app.notify("Not a file: " + p.string(), true);
      return true;
    }
    path.handle(e);
    return true;
  }
  if (e == Event::ArrowDown || is(e, {"j"})) return ++sel, true;
  if (e == Event::ArrowUp || is(e, {"k"})) return --sel, true;
  if (e == Event::Backspace || is(e, {"h"})) {
    dir = dir.parent_path();
    list();
    return true;
  }
  if (e == Event::Return || is(e, {"l"})) {
    if (sel == 0) {
      dir = dir.parent_path();
      list();
    } else if (entries[sel - 1].is_directory()) {
      dir = entries[sel - 1].path();
      list();
    } else {
      done(entries[sel - 1].path());
    }
    return true;
  }
  if (e.is_character()) {  // start typing a path
    typing = true;
    path.handle(e);
  }
  return true;
}

// --- viewer ---

ViewerModal::ViewerModal(std::vector<std::pair<Media, ViewerInfo>> it, std::string k, bool v)
    : items(std::move(it)), kind(std::move(k)), viewer(v) {}

void ViewerModal::show(App& app) {
  shown = int(index);
  auto& [m, info] = items[index];
  path = media::save(m, local_time(now_epoch(), "%Y%m%d-%H%M%S") + "-" + std::to_string(index));
  std::optional<img::Image> im;
  if (m.kind == "video") {
    if (auto frame = media::video_frame(path)) im = img::load(frame->string());
    auto player = app.cfg().str("images", "video_player");
    if (!player.empty()) app.notify(media::play(path, player));
  } else {
    im = img::decode(m.data);
  }
  current = im ? *im : img::Image{};
  image = current.empty() ? nullptr : &current;
  ++image_version;
}

Element ViewerModal::render(App& app) {
  const auto& t = app.theme();
  if (shown != int(index) && index < items.size()) show(app);
  auto& [m, info] = items[std::min(index, items.size() - 1)];
  bool snap = kind == "Snap" || kind == "Story";
  Color c = m.kind == "video" ? t.purple : snap ? t.red : t.blue;
  Elements top = {wtext(" "), app.pill(std::string(snap ? "■ " : "▣ ") + kind, Color::Black, c, true)};
  if (!info.sender.empty()) top.push_back(hbox({wtext("  "), app.avatar(info.sender), wtext(" " + info.sender) | bold}));
  if (!info.time.empty()) top.push_back(wtext("  · " + ago(parse_iso(info.time)) + " ago") | color(t.dim));
  if (items.size() > 1 || viewer) top.push_back(wtext("   " + std::to_string(index + 1)) | color(t.dim));
  if (busy) top.push_back(wtext("   ◌ loading…") | color(t.dim));

  Element pic;
  int bw = image_box.x_max - image_box.x_min + 1, bh = image_box.y_max - image_box.y_min + 1;
  if (!image) {
    pic = vbox({filler(), wtext(m.kind + " saved to " + path.string()) | color(t.dim) | hcenter, filler()});
  } else if (app.protocol() == img::Protocol::Halfblock && bw > 1 && bh > 1) {
    auto [cols, rows] = img::fit_cells(current.w, current.h, bw, bh);
    pic = vbox({filler(), hbox({filler(), img::halfblock(current, cols, rows), filler()}), filler()});
  } else {
    pic = filler();  // kitty draws over this box after the frame
  }

  Elements bottom = {wtext(" ")};
  if (viewer) {
    for (int i = 0; i < 8; ++i) {
      bottom.push_back(app.pill(std::to_string(i + 1) + " " + reaction_emoji(REACTION_ORDER[i]), t.fg, t.panel));
      bottom.push_back(wtext(" "));
    }
  }
  bottom.push_back(app.hint("n", "next"));
  if (m.kind == "video") bottom.push_back(app.hint("p", "play"));
  bottom.push_back(app.hint("esc", "close"));
  bottom.push_back(wtext("   " + path.filename().string()) | color(t.dim));
  auto frame = (pic | flex | reflect(image_box)) | borderStyled(ROUNDED, c);
  return vbox({hbox(std::move(top)), frame | flex, hbox(std::move(bottom))}) | bgcolor(Color::Black);
}

void ViewerModal::close(App& app) {
  alive = false;
  if (viewer && app.bridge())
    app.bg([b = app.bridge()] { b->close_viewer(); });
  app.pop(this);
}

bool ViewerModal::event(App& app, const Event& e) {
  if (e == Event::Escape || is(e, {"q"})) return close(app), true;
  if (is(e, {"p"}) && items[index].first.kind == "video") {
    app.notify(media::play(path, app.cfg().str("images", "video_player")));
    return true;
  }
  for (int i = 0; i < 8; ++i) {
    if (viewer && is(e, {std::to_string(i + 1).c_str()})) {
      std::string name = REACTION_ORDER[i];
      app.bg([&app, name] {
        app.bridge()->viewer_react(name);
        app.post([&app, name] { app.notify(std::string("Reacted ") + reaction_emoji(name)); });
      });
      return true;
    }
  }
  if (is(e, {"n", " ", "l"}) || e == Event::ArrowRight) {
    if (busy) return true;
    if (index + 1 < items.size()) {
      ++index;
      show(app);
      return true;
    }
    if (!viewer) return close(app), true;
    busy = true;
    auto self = std::static_pointer_cast<ViewerModal>(shared_from_this());
    app.bg([&app, self] {
      auto next = app.bridge()->viewer_next();
      app.post([&app, self, next] {
        self->busy = false;
        if (!self->alive) return;
        if (!next) return self->close(app);
        self->items.push_back(*next);
        self->index++;
        self->show(app);
      });
    });
  }
  return true;
}

// --- send to ---

SendToModal::SendToModal(std::vector<Recipient> r, std::function<void(std::optional<std::set<int>>)> cb)
    : rows(std::move(r)), cb(std::move(cb)) {
  for (auto& x : rows)
    if (x.selected) chosen.insert(x.idx);
}

std::vector<const Recipient*> SendToModal::shown() const {
  std::vector<const Recipient*> out;
  std::string f = lower(filter.value());
  for (auto& r : rows)
    if (f.empty() || lower(r.name).find(f) != std::string::npos) out.push_back(&r);
  return out;
}

Element SendToModal::render(App& app) {
  const auto& t = app.theme();
  auto list = shown();
  clamp(sel, list.size());
  Elements lines;
  std::string section;
  for (int i = 0; i < int(list.size()); ++i) {
    auto* r = list[i];
    if (r->section != section && filter.empty()) {
      section = r->section;
      lines.push_back(hbox({wtext(" "), app.pill(section, t.dim, t.panel)}));
    }
    bool on = chosen.count(r->idx);
    auto row = app.row_select(hbox({wtext(on ? " ● " : " ○ ") | bold | color(on ? t.blue : t.dim), app.avatar(r->name),
                                    wtext(" " + r->name) | (on ? bold : nothing), wtext("  " + r->extra) | color(t.dim),
                                    filler()}),
                              i == sel && !typing);
    lines.push_back(i == sel && !typing ? row | focus : row);
  }
  std::string names;
  for (auto& r : rows)
    if (chosen.count(r.idx)) names += (names.empty() ? "" : ", ") + r.name;
  return app.dialog(
      vbox({wtext("Send To") | bold | color(t.primary),
            filter.render(typing, t.fg, t.dim) | borderStyled(ROUNDED, typing ? t.primary : t.panel),
            vbox(std::move(lines)) | yframe | size(HEIGHT, EQUAL, 22),
            hbox({app.hint("enter", ""), wtext("send to " + (names.empty() ? std::string("nobody yet") : names)) |
                                             (names.empty() ? color(t.dim) : bold)}),
            hbox({app.hint("space", "select"), wtext("  "), app.hint("/", "search"), wtext("  "), app.hint("esc", "back")})}),
      76);
}

bool SendToModal::event(App& app, const Event& e) {
  auto list = shown();
  if (typing) {
    if (e == Event::Return || e == Event::Escape || e == Event::ArrowDown) return typing = false, sel = 0, true;
    filter.handle(e);
    return true;
  }
  if (e == Event::Escape) {
    auto c = cb;
    app.pop(this);
    c(std::nullopt);
    return true;
  }
  if (is(e, {"/"})) return typing = true, true;
  if (e == Event::ArrowDown || is(e, {"j"})) return ++sel, true;
  if (e == Event::ArrowUp || is(e, {"k"})) return --sel, true;
  if (is(e, {" "}) && !list.empty()) {
    int idx = list[std::clamp(sel, 0, int(list.size()) - 1)]->idx;
    if (!chosen.erase(idx)) chosen.insert(idx);
    return true;
  }
  if (e == Event::Return) {
    if (chosen.empty()) return app.notify("Pick at least one recipient (space).", true), true;
    auto c = cb;
    auto ch = chosen;
    app.pop(this);
    c(ch);
  }
  return true;
}

// --- camera ---

void CameraModal::start(App& app) {
  auto self = std::static_pointer_cast<CameraModal>(shared_from_this());
  app.bg([&app, self] {
    try {
      app.bridge()->open_camera();
    } catch (const std::exception& e) {
      std::string msg = e.what();
      app.post([&app, self, msg] {
        app.notify(msg, true);
        self->alive = false;
        app.pop(self.get());
      });
      return;
    }
    app.post([&app, self] {
      self->state = State::Live;
      self->live_loop(app);
    });
  });
}

void CameraModal::live_loop(App& app) {
  auto self = std::static_pointer_cast<CameraModal>(shared_from_this());
  std::thread([&app, self] {
    while (self->alive && self->state == State::Live) {
      auto jpg = app.bridge()->camera_frame();
      if (jpg) {
        if (auto im = img::decode(*jpg)) {
          app.post([self, im = std::move(*im)]() mutable {
            if (self->state != State::Live) return;
            self->frame = std::move(im);
            self->image = &self->frame;
            ++self->image_version;
          });
        }
      }
      sleep_ms(80);
    }
  }).detach();
}

Element CameraModal::render(App& app) {
  const auto& t = app.theme();
  Elements top;
  switch (state) {
    case State::Preview: top.push_back(app.pill("■ Snap ready", Color::Black, t.primary, true)); break;
    case State::Live: top.push_back(app.pill("● LIVE", Color::White, t.red, true)); break;
    case State::Starting: top.push_back(app.pill("◌ starting camera…", t.dim, t.panel)); break;
    case State::Capturing: top.push_back(app.pill("◌ capturing…", t.dim, t.panel)); break;
    case State::Sending: top.push_back(app.pill("◌ sending…", t.dim, t.panel)); break;
    case State::Closing: top.push_back(app.pill("◌ closing…", t.dim, t.panel)); break;
  }
  top.insert(top.begin(), wtext(" "));
  top.push_back(hbox({wtext("  to  "), app.avatar(chat), wtext(" " + chat) | bold}));

  Element pic = filler();
  int bw = image_box.x_max - image_box.x_min + 1, bh = image_box.y_max - image_box.y_min + 1;
  if (image && app.protocol() == img::Protocol::Halfblock && bw > 1 && bh > 1) {
    auto [cols, rows] = img::fit_cells(frame.w, frame.h, bw, bh);
    pic = vbox({filler(), hbox({filler(), img::halfblock(frame, cols, rows), filler()}), filler()});
  }
  Elements bottom = {wtext(" ")};
  if (state == State::Live) {
    bottom.push_back(app.hint("space", "take snap"));
    bottom.push_back(app.hint("←/→", "lens"));
    bottom.push_back(app.hint("esc", "close"));
  } else if (state == State::Preview) {
    bottom.push_back(app.hint("type", "caption"));
    bottom.push_back(app.hint("enter", "send to…"));
    bottom.push_back(app.hint("esc", "retake"));
  } else {
    bottom.push_back(app.hint("esc", "close"));
  }
  Color frame_c = state == State::Live ? t.red : state == State::Preview ? t.primary : t.panel;
  Elements body = {hbox(std::move(top)), (pic | flex | reflect(image_box)) | borderStyled(ROUNDED, frame_c) | flex};
  if (state == State::Preview)
    body.push_back(caption.render(true, t.fg, t.dim) | borderStyled(ROUNDED, t.primary));
  body.push_back(hbox(std::move(bottom)));
  return vbox(std::move(body)) | bgcolor(Color::Black);
}

void CameraModal::close(App& app) {
  state = State::Closing;
  alive = false;
  app.bg([b = app.bridge()] { b->close_camera(); });
  app.pop(this);
}

bool CameraModal::event(App& app, const Event& e) {
  auto self = std::static_pointer_cast<CameraModal>(shared_from_this());
  if (state == State::Preview) {
    if (e == Event::Escape) {  // retake
      state = State::Starting;
      caption.clear();
      image = nullptr;
      app.bg([&app, self] {
        app.bridge()->discard_preview();
        app.post([&app, self] {
          self->state = State::Live;
          self->live_loop(app);
        });
      });
      return true;
    }
    if (e == Event::Return) {
      std::string cap = trim(caption.value());
      state = State::Sending;
      app.bg([&app, self, cap] {
        if (!cap.empty()) app.bridge()->set_caption(cap);
        auto rows = app.bridge()->open_send_to();
        app.post([&app, self, rows] {
          self->state = State::Preview;
          app.push(std::make_shared<SendToModal>(rows, [&app, self](std::optional<std::set<int>> chosen) {
            if (!chosen) {
              app.bg([&app] { app.bridge()->close_send_to(); });
              return;
            }
            self->state = State::Sending;
            app.bg([&app, self, want = *chosen] {
              try {
                auto rows = app.bridge()->set_recipients(want);
                std::set<int> got;
                std::string names;
                for (auto& r : rows)
                  if (r.selected) got.insert(r.idx), names += (names.empty() ? "" : ", ") + r.name;
                if (got != want) throw ActionError("Snapchat's recipient list didn't match your selection; not sent.");
                app.bridge()->send_snap();
                app.post([&app, self, names] {
                  app.notify("👻 Snap sent to " + names);
                  self->close(app);
                });
              } catch (...) {
                app.post([self] { self->state = State::Preview; });
                throw;
              }
            });
          }));
        });
      });
      return true;
    }
    caption.handle(e);
    return true;
  }
  if (e == Event::Escape) return close(app), true;
  if (state != State::Live) return true;
  if (e == Event::Character(" ")) {
    state = State::Capturing;
    app.bg([&app, self] {
      try {
        app.bridge()->capture_photo();
        auto m = app.bridge()->preview_media();
        auto im = img::decode(m.data);
        app.post([self, im] {
          self->state = State::Preview;
          if (im) {
            self->frame = *im;
            self->image = &self->frame;
            ++self->image_version;
          }
        });
      } catch (...) {
        app.post([&app, self] {
          self->state = State::Live;
          self->live_loop(app);
        });
        throw;
      }
    });
    return true;
  }
  if (e == Event::Character("v")) {
    app.notify("Snapchat Web can only take photo snaps — video snaps need the phone app.", true);
    return true;
  }
  if (e == Event::ArrowLeft || e == Event::ArrowRight || is(e, {"h", "l"})) {
    int step = (e == Event::ArrowLeft || is(e, {"h"})) ? -1 : 1;
    app.bg([&app, step] { app.bridge()->camera_lens(step); });
  }
  return true;
}

}  // namespace ghost::ui

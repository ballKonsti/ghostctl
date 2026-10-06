#include "bridge.hpp"

#include "inspect.hpp"
#include "page_js.hpp"
#include "util.hpp"

namespace ghost {

namespace {
constexpr const char* TARGET = "[data-ghostctl-target]";
constexpr const char* VIEWER = "[data-ghostctl-viewer]";

std::string q(std::string_view s) { return json(s).dump(); }  // JS string literal
}  // namespace

SelectorError::SelectorError(const sel::Sel& s, const std::string& context)
    : std::runtime_error("Selector '" + std::string(s.name) + "' (" + std::string(s.css) + ") matched nothing" +
                         (context.empty() ? "" : " while " + context) +
                         ". Snapchat may have changed its layout; run `ghostctl inspect`.") {}

SelectorError::SelectorError(const std::string& name, const std::string& context)
    : std::runtime_error("Selector '" + name + "' matched nothing" + (context.empty() ? "" : " while " + context) +
                         ". Snapchat may have changed its layout; run `ghostctl inspect`.") {}

bool Chat::unread() const { return lower(status).starts_with("new"); }

std::string Chat::streak() const {
  for (auto& e : extras)
    if (e.find("🔥") != std::string::npos) return e;
  return {};
}

const Message* Conversation::find(const std::string& key) const {
  for (auto& it : items)
    if (it.kind == ConvItem::Msg && it.msg.key == key) return &it.msg;
  return nullptr;
}

std::string Media::suffix() const {
  std::string sub = mime.substr(mime.find('/') == std::string::npos ? mime.size() : mime.find('/') + 1);
  sub = sub.substr(0, sub.find(';'));
  if (sub == "x-wav") sub = "wav";
  if (sub == "mpeg") sub = "mp3";
  if (sub == "jpeg") sub = "jpg";
  if (sub.empty()) sub = kind == "video" ? "mp4" : kind == "audio" ? "wav" : "png";
  return "." + sub;
}

// --- parsing ---

std::vector<Chat> parse_feed(const json& j) {
  std::vector<Chat> out;
  if (!j.is_array()) return out;
  for (auto& r : j) {
    Chat c;
    c.id = r.value("id", "");
    c.name = r.value("name", "");
    c.status = r.value("status", "");
    c.badge = r.value("badge", "");
    c.group = r.value("group", false);
    if (r.contains("time") && r["time"].is_string()) c.time = parse_iso(r["time"]);
    for (auto& e : r.value("extras", json::array())) c.extras.push_back(e);
    out.push_back(std::move(c));
  }
  return out;
}

Conversation parse_conv(const json& j) {
  Conversation c;
  c.id = j.value("id", "");
  for (auto& s : j.value("seen_by", json::array())) c.seen_by.push_back(s);
  c.activity = j.value("activity", "");
  c.typing = j.value("typing", false);
  for (auto& it : j.value("items", json::array())) {
    std::string kind = it.value("kind", "");
    if (kind == "date") {
      c.items.push_back({ConvItem::Date, {}, it.value("label", "")});
    } else if (kind == "notice") {
      c.items.push_back({ConvItem::Notice, {}, it.value("text", "")});
    } else {
      Message m;
      m.key = it.value("key", "");
      m.sender = it.value("sender", "");
      if (it.contains("time") && it["time"].is_string()) m.time = parse_iso(it["time"]);
      m.text = it.value("text", "");
      m.quote_sender = it.value("quote_sender", "");
      m.quote_text = it.value("quote_text", "");
      m.snap_status = it.value("snap_status", "");
      m.snap_new = it.value("snap_new", false);
      m.not_supported = it.value("not_supported", false);
      m.duration = it.contains("duration") && it["duration"].is_number() ? it["duration"].get<double>() : 0;
      for (auto& x : it.value("media", json::array())) m.media.push_back(x);
      for (auto& x : it.value("reactions", json::array())) m.reactions.push_back(x);
      c.items.push_back({ConvItem::Msg, std::move(m), ""});
    }
  }
  return c;
}

// --- lifecycle ---

Bridge::Bridge(Browser& b, Handler h, double action_gap) : b_(b), handler_(std::move(h)), gap_(action_gap) {}

Bridge::~Bridge() { stop(); }

bool Bridge::inject() {
  // page.js calls window.__ghostctl_emit(region, payload); route that to the CDP binding.
  b_.eval(
      "window.__ghostctl_emit = window.__ghostctl_emit || ((r, p) => window.__ghostctl_bind && "
      "window.__ghostctl_bind(JSON.stringify([r, p]))); true");
  auto r = b_.eval("(" + std::string(js::page) + ")(" + sel::page_selectors().dump() + ")");
  return r.is_boolean() && r.get<bool>();
}

json Bridge::call(const std::string& fn, json args) {
  std::string expr = "(window.__ghostctl ? window.__ghostctl." + fn + "(..." + args.dump() + ") : '__nopage__')";
  json r = b_.eval(expr);
  if (r.is_string() && r.get<std::string>() == "__nopage__") {
    inject();
    r = b_.eval(expr);
  }
  return r;
}

void Bridge::emit_json(const std::string& region, const json& data) {
  if (data.is_null()) return;
  BridgeEvent ev;
  if (region == "feed") {
    ev.kind = BridgeEvent::Feed;
    ev.feed = parse_feed(data);
  } else if (region == "conv") {
    ev.kind = BridgeEvent::Conv;
    ev.conv = parse_conv(data);
  } else {
    ev.kind = BridgeEvent::Error;
    ev.error = data.is_string() ? data.get<std::string>() : data.dump();
  }
  handler_(std::move(ev));
}

void Bridge::start() {
  b_.on_binding([this](const std::string& payload) {
    auto arr = json::parse(payload, nullptr, false);
    if (!arr.is_array() || arr.size() != 2) return;
    std::string region = arr[0];
    json data = arr[1].is_string() ? json::parse(arr[1].get<std::string>(), nullptr, false) : arr[1];
    if (region == "error") data = arr[1];
    if (!data.is_discarded()) emit_json(region, data);
  });
  dismiss_popups();
  bool feed = false;
  for (int i = 0; i < 40 && !feed; ++i) {
    feed = b_.visible(sel::FEED.css);
    if (!feed) sleep_ms(500);
  }
  if (!feed) throw SelectorError(sel::FEED, "waiting for the chat list");
  try {
    live_ = inject();
  } catch (const CdpError&) {
    live_ = false;
  }
  if (!live_) {
    poller_ = std::thread([this] {
      while (!stop_) {
        try {
          refresh();
        } catch (...) {
        }
        for (int i = 0; i < 40 && !stop_; ++i) sleep_ms(100);
      }
    });
  }
  refresh();
}

void Bridge::stop() {
  stop_ = true;
  if (poller_.joinable()) poller_.join();
  stop_ = false;
}

void Bridge::refresh() {
  emit_json("feed", call("readFeed"));
  emit_json("conv", call("readConversation"));
}

// --- pacing ---

void Bridge::pace(bool clear) {
  double gap = now_epoch() - last_action_;
  if (gap < gap_) sleep_ms(int((gap_ - gap) * 1000));
  last_action_ = now_epoch();
  if (clear) clear_overlay();
}

void Bridge::target(const std::string& key) {
  if (!call("target", {key}).get<bool>())
    throw ActionError("That message is no longer on the page (scrolled away or deleted).");
}

bool Bridge::overlay_open() {
  return b_
      .eval("[...document.querySelectorAll(" + q(sel::OVERLAY.css) +
            ")].some(e => { const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0; })")
      .get<bool>();
}

void Bridge::clear_overlay() {
  // Escape doesn't close Snapchat's pop-ups; a click on their full-viewport backdrop does.
  for (int i = 0; i < 3; ++i) {
    if (!overlay_open()) return;
    b_.click_at(4, 4);
    sleep_ms(300);
  }
}

void Bridge::dismiss_popups() {
  for (int i = 0; i < 8; ++i) {
    if (b_.visible(sel::NOTIFY_NOT_NOW)) {
      b_.click(sel::NOTIFY_NOT_NOW.css);
      return;
    }
    sleep_ms(500);
  }
}

// --- navigation ---

void Bridge::open_chat(const std::string& id) {
  {
    std::lock_guard lk(lock_);
    pace();
    // In-app route change: no reload, and never a click on a "New Snap" row.
    b_.eval("history.pushState({}, '', '/web/' + " + q(id) +
            "); dispatchEvent(new PopStateEvent('popstate', {state: {}})); true");
    bool ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
      ok = b_.count("ul#cv-" + id) > 0;
      if (!ok) sleep_ms(250);
    }
    if (!ok) throw SelectorError(sel::CONV_LIST, "opening chat " + id);
  }
  refresh();
}

void Bridge::close_chat() {
  std::lock_guard lk(lock_);
  pace();
  b_.click(sel::CONV_CLOSE.css);
}

void Bridge::mark_read(const std::string& id) {
  open_chat(id);
  sleep_ms(1500);
  close_chat();
}

void Bridge::load_older() {
  std::lock_guard lk(lock_);
  pace();
  if (!b_.eval("(() => { const ul = document.querySelector(" + q(sel::CONV_LIST.css) +
               "); if (!ul) return false; ul.scrollTop = 0; return true; })()")
           .get<bool>())
    throw SelectorError(sel::CONV_LIST, "loading older messages");
}

void Bridge::load_more_chats() {
  std::lock_guard lk(lock_);
  pace();
  if (!b_.eval("(() => { const f = document.querySelector(" + q(sel::FEED.css) +
               "); if (!f) return false; f.scrollTop = f.scrollHeight; return true; })()")
           .get<bool>())
    throw SelectorError(sel::FEED, "loading more chats");
}

// --- composing ---

static std::string composer_text(Browser& b) {
  auto r = b.eval("(() => { const e = document.querySelector(" + q(sel::COMPOSER.css) +
                  "); return e ? e.innerText : null; })()");
  if (r.is_null()) throw SelectorError(sel::COMPOSER, "finding the message box");
  return r.get<std::string>();
}

static void set_composer(Bridge& br, Browser& b, const std::string& text) {
  br.clear_overlay();
  if (!b.click(sel::COMPOSER.css)) throw SelectorError(sel::COMPOSER, "finding the message box");
  b.key("Control+A");
  b.key("Backspace");
  auto lines = split_lines(text);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) b.key("Shift+Enter");
    if (!lines[i].empty()) b.insert_text(lines[i]);
  }
}

void Bridge::set_draft(const std::string& text) {
  std::lock_guard lk(lock_);
  set_composer(*this, b_, text);
}

void Bridge::send_text(const std::string& raw) {
  std::string text = trim(raw);
  if (text.empty()) return;
  std::lock_guard lk(lock_);
  pace();
  for (int attempt = 0; attempt < 2; ++attempt) {
    set_composer(*this, b_, text);
    bool registered = false;
    for (int i = 0; i < 20 && !registered; ++i) {
      registered = squash(composer_text(b_)) == squash(text);
      if (!registered) sleep_ms(50);
    }
    if (!registered) {  // insertText didn't register: type it with real key events
      b_.click(sel::COMPOSER.css);
      b_.key("Control+A");
      b_.key("Backspace");
      std::string flat = text;
      std::replace(flat.begin(), flat.end(), '\n', ' ');
      b_.type_slow(flat, 8);
    }
    sleep_ms(150);
    b_.key("Enter");
    for (int i = 0; i < 24; ++i) {  // the composer clears once Snapchat accepted it
      sleep_ms(250);
      if (squash(composer_text(b_)).empty()) return;
    }
  }
  throw ActionError("Message not sent: Snapchat didn't accept it (the message box did not clear).");
}

void Bridge::reply(const std::string& key, const std::string& text) {
  open_menu(key);
  choose_menu("Reply");
  sleep_ms(400);
  send_text(text);
}

// --- message menu ---

bool Bridge::menu_visible(const std::string& label) { return b_.visible("text=" + q(label)); }

std::vector<std::string> Bridge::open_menu(const std::string& key) {
  std::lock_guard lk(lock_);
  pace();
  target(key);
  auto p = b_.point(std::string(TARGET) + " > div");
  if (!p) throw ActionError("That message is no longer on the page.");
  b_.click_at(p->x, p->y, "right");
  sleep_ms(600);
  auto reactions = b_.eval("[...new Set([...document.querySelectorAll(" + q(sel::MENU_REACTION.css) +
                           ")].filter(i => i.getBoundingClientRect().width > 0).map(i => i.alt.replace(/^Reaction "
                           "/, '').replace(/ from .*/, '')))]");
  std::vector<std::string> out;
  for (auto& r : reactions) out.push_back("react:" + r.get<std::string>());
  size_t n_react = out.size();
  for (auto label : sel::MENU_ITEMS)
    if (menu_visible(std::string(label))) out.emplace_back(label);
  if (out.size() == n_react && n_react == 0) {
    clear_overlay();
    throw SelectorError("message menu", "reading the right-click menu");
  }
  return out;
}

void Bridge::choose_menu(const std::string& entry) {
  std::lock_guard lk(lock_);
  pace(false);
  std::string css = entry.starts_with("react:")
                        ? "img[alt^='Reaction " + entry.substr(6) + " from ']"
                        : "text=" + q(entry);
  if (b_.click(css)) {
    if (entry != "Delete") {  // Delete may open a confirm dialog
      sleep_ms(400);
      clear_overlay();
    }
    return;
  }
  clear_overlay();
  throw ActionError("Menu entry '" + entry + "' disappeared (the menu closed). Try again.");
}

void Bridge::choose_menu_confirm(const std::string& label) {
  std::lock_guard lk(lock_);
  b_.click("role=button[name=" + q(label) + "]");
}

void Bridge::close_menu() {
  std::lock_guard lk(lock_);
  clear_overlay();
}

// --- media ---

Media Bridge::fetch(const std::string& src, const std::string& kind, const std::string& shot_css) {
  try {
    auto got = call("fetchSrc", {src});
    return {kind, b64decode(got.value("b64", "")), got.value("mime", "")};
  } catch (const CdpError&) {
    // Cross-origin or revoked blob: screenshot the element instead.
    auto r = b_.helper("rect", {shot_css});
    if (!r.is_object()) throw;
    return {"image", b_.screenshot_png({r["x"], r["y"], r["w"], r["h"]}), "image/png"};
  }
}

std::vector<Media> Bridge::message_media(const std::string& key) {
  std::lock_guard lk(lock_);
  target(key);
  std::vector<Media> out;
  for (auto& m : call("targetMedia")) {
    std::string tag = m.value("tag", "");
    std::string kind = tag == "video" ? "video" : tag == "audio" ? "audio" : "image";
    out.push_back(fetch(m.value("src", ""), kind, std::string(TARGET) + " " + tag));
  }
  return out;
}

std::optional<std::pair<Media, ViewerInfo>> Bridge::capture_viewer(double timeout_s) {
  double deadline = now_epoch() + timeout_s;
  while (now_epoch() < deadline) {
    auto v = call("findViewer");
    if (v.is_object()) {
      std::string tag = v.value("tag", ""), src = v.value("src", "");
      if (!src.empty() || tag != "img") {
        if (tag == "video") sleep_ms(300);
        ViewerInfo info{v.value("sender", ""), v["time"].is_string() ? v["time"].get<std::string>() : ""};
        Media media;
        if (!src.empty() && !src.starts_with("data:")) {
          media = fetch(src, tag == "video" ? "video" : "image", VIEWER);
        } else {
          auto r = b_.helper("rect", {VIEWER});
          media = {"image", b_.screenshot_png({r["x"], r["y"], r["w"], r["h"]}), "image/png"};
        }
        return std::pair{media, info};
      }
    }
    sleep_ms(300);
  }
  return std::nullopt;
}

void Bridge::dump(const fs::path& dir, const std::string& label) {
  try {
    dump_page(b_, label, dir);
  } catch (...) {
  }
}

std::pair<Media, ViewerInfo> Bridge::open_snap(const std::string& key, const fs::path* dump_dir) {
  std::lock_guard lk(lock_);
  pace();
  target(key);
  std::string css = std::string(TARGET) + " >> " + std::string(sel::SNAP_CLICK_TO_VIEW.css);
  if (!b_.click(css)) throw ActionError("That message isn't an unopened snap.");
  auto got = capture_viewer();
  if (dump_dir) dump(*dump_dir, "snap-viewer");
  if (!got) throw SelectorError(sel::VIEWER_MEDIA, "waiting for the snap to appear");
  return *got;
}

std::optional<std::pair<Media, ViewerInfo>> Bridge::viewer_next() {
  std::lock_guard lk(lock_);
  pace(false);  // the viewer itself is a pop-up
  auto old = b_.eval("(() => { const e = document.querySelector(" + q(VIEWER) +
                     "); return e ? (e.currentSrc || e.src) : null; })()");
  if (!b_.click(sel::VIEWER_ADVANCE.css)) return std::nullopt;
  for (int i = 0; i < 10; ++i) {
    sleep_ms(400);
    auto v = call("findViewer");
    if (!v.is_object()) return std::nullopt;
    if (!v.value("src", "").empty() && v["src"] != old) return capture_viewer(3);
  }
  return std::nullopt;
}

void Bridge::viewer_react(const std::string& name) {
  std::lock_guard lk(lock_);
  pace(false);
  if (!b_.click("img[alt^='Reaction " + name + " from ']")) throw ActionError("This snap has no reaction bar.");
}

void Bridge::close_viewer() {
  std::lock_guard lk(lock_);
  for (int i = 0; i < 3; ++i) {
    if (!call("findViewer").is_object()) return;
    if (!b_.click(sel::VIEWER_CLOSE.css)) b_.key("Escape");
    sleep_ms(600);
  }
  clear_overlay();
}

std::string Bridge::stories_available() {
  auto r = b_.eval("(() => { const e = document.querySelector(" + q(sel::STORIES.css) +
                   "); return e ? e.textContent : null; })()");
  if (r.is_null()) throw SelectorError(sel::STORIES, "finding the stories tile (close the chat first)");
  return squash(r.get<std::string>());
}

std::pair<Media, ViewerInfo> Bridge::open_stories(const fs::path* dump_dir) {
  std::lock_guard lk(lock_);
  pace();
  b_.click(sel::STORIES.css);
  auto got = capture_viewer();
  if (dump_dir) dump(*dump_dir, "story-viewer");
  if (!got) throw SelectorError(sel::VIEWER_MEDIA, "waiting for the story to appear");
  return *got;
}

std::string Bridge::send_file(const fs::path& path) {
  std::string ext = lower(path.extension().string());
  if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".gif")
    throw ActionError(
        "Snapchat Web only accepts png/jpeg/gif in chat (no video). Snaps come from the camera (c).");
  std::lock_guard lk(lock_);
  pace();
  if (!b_.count(sel::UPLOAD_INPUT.css)) throw SelectorError(sel::UPLOAD_INPUT, "attaching a file");
  auto count_items = [&] { return b_.count("ul[id^='cv-'] li"); };
  int before = count_items();
  b_.set_files(sel::UPLOAD_INPUT.css, fs::absolute(path).string());
  for (int i = 0; i < 24; ++i) {
    sleep_ms(500);
    if (count_items() > before) return "sent";
    if (b_.visible("role=button[name='Send']")) {
      pace(false);
      b_.click("role=button[name='Send']");
      return "sent (confirmed preview)";
    }
  }
  return "attached; no confirmation seen — check the chat";
}

// --- camera ---

bool Bridge::camera_live() {
  return b_.eval("(() => { const v = document.querySelector(" + q(sel::CAMERA_VIDEO.css) +
                 "); return !!(v && v.videoWidth > 0); })()")
      .get<bool>();
}

void Bridge::open_camera() {
  std::lock_guard lk(lock_);
  pace();
  b_.grant_camera();
  if (!b_.click(sel::CAMERA_BUTTON.css)) throw SelectorError(sel::CAMERA_BUTTON, "opening the camera");
  for (int i = 0; i < 10; ++i) {
    sleep_ms(300);
    if (b_.visible(sel::CAMERA_GOT_IT)) {
      b_.click(sel::CAMERA_GOT_IT.css);
      break;
    }
    if (camera_live()) break;
  }
  for (int i = 0; i < 50; ++i) {
    if (camera_live()) return;
    sleep_ms(300);
  }
  throw ActionError("The camera didn't start. Is the webcam in use by another app?");
}

std::optional<std::string> Bridge::camera_frame(int max_width) {
  try {
    auto url = call("frame", {sel::CAMERA_VIDEO.css, max_width});
    if (!url.is_string()) return std::nullopt;
    std::string s = url;
    auto comma = s.find(',');
    if (comma == std::string::npos) return std::nullopt;
    return b64decode(std::string_view(s).substr(comma + 1));
  } catch (const CdpError&) {
    return std::nullopt;
  }
}

void Bridge::camera_lens(int step) {
  std::lock_guard lk(lock_);
  const auto& arrow = step > 0 ? sel::CAMERA_LENS_NEXT : sel::CAMERA_LENS_PREV;
  if (!b_.click(arrow.css) && step > 0) b_.click(sel::CAMERA_LENS.css);  // arrows appear after the first change
}

void Bridge::wait_preview(double timeout_s) {
  double deadline = now_epoch() + timeout_s;
  while (now_epoch() < deadline) {
    if (b_.count(sel::PREVIEW_MEDIA.css)) return;
    sleep_ms(250);
  }
  throw SelectorError(sel::PREVIEW_MEDIA, "waiting for the snap preview");
}

void Bridge::capture_photo() {
  // The shutter only has onClick: a quick press/release on its centre takes a photo.
  std::lock_guard lk(lock_);
  auto p = b_.helper("pointAny", {sel::CAMERA_SHUTTER.css});
  if (!p.is_object()) throw SelectorError(sel::CAMERA_SHUTTER, "finding the shutter");
  b_.click_at(p["x"], p["y"], "left", 150);
  wait_preview();
}

Media Bridge::preview_media() {
  std::lock_guard lk(lock_);
  auto info = b_.eval("(() => { const e = document.querySelector(" + q(sel::PREVIEW_MEDIA.css) +
                      "); return e && {tag: e.tagName.toLowerCase(), src: e.currentSrc || e.src}; })()");
  if (!info.is_object()) throw SelectorError(sel::PREVIEW_MEDIA, "reading the snap preview");
  return fetch(info["src"], info["tag"] == "video" ? "video" : "image", std::string(sel::PREVIEW_MEDIA.css));
}

void Bridge::set_caption(const std::string& text) {
  std::lock_guard lk(lock_);
  if (!b_.visible(sel::PREVIEW_CAPTION.css)) {
    b_.click(sel::PREVIEW_CAPTION_BUTTON.css);
    sleep_ms(300);
  }
  if (!b_.click(sel::PREVIEW_CAPTION.css)) throw SelectorError(sel::PREVIEW_CAPTION, "adding the caption");
  b_.key("Control+A");
  b_.key("Backspace");
  b_.insert_text(text.substr(0, 250));
  b_.key("Escape");  // leave caption editing
}

void Bridge::discard_preview() {
  std::lock_guard lk(lock_);
  if (b_.click(sel::PREVIEW_DISCARD.css)) sleep_ms(500);
}

void Bridge::close_camera() {
  close_send_to();
  discard_preview();
  std::lock_guard lk(lock_);
  for (int i = 0; i < 3; ++i) {
    if (!b_.click(sel::CAMERA_OFF.css)) break;
    sleep_ms(600);
  }
  clear_overlay();
}

// "Best Friends" truncates names and "Recents" repeats people; the A-Z sections,
// "Groups" and "Stories" list everyone exactly once.
std::vector<Recipient> Bridge::recipients() {
  std::vector<Recipient> out;
  auto rows = call("readRecipients", {sel::SENDTO_SELECTED_MARK});
  if (!rows.is_array()) return out;
  for (auto& r : rows) {
    std::string section = r.value("section", "");
    if (section == "Best Friends" || section == "Recents") continue;
    out.push_back({r.value("idx", 0), section, r.value("name", ""), r.value("extra", ""), r.value("selected", false)});
  }
  return out;
}

std::vector<Recipient> Bridge::open_send_to() {
  std::lock_guard lk(lock_);
  b_.click(sel::PREVIEW_SEND_TO.css);
  for (int i = 0; i < 30; ++i) {
    sleep_ms(250);
    auto rows = recipients();
    if (!rows.empty()) return rows;
  }
  throw SelectorError(sel::SENDTO_FORM, "opening the Send To list");
}

std::vector<Recipient> Bridge::set_recipients(const std::set<int>& wanted) {
  std::lock_guard lk(lock_);
  for (auto& r : recipients()) {
    if (r.selected != bool(wanted.count(r.idx))) {
      b_.click("[data-ghostctl-rcpt='" + std::to_string(r.idx) + "'] > div");
      sleep_ms(250);
    }
  }
  return recipients();
}

void Bridge::close_send_to() {
  std::lock_guard lk(lock_);
  if (b_.count(sel::SENDTO_FORM.css)) {
    b_.click(std::string(sel::SENDTO_FORM.css) + " button");  // its first button is "back"
    sleep_ms(500);
  }
}

void Bridge::send_snap() {
  std::lock_guard lk(lock_);
  pace(false);
  if (!b_.click(sel::SENDTO_SUBMIT.css)) throw SelectorError(sel::SENDTO_SUBMIT, "sending the snap");
  for (int i = 0; i < 40; ++i) {
    sleep_ms(250);
    if (!b_.count(sel::SENDTO_FORM.css)) return;
  }
  throw ActionError("Snapchat didn't confirm the snap was sent; check the chat.");
}

}  // namespace ghost

// Turns the Snapchat Web page into events (chat list, conversation) and actions.
//
// page.js (shared with the Python version) reads the chat list and the open
// conversation into JSON; its MutationObserver pushes changes through the
// CDP binding __ghostctl_bind. If that fails, a thread polls instead.
// Actions on one message find it again by key, tag it with
// data-ghostctl-target, and act on that element.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "browser.hpp"

namespace ghost {

struct SelectorError : std::runtime_error {
  explicit SelectorError(const sel::Sel& s, const std::string& context = "");
  explicit SelectorError(const std::string& name, const std::string& context = "");
};

// Something that can't be done (not a broken selector).
struct ActionError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Chat {
  std::string id, name, status, badge;
  double time = 0;
  std::vector<std::string> extras;  // e.g. "941 🔥"
  bool group = false;
  bool unread() const;
  std::string streak() const;
};

struct Message {
  std::string key, sender, text, quote_sender, quote_text, snap_status;
  double time = 0;
  std::vector<std::string> media;      // "image", "video", "audio"
  std::vector<std::string> reactions;  // "love · Konstantin"
  bool snap_new = false, not_supported = false;
  double duration = 0;  // voice note seconds
  bool mine() const { return sender == "Me"; }
};

struct ConvItem {
  enum Kind { Msg, Date, Notice } kind;
  Message msg;
  std::string label;  // date or notice text
};

struct Conversation {
  std::string id;
  std::vector<ConvItem> items;
  std::vector<std::string> seen_by;
  std::string activity;
  bool typing = false;
  const Message* find(const std::string& key) const;
};

struct Media {
  std::string kind;  // "image" | "video" | "audio"
  std::string data;
  std::string mime;
  std::string suffix() const;
};

struct ViewerInfo {
  std::string sender, time;
};

struct Recipient {
  int idx;
  std::string section, name, extra;
  bool selected;
};

struct BridgeEvent {
  enum Kind { Feed, Conv, Error } kind;
  std::vector<Chat> feed;
  Conversation conv;
  std::string error;
};

class Bridge {
 public:
  using Handler = std::function<void(BridgeEvent)>;
  Bridge(Browser& b, Handler h, double action_gap = 1.2);
  ~Bridge();

  void start();
  void stop();
  bool live() const { return live_; }
  void refresh();

  // navigation
  void dismiss_popups();
  void open_chat(const std::string& id);
  void close_chat();
  void mark_read(const std::string& id);
  void load_older();
  void load_more_chats();

  // composing
  void set_draft(const std::string& text);
  void send_text(const std::string& text);
  void reply(const std::string& key, const std::string& text);

  // message menu
  std::vector<std::string> open_menu(const std::string& key);  // "react:love", "Save in Chat", ...
  void choose_menu(const std::string& entry);
  void choose_menu_confirm(const std::string& label);
  void close_menu();

  // media
  std::vector<Media> message_media(const std::string& key);
  std::pair<Media, ViewerInfo> open_snap(const std::string& key, const fs::path* dump_dir = nullptr);
  std::optional<std::pair<Media, ViewerInfo>> viewer_next();
  void viewer_react(const std::string& name);
  void close_viewer();
  std::string stories_available();
  std::pair<Media, ViewerInfo> open_stories(const fs::path* dump_dir = nullptr);
  std::string send_file(const fs::path& path);

  // camera (photo snaps)
  void open_camera();
  bool camera_live();
  std::optional<std::string> camera_frame(int max_width = 540);  // JPEG bytes
  void camera_lens(int step);
  void capture_photo();
  Media preview_media();
  void set_caption(const std::string& text);
  void discard_preview();
  void close_camera();
  std::vector<Recipient> open_send_to();
  std::vector<Recipient> set_recipients(const std::set<int>& wanted);
  void close_send_to();
  void send_snap();

  bool overlay_open();
  void clear_overlay();
  Browser& browser() { return b_; }

 private:
  json call(const std::string& fn, json args = json::array());
  bool inject();
  void emit_json(const std::string& region, const json& data);
  void pace(bool clear = true);
  void target(const std::string& key);
  bool menu_visible(const std::string& label);
  Media fetch(const std::string& src, const std::string& kind, const std::string& shot_css);
  std::optional<std::pair<Media, ViewerInfo>> capture_viewer(double timeout_s = 10);
  void wait_preview(double timeout_s = 15);
  std::vector<Recipient> recipients();
  void dump(const fs::path& dir, const std::string& label);

  Browser& b_;
  Handler handler_;
  double gap_;
  std::atomic<bool> live_{false}, stop_{false};
  std::thread poller_;
  std::recursive_mutex lock_;  // one page action at a time
  double last_action_ = 0;
};

// Parse page.js output.
std::vector<Chat> parse_feed(const json& j);
Conversation parse_conv(const json& j);

}  // namespace ghost

// The TUI. Everything that touches the page runs on background threads; their
// results are posted back to the UI thread, which owns all state below.
#pragma once

#include <atomic>
#include <ftxui/component/app.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bridge.hpp"
#include "browser.hpp"
#include "config.hpp"
#include "emoji.hpp"
#include "term_image.hpp"
#include "ui/textfield.hpp"
#include "ui/theme.hpp"

namespace ghost::ui {

class App;

// "slash" -> "/", "question_mark" -> "?", "down" -> "↓" ...
std::string key_label(const std::string& spec);

// A pop-up or full-screen view on top of the main screen.
struct Modal : std::enable_shared_from_this<Modal> {
  virtual ~Modal() = default;
  virtual ftxui::Element render(App& app) = 0;
  virtual bool event(App& app, const ftxui::Event& e) = 0;
  virtual bool fullscreen() const { return false; }
  // Image shown in `image_box` (viewer, camera). Bump `image_version` on change.
  const img::Image* image = nullptr;
  int image_version = 0;
  ftxui::Box image_box;
  std::atomic<bool> alive{true};
};

struct Pending {
  std::string chat_id, text, failed;
};

enum class Focus { Chats, Conv, Compose, Search };

class App {
 public:
  explicit App(Config cfg);
  ~App();
  // connect=false: UI only (developer previews of modals).
  int run(bool connect = true);

  // --- for modals and background tasks ---
  const Theme& theme() const { return theme_; }
  const Config& cfg() const { return cfg_; }
  Bridge* bridge() { return bridge_.get(); }
  img::Protocol protocol() const { return protocol_; }
  // Run on the UI thread (from any thread).
  void post(std::function<void()> fn);
  // Run on a background thread; exceptions become a status-bar error.
  void bg(std::function<void()> fn);
  void push(std::shared_ptr<Modal> m);
  void pop(Modal* m);
  void notify(const std::string& msg, bool warn = false);
  void status(const std::string& msg, bool error = false);
  void fail(const std::exception& e);
  bool key(const ftxui::Event& e, const std::string& action) const;
  // Block a background thread until the UI answers (login prompts).
  std::optional<std::string> ask(const std::string& prompt, bool secret, std::vector<std::string> messages);
  bool confirm(const std::string& question, const std::string& yes = "yes");

  ftxui::Element dialog(ftxui::Element body, int width = 76) const;
  ftxui::Element hint(const std::string& key, const std::string& label) const;
  std::string chat_name(const std::string& id) const;
  std::string open_id() const { return open_id_; }

 private:
  // rendering
  ftxui::Element render();
  ftxui::Element render_main();
  ftxui::Element render_topbar();
  ftxui::Element render_chats();
  ftxui::Element render_chat_row(const Chat& c, bool selected);
  ftxui::Element render_conv();
  ftxui::Element render_message(const Message& m, bool header, bool selected);
  ftxui::Element render_empty();
  ftxui::Element render_footer();
  void flush_images();

  // events
  bool on_event(const ftxui::Event& e);
  bool on_compose_key(const ftxui::Event& e);
  bool on_search_key(const ftxui::Event& e);
  void on_bridge(BridgeEvent ev);
  void notify_new(const std::vector<Chat>& rows);

  // connection
  void connect();
  bool log_in(std::string error = "");
  void watch_session();

  // actions
  std::vector<const Chat*> visible_chats() const;
  std::vector<const Message*> messages() const;
  const Message* selected_message() const;
  void move(int delta);
  void open_selected_chat();
  void open_chat(const Chat& c);
  void back();
  void compose(std::optional<std::string> reply_key = std::nullopt);
  void end_compose(bool clear_page);
  void submit_compose();
  void send_message(const std::string& chat_id, const std::string& text, std::optional<std::string> reply_key);
  void draft_changed();
  void update_emoji();
  void accept_emoji();
  void message_menu();
  void open_media();
  void play_voice(const Message& m);
  void stop_voice();
  void stories();
  void camera();
  void send_file_picker();
  void confirm_send_file(const fs::path& p);
  void mark_read();
  void settle_pending();

  Config cfg_;
  Theme theme_;
  img::Protocol protocol_;
  std::unique_ptr<ftxui::App> screen_;
  std::unique_ptr<Browser> browser_;
  std::unique_ptr<Bridge> bridge_;
  std::atomic<int> tasks_{0};
  std::atomic<bool> quitting_{false};

  // state (UI thread only)
  std::map<std::string, Chat> chats_;
  std::optional<Conversation> conv_;
  std::string open_id_, selected_chat_, selected_msg_;
  bool follow_end_ = true, feed_seen_ = false, live_ = false, connected_ = false;
  std::string mode_note_, status_;
  bool status_error_ = false;
  Focus focus_ = Focus::Chats;
  TextField compose_{"Send a chat"}, search_{"Search"};
  bool search_open_ = false;
  std::optional<std::string> reply_key_;
  std::vector<Pending> pending_;
  std::map<std::string, bool> saved_;
  std::vector<emoji::Hit> suggestions_;
  std::atomic<int> draft_gen_{0};
  std::string playing_key_;
  int audio_pid_ = -1;
  bool dumped_viewer_ = false;
  struct Toast {
    std::string text;
    bool warn;
    double until;
  };
  std::vector<Toast> toasts_;
  std::vector<std::shared_ptr<Modal>> modals_;
  // images on screen (kitty)
  int shown_version_ = -1;
  ftxui::Box shown_box_{};
  const Modal* shown_modal_ = nullptr;
};

}  // namespace ghost::ui

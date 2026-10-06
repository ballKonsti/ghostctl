// Pop-ups and full-screen views. Each reports its result through a callback
// (called on the UI thread) and removes itself with app.pop(this).
#pragma once

#include <functional>
#include <optional>
#include <set>

#include "ui/app.hpp"

namespace ghost::ui {

struct HelpModal : Modal {
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
};

struct ConfirmModal : Modal {
  ConfirmModal(std::string q, std::string yes, std::function<void(bool)> cb)
      : question(std::move(q)), yes(std::move(yes)), cb(std::move(cb)) {}
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  std::string question, yes;
  std::function<void(bool)> cb;
};

struct MenuModal : Modal {
  struct Option {
    std::string id;
    ftxui::Element label;
  };
  MenuModal(ftxui::Element title, std::vector<Option> opts, std::function<void(std::optional<std::string>)> cb)
      : title(std::move(title)), options(std::move(opts)), cb(std::move(cb)) {}
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  ftxui::Element title;
  std::vector<Option> options;
  std::function<void(std::optional<std::string>)> cb;
  int sel = 0;
};

struct PromptModal : Modal {
  PromptModal(std::string prompt, bool secret, std::vector<std::string> messages,
              std::function<void(std::optional<std::string>)> cb);
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  std::vector<std::string> messages;
  TextField field;
  std::function<void(std::optional<std::string>)> cb;
};

struct LoginForm {
  std::string user, password;
  bool remember;
};

struct LoginModal : Modal {
  LoginModal(bool can_remember, std::string error, std::function<void(std::optional<LoginForm>)> cb);
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  bool fullscreen() const override { return true; }
  TextField user{"Username or email"}, pw{"Password", true};
  bool remember, can_remember;
  int focus = 0;  // 0 user, 1 password, 2 remember
  std::string error;
  std::function<void(std::optional<LoginForm>)> cb;
};

struct EmojiModal : Modal {
  explicit EmojiModal(std::function<void(std::optional<std::string>)> cb);
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  TextField query{"fire, herz, lach, skull…"};
  std::vector<emoji::Hit> results;
  int sel = 0;
  std::function<void(std::optional<std::string>)> cb;
};

struct FilePickerModal : Modal {
  explicit FilePickerModal(std::function<void(std::optional<fs::path>)> cb);
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  void list();
  fs::path dir;
  std::vector<fs::directory_entry> entries;
  TextField path{"~/Pictures/photo.jpg — or pick below"};
  int sel = 0;
  bool typing = false;
  std::function<void(std::optional<fs::path>)> cb;
};

// Full-screen snap / story / photo viewer.
struct ViewerModal : Modal {
  ViewerModal(std::vector<std::pair<Media, ViewerInfo>> items, std::string kind, bool viewer);
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  bool fullscreen() const override { return true; }
  void show(App& app);
  void close(App& app);
  std::vector<std::pair<Media, ViewerInfo>> items;
  std::string kind;
  bool viewer;
  size_t index = 0;
  int shown = -1;  // index currently decoded into `current`
  img::Image current;
  fs::path path;
  bool busy = false;
};

struct SendToModal : Modal {
  SendToModal(std::vector<Recipient> rows, std::function<void(std::optional<std::set<int>>)> cb);
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  std::vector<const Recipient*> shown() const;
  std::vector<Recipient> rows;
  std::set<int> chosen;
  TextField filter{"search friends, groups, My Story"};
  bool typing = false;
  int sel = 0;
  std::function<void(std::optional<std::set<int>>)> cb;
};

// Live webcam preview, capture, caption and send — through Snapchat's camera.
struct CameraModal : Modal {
  explicit CameraModal(std::string chat_name) : chat(std::move(chat_name)) {}
  ftxui::Element render(App& app) override;
  bool event(App& app, const ftxui::Event& e) override;
  bool fullscreen() const override { return true; }
  void start(App& app);
  void live_loop(App& app);
  void close(App& app);
  std::string chat;
  enum class State { Starting, Live, Capturing, Preview, Sending, Closing } state = State::Starting;
  img::Image frame;
  TextField caption{"Add a caption (optional) — enter to choose recipients"};
};

}  // namespace ghost::ui

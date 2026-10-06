// A one-line text editor: UTF-8 / emoji aware (moves by grapheme), optional
// password mode. The app routes keys to it, so tab and ctrl+e stay free.
#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>

namespace ghost::ui {

class TextField {
 public:
  explicit TextField(std::string placeholder = "", bool secret = false)
      : placeholder(std::move(placeholder)), secret(secret) {}

  // Editing keys (characters, backspace, delete, arrows, home/end, ctrl+u, ctrl+w).
  // Returns true if the event was used.
  bool handle(const ftxui::Event& e);
  ftxui::Element render(bool focused, ftxui::Color fg, ftxui::Color dim) const;

  const std::string& value() const { return value_; }
  void set(std::string v);
  void insert(const std::string& s);
  void clear() { set(""); }
  bool empty() const { return value_.empty(); }

  std::string placeholder;
  bool secret;

 private:
  std::vector<std::string> glyphs() const;
  void from_glyphs(const std::vector<std::string>& g);

  std::string value_;
  size_t cursor_ = 0;  // in glyphs
};

}  // namespace ghost::ui

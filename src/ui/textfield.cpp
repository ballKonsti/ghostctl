#include "ui/textfield.hpp"

#include "ui/wtext.hpp"

namespace ghost::ui {

using namespace ftxui;

std::vector<std::string> TextField::glyphs() const {
  std::vector<std::string> g;
  for (auto& c : clusters(value_)) g.push_back(c.text);
  return g;
}

void TextField::from_glyphs(const std::vector<std::string>& g) {
  value_.clear();
  for (auto& s : g) value_ += s;
}

void TextField::set(std::string v) {
  value_ = std::move(v);
  cursor_ = glyphs().size();
}

void TextField::insert(const std::string& s) {
  auto g = glyphs();
  std::string before, after;
  for (size_t i = 0; i < g.size(); ++i) (i < cursor_ ? before : after) += g[i];
  value_ = before + s + after;
  cursor_ = clusters(before + s).size();
}

bool TextField::handle(const Event& e) {
  auto g = glyphs();
  cursor_ = std::min(cursor_, g.size());
  if (e.is_character()) {
    insert(e.character());
    return true;
  }
  if (e == Event::Backspace) {
    if (cursor_ > 0) {
      g.erase(g.begin() + long(--cursor_));
      from_glyphs(g);
    }
    return true;
  }
  if (e == Event::Delete) {
    if (cursor_ < g.size()) {
      g.erase(g.begin() + long(cursor_));
      from_glyphs(g);
    }
    return true;
  }
  if (e == Event::ArrowLeft) return cursor_ = cursor_ ? cursor_ - 1 : 0, true;
  if (e == Event::ArrowRight) return cursor_ = std::min(cursor_ + 1, g.size()), true;
  if (e == Event::Home) return cursor_ = 0, true;
  if (e == Event::End) return cursor_ = g.size(), true;
  if (e.input() == std::string(1, 21)) {  // ctrl+u: clear to start
    g.erase(g.begin(), g.begin() + long(cursor_));
    cursor_ = 0;
    from_glyphs(g);
    return true;
  }
  if (e.input() == std::string(1, 23)) {  // ctrl+w: delete previous word
    size_t i = cursor_;
    while (i > 0 && g[i - 1] == " ") --i;
    while (i > 0 && g[i - 1] != " ") --i;
    g.erase(g.begin() + long(i), g.begin() + long(cursor_));
    cursor_ = i;
    from_glyphs(g);
    return true;
  }
  return false;
}

Element TextField::render(bool focused, Color fg, Color dim) const {
  auto g = glyphs();
  if (g.empty()) {
    auto ph = wtext(placeholder) | color(dim);
    return focused ? hbox({wtext(" ") | inverted, ph}) : ph;
  }
  std::string before, at, after;
  for (size_t i = 0; i < g.size(); ++i) {
    std::string s = secret ? "•" : g[i];
    (i < cursor_ ? before : i == cursor_ ? at : after) += s;
  }
  if (!focused) return wtext(before + at + after) | color(fg);
  if (at.empty()) at = " ";
  return hbox({wtext(before), wtext(at) | inverted, wtext(after)}) | color(fg);
}

}  // namespace ghost::ui

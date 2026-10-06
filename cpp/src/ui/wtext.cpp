#include "ui/wtext.hpp"

#include <ftxui/dom/flexbox_config.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

namespace ghost::ui {

namespace {

// Decode one UTF-8 code point at s[i]; advances i.
char32_t next_cp(std::string_view s, size_t& i) {
  unsigned char c = s[i];
  int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  char32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
  for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
  i += len;
  return cp;
}

bool in(char32_t c, char32_t a, char32_t b) { return c >= a && c <= b; }
bool regional(char32_t c) { return in(c, 0x1F1E6, 0x1F1FF); }
bool skin_tone(char32_t c) { return in(c, 0x1F3FB, 0x1F3FF); }

// Code points that attach to the previous cluster.
bool extends(char32_t c) {
  return in(c, 0x0300, 0x036F) || in(c, 0x1AB0, 0x1AFF) || in(c, 0x1DC0, 0x1DFF) || in(c, 0x20D0, 0x20FF) ||
         in(c, 0xFE20, 0xFE2F) || in(c, 0xFE00, 0xFE0F) || in(c, 0xE0100, 0xE01EF) || c == 0x200D ||
         skin_tone(c) || in(c, 0xE0020, 0xE007F) || c == 0x20E3;
}

}  // namespace

std::vector<Cluster> clusters(std::string_view s) {
  std::vector<Cluster> out;
  std::vector<char32_t> cps;  // code points of the current cluster
  std::string cur;
  auto flush = [&] {
    if (cur.empty()) return;
    int w;
    bool vs15 = false, emojiish = false;
    int ri = 0;
    for (char32_t c : cps) {
      vs15 |= c == 0xFE0E;
      emojiish |= c == 0xFE0F || c == 0x200D || skin_tone(c) || c == 0x20E3;
      ri += regional(c);
    }
    // Width of the base code point, as FTXUI (wcwidth-style) sees it.
    size_t j = 0;
    std::string base;
    char32_t b = next_cp(cur, j);
    base = std::string(cur.substr(0, j));
    int bw = b < 0x20 ? 0 : ftxui::string_width(base);
    // Emoji blocks are wide even when the table predates them (e.g. 🩷, Unicode 15).
    if (in(b, 0x1F300, 0x1F5FF) || in(b, 0x1F600, 0x1F64F) || in(b, 0x1F680, 0x1F6FF) ||
        in(b, 0x1F900, 0x1F9FF) || in(b, 0x1FA70, 0x1FAFF))
      bw = 2;
    if (vs15) w = 1;
    else if (emojiish || ri >= 2) w = 2;
    else w = std::clamp(bw, 0, 2);
    if (b == '\t') w = 1, cur = " ";
    out.push_back({cur, w});
    cur.clear();
    cps.clear();
  };
  size_t i = 0;
  while (i < s.size()) {
    size_t start = i;
    char32_t c = next_cp(s, i);
    std::string_view piece = s.substr(start, i - start);
    bool join = !cps.empty() && (extends(c) || cps.back() == 0x200D ||
                                 (regional(c) && cps.size() == 1 && regional(cps[0])));
    if (!join) flush();
    cps.push_back(c);
    cur += piece;
  }
  flush();
  return out;
}

int display_width(std::string_view s) {
  int w = 0;
  for (auto& c : clusters(s)) w += c.width;
  return w;
}

std::string fit_width(const std::string& s, int width) {
  if (display_width(s) <= width) return s;
  std::string out;
  int w = 0;
  for (auto& c : clusters(s)) {
    if (w + c.width > width - 1) break;
    out += c.text;
    w += c.width;
  }
  return out + "…";
}

namespace {

class WText : public ftxui::Node {
 public:
  explicit WText(std::string_view s) : cells_(clusters(s)) {
    int w = 0;
    for (auto& c : cells_) w += c.width;
    requirement_.min_x = w;
    requirement_.min_y = 1;
  }

  void Render(ftxui::Screen& screen) override {
    const auto vis = ftxui::Box::Intersection(screen.stencil, box_);
    if (vis.IsEmpty() || box_.y_min < vis.y_min || box_.y_min > vis.y_max) return;
    int x = box_.x_min, y = box_.y_min;
    for (auto& c : cells_) {
      if (c.width == 0) continue;
      if (x + c.width - 1 > box_.x_max) break;
      if (x >= vis.x_min && x + c.width - 1 <= vis.x_max) {
        screen.PixelAt(x, y).character = c.text;
        if (c.width == 2) screen.PixelAt(x + 1, y).character = "";  // continuation of a wide cell
      }
      x += c.width;
    }
  }

 private:
  std::vector<Cluster> cells_;
};

}  // namespace

ftxui::Element wtext(std::string_view s) { return std::make_shared<WText>(s); }

ftxui::Element wparagraph(std::string_view s) {
  static const auto config = ftxui::FlexboxConfig().SetGap(1, 0);
  ftxui::Elements lines;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find('\n', start);
    std::string_view line = s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    ftxui::Elements words;
    size_t ws = 0;
    while (ws <= line.size()) {
      size_t sp = line.find(' ', ws);
      std::string_view word = line.substr(ws, sp == std::string_view::npos ? std::string_view::npos : sp - ws);
      if (!word.empty()) words.push_back(wtext(word));
      if (sp == std::string_view::npos) break;
      ws = sp + 1;
    }
    if (words.empty()) words.push_back(wtext(" "));
    lines.push_back(ftxui::flexbox(std::move(words), config));
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return lines.size() == 1 ? lines[0] : ftxui::vbox(std::move(lines));
}

}  // namespace ghost::ui

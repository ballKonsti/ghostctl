#include "ui/theme.hpp"

#include <cstdio>

#include "util.hpp"

namespace ghost::ui {

using ftxui::Color;

static Color hex(unsigned v) { return Color::RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

Theme theme_named(const std::string& name) {
  // bg, surface, panel, boost, fg, dim, primary, red, blue, purple, green, warn
  if (name == "nord")
    return {hex(0x2E3440), hex(0x3B4252), hex(0x434C5E), hex(0x4C566A), hex(0xECEFF4), hex(0x9AA3B5), hex(0x88C0D0),
            hex(0xBF616A), hex(0x81A1C1), hex(0xB48EAD), hex(0xA3BE8C), hex(0xEBCB8B)};
  if (name == "gruvbox")
    return {hex(0x1D2021), hex(0x282828), hex(0x3C3836), hex(0x504945), hex(0xEBDBB2), hex(0xA89984), hex(0xFABD2F),
            hex(0xFB4934), hex(0x83A598), hex(0xD3869B), hex(0xB8BB26), hex(0xFE8019)};
  if (name == "dracula")
    return {hex(0x21222C), hex(0x282A36), hex(0x343746), hex(0x44475A), hex(0xF8F8F2), hex(0x9EA3C0), hex(0xBD93F9),
            hex(0xFF5555), hex(0x8BE9FD), hex(0xFF79C6), hex(0x50FA7B), hex(0xF1FA8C)};
  if (name == "tokyo-night")
    return {hex(0x16161E), hex(0x1A1B26), hex(0x24283B), hex(0x2F3549), hex(0xC0CAF5), hex(0x7F86A8), hex(0x7AA2F7),
            hex(0xF7768E), hex(0x7DCFFF), hex(0xBB9AF7), hex(0x9ECE6A), hex(0xE0AF68)};
  if (name == "catppuccin-mocha")
    return {hex(0x11111B), hex(0x1E1E2E), hex(0x313244), hex(0x45475A), hex(0xCDD6F4), hex(0x9399B2), hex(0xF5C2E7),
            hex(0xF38BA8), hex(0x89B4FA), hex(0xCBA6F7), hex(0xA6E3A1), hex(0xF9E2AF)};
  if (name == "monokai")
    return {hex(0x1E1F1C), hex(0x272822), hex(0x3E3D32), hex(0x49483E), hex(0xF8F8F2), hex(0x9D9A85), hex(0xE6DB74),
            hex(0xF92672), hex(0x66D9EF), hex(0xAE81FF), hex(0xA6E22E), hex(0xFD971F)};
  // ghost: Snapchat's palette.
  return {hex(0x0B0B0F), hex(0x13131A), hex(0x1D1D26), hex(0x2C2C3A), hex(0xECECF1), hex(0x8A8A99), hex(0xFFFC00),
          hex(0xF23C57), hex(0x2EA8FF), hex(0xA05DCD), hex(0x2FD07A), hex(0xFFB224)};
}

Color parse_color(const std::string& s0, Color fallback) {
  std::string s = trim(s0);
  if (s.starts_with("bold ")) s = s.substr(5);
  unsigned v;
  if (s.size() == 7 && s[0] == '#' && std::sscanf(s.c_str() + 1, "%6x", &v) == 1) return hex(v);
  return fallback;
}

StatusStyle status_style(const std::string& status, const Theme& t) {
  std::string s = lower(status);
  struct Row {
    const char* key;
    const char* icon;
    Color color;
    bool bold;
  };
  const Row rows[] = {
      {"new snap", "■", t.red, true},     {"new video", "■", t.purple, true}, {"new chat", "■", t.blue, true},
      {"new", "■", t.blue, true},         {"typing", "✎", t.primary, true},   {"call active", "✆", t.green, true},
      {"missed", "✆", t.red, false},      {"call", "✆", t.green, false},      {"delivered", "➤", t.blue, false},
      {"sent", "➤", t.blue, false},       {"opened", "▷", t.dim, false},      {"received", "□", t.dim, false},
      {"screenshot", "⧉", t.primary, false}, {"replayed", "↻", t.dim, false}, {"saved", "◆", t.dim, false},
  };
  for (auto& r : rows)
    if (s.find(r.key) != std::string::npos) return {r.icon, r.color, r.bold};
  return {"·", t.dim, false};
}

const char* const REACTION_ORDER[8] = {"love", "laugh", "fire", "thumbs-up", "thumbs-down", "cry", "shock",
                                       "question mark"};

const char* reaction_emoji(const std::string& n) {
  static const std::pair<const char*, const char*> map[] = {
      {"love", "❤️"}, {"laugh", "😂"},      {"fire", "🔥"}, {"thumbs-up", "👍"},
      {"thumbs-down", "👎"}, {"cry", "😢"}, {"shock", "😮"}, {"question mark", "❓"}};
  for (auto& [k, v] : map)
    if (n == k) return v;
  return "•";
}

std::string reaction_label(const std::string& r) {
  auto sep = r.find(" · ");
  std::string name = r.substr(0, sep), who = sep == std::string::npos ? "" : r.substr(sep + 4);
  std::string e = reaction_emoji(name);
  return trim((e == "•" ? name : e) + " " + who);
}

const char* const GHOST_ART[10] = {
    "      ▄▄██████▄▄      ", "    ▄████████████▄    ", "   ████████████████   ", "   ████████████████   ",
    "   ████████████████   ", " ▄▄████████████████▄▄ ", "  ▀████████████████▀  ", "   ████████████████   ",
    "   ████▀▀████▀▀████   ", "   ▀▀▀    ▀▀    ▀▀▀   ",
};

}  // namespace ghost::ui

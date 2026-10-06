// Look and feel: palettes, Snapchat-style status icons, reactions, the ghost.
#pragma once

#include <ftxui/screen/color.hpp>
#include <string>

namespace ghost::ui {

struct Theme {
  ftxui::Color bg, surface, panel, boost, fg, dim, primary;
  ftxui::Color red, blue, purple, green, warn;
};

// "ghost" (Snapchat colours), "nord", "gruvbox", "dracula", "tokyo-night",
// "catppuccin-mocha", "monokai". Unknown names fall back to ghost.
Theme theme_named(const std::string& name);
// "#RRGGBB" (or a theme colour name like "red"/"blue") -> colour.
ftxui::Color parse_color(const std::string& s, ftxui::Color fallback);

struct StatusStyle {
  const char* icon;
  ftxui::Color color;
  bool bold;
};
// Mirrors Snapchat's icons: filled = new/unopened, arrow = sent, hollow = opened.
StatusStyle status_style(const std::string& status, const Theme& t);

// "love" -> "❤️" (the eight reactions Snapchat offers).
const char* reaction_emoji(const std::string& name);
extern const char* const REACTION_ORDER[8];
// "love · Konstantin" -> "❤️ Konstantin"
std::string reaction_label(const std::string& r);

extern const char* const GHOST_ART[10];

}  // namespace ghost::ui

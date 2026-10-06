// Text with correct emoji widths. FTXUI measures "♥️" as 1 cell and
// "👨‍👩‍👧" as 8 and splits skin tones/flags; terminals like kitty draw all of
// them as 2 cells. These elements cluster graphemes themselves (variation
// selectors, ZWJ sequences, skin tones, flags, keycaps, tags, combining marks)
// and place wide clusters into two screen cells.
#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ghost::ui {

struct Cluster {
  std::string text;
  int width;  // 1 or 2 (0 = invisible)
};

std::vector<Cluster> clusters(std::string_view s);
int display_width(std::string_view s);
// Shorten to `width` cells, ending with "…".
std::string fit_width(const std::string& s, int width);

ftxui::Element wtext(std::string_view s);
// Word-wrapped text (lines split on '\n').
ftxui::Element wparagraph(std::string_view s);

}  // namespace ghost::ui

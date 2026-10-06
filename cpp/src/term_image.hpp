// Images in the terminal: decode, scale, and draw with the kitty graphics
// protocol (kitty, WezTerm, Ghostty) or coloured half-block characters.
#pragma once

#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ghost::img {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  bool empty() const { return w == 0 || h == 0; }
};

enum class Protocol { Kitty, Halfblock, None };

// "auto" picks kitty graphics on terminals that speak it, else half-blocks.
Protocol choose(const std::string& config_value);
const char* name(Protocol p);

std::optional<Image> decode(const std::string& bytes);
std::optional<Image> load(const std::string& path);
Image resize(const Image& src, int w, int h);

struct CellSize {
  int w = 10, h = 20;  // pixels per terminal cell
};
CellSize cell_size();
// Largest cell box with the image's aspect ratio that fits max_cols x max_rows.
std::pair<int, int> fit_cells(int img_w, int img_h, int max_cols, int max_rows);

// kitty: send image data as image `id`, scaled for a cols x rows cell box
// (replaces earlier data with that id).
std::string kitty_transmit(const Image& im, int id, int cols, int rows);
// kitty: show image `id` in the cell box at (col,row) (0-based); moves an
// earlier placement of it.
std::string kitty_put(int id, int col, int row, int cols, int rows);
std::string kitty_delete(int id);
std::string kitty_delete_all();

// Half-block rendering as an FTXUI element of cols x rows cells.
ftxui::Element halfblock(const Image& im, int cols, int rows);

}  // namespace ghost::img

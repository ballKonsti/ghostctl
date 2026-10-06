#include "term_image.hpp"

#include <sys/ioctl.h>
#include <unistd.h>
#include <webp/decode.h>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "media.hpp"
#include "util.hpp"

namespace ghost::img {

Protocol choose(const std::string& v) {
  if (v == "none") return Protocol::None;
  if (v == "kitty") return Protocol::Kitty;
  if (v == "halfblock" || v == "unicode" || v == "sixel" || v == "iterm") return Protocol::Halfblock;
  auto term = media::detect_terminal();
  const char* tp = std::getenv("TERM_PROGRAM");
  if (term == "kitty" || term == "WezTerm" || term == "ghostty" || (tp && std::string(tp) == "ghostty"))
    return Protocol::Kitty;
  return Protocol::Halfblock;
}

const char* name(Protocol p) {
  switch (p) {
    case Protocol::Kitty: return "kitty";
    case Protocol::Halfblock: return "halfblock";
    default: return "none";
  }
}

std::optional<Image> decode(const std::string& bytes) {
  Image im;
  int n = 0;
  if (unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()),
                                                &im.w, &im.h, &n, 4)) {
    im.rgba.assign(px, px + size_t(im.w) * im.h * 4);
    stbi_image_free(px);
    return im;
  }
  if (uint8_t* px = WebPDecodeRGBA(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &im.w, &im.h)) {
    im.rgba.assign(px, px + size_t(im.w) * im.h * 4);
    WebPFree(px);
    return im;
  }
  return std::nullopt;
}

std::optional<Image> load(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::stringstream ss;
  ss << f.rdbuf();
  return decode(ss.str());
}

Image resize(const Image& src, int w, int h) {
  if (w <= 0 || h <= 0 || src.empty()) return {};
  if (w == src.w && h == src.h) return src;
  Image out{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
  stbir_resize_uint8_srgb(src.rgba.data(), src.w, src.h, 0, out.rgba.data(), w, h, 0, STBIR_RGBA);
  return out;
}

CellSize cell_size() {
  winsize ws{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row && ws.ws_xpixel && ws.ws_ypixel)
    return {ws.ws_xpixel / ws.ws_col, ws.ws_ypixel / ws.ws_row};
  return {};
}

std::pair<int, int> fit_cells(int img_w, int img_h, int max_cols, int max_rows) {
  if (img_w <= 0 || img_h <= 0) return {max_cols, max_rows};
  auto cs = cell_size();
  double scale = std::min(double(max_cols) * cs.w / img_w, double(max_rows) * cs.h / img_h);
  return {std::max(1, int(img_w * scale / cs.w)), std::max(1, int(img_h * scale / cs.h))};
}

std::string kitty_transmit(const Image& im0, int id, int cols, int rows) {
  // Send roughly the pixels the box can show: smaller payloads, smoother live video.
  auto cs = cell_size();
  int tw = std::min(im0.w, cols * cs.w), th = std::min(im0.h, rows * cs.h);
  Image im = (tw < im0.w || th < im0.h) ? resize(im0, tw, th) : im0;

  uLongf zlen = compressBound(im.rgba.size());
  std::string z(zlen, '\0');
  compress2(reinterpret_cast<Bytef*>(z.data()), &zlen, im.rgba.data(), im.rgba.size(), 1);
  z.resize(zlen);
  std::string b64 = b64encode(z);

  std::string out;
  for (size_t i = 0; i < b64.size(); i += 4096) {
    bool first = i == 0, more = i + 4096 < b64.size();
    out += "\x1b_G";
    if (first)
      out += "a=t,f=32,o=z,q=2,i=" + std::to_string(id) + ",s=" + std::to_string(im.w) + ",v=" +
             std::to_string(im.h) + ",";
    out += more ? "m=1" : "m=0";
    out += ";" + b64.substr(i, 4096) + "\x1b\\";
  }
  return out;
}

std::string kitty_put(int id, int col, int row, int cols, int rows) {
  return "\x1b" "7\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H\x1b_Ga=p,q=2,C=1,i=" +
         std::to_string(id) + ",p=1,c=" + std::to_string(cols) + ",r=" + std::to_string(rows) + "\x1b\\\x1b" "8";
}

std::string kitty_delete(int id) { return "\x1b_Ga=d,d=I,q=2,i=" + std::to_string(id) + "\x1b\\"; }
std::string kitty_delete_all() { return "\x1b_Ga=d,d=A,q=2\x1b\\"; }

ftxui::Element halfblock(const Image& src, int cols, int rows) {
  using namespace ftxui;
  Image im = resize(src, cols, rows * 2);
  Elements lines;
  for (int r = 0; r < rows; ++r) {
    Elements cells;
    for (int c = 0; c < cols; ++c) {
      const uint8_t* top = &im.rgba[(size_t(2 * r) * im.w + c) * 4];
      const uint8_t* bot = &im.rgba[(size_t(2 * r + 1) * im.w + c) * 4];
      cells.push_back(text("▀") | color(Color::RGB(top[0], top[1], top[2])) |
                      bgcolor(Color::RGB(bot[0], bot[1], bot[2])));
    }
    lines.push_back(hbox(std::move(cells)));
  }
  return vbox(std::move(lines));
}

}  // namespace ghost::img

#include "emoji.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <tuple>
#include <unordered_map>

#include "config.hpp"
#include "util.hpp"

namespace ghost::emoji {

namespace {

struct Entry {
  const char* emoji;
  const char* names;  // '|' separated
};

const Entry TABLE[] = {
#include "emoji_data.inc"
};

struct Parsed {
  std::string emoji;
  std::vector<std::string> names;
};

const std::vector<Parsed>& index() {
  static const std::vector<Parsed> idx = [] {
    std::vector<Parsed> v;
    v.reserve(std::size(TABLE));
    for (auto& e : TABLE) {
      Parsed p{e.emoji, {}};
      std::string all = e.names;
      size_t start = 0;
      while (true) {
        size_t bar = all.find('|', start);
        p.names.push_back(all.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) break;
        start = bar + 1;
      }
      v.push_back(std::move(p));
    }
    return v;
  }();
  return idx;
}

const std::unordered_map<std::string, std::string>& by_name() {
  static const auto table = [] {
    std::unordered_map<std::string, std::string> t;
    for (auto& p : index())
      for (auto& n : p.names) {
        std::string under = n, joined;
        std::replace(under.begin(), under.end(), ' ', '_');
        for (char c : n)
          if (c != ' ') joined += c;
        t.try_emplace(under, p.emoji);
        t.try_emplace(joined, p.emoji);
      }
    return t;
  }();
  return table;
}

bool word_start(const std::string& name, const std::string& q) {
  size_t pos = 0;
  while ((pos = name.find(q, pos)) != std::string::npos) {
    if (pos == 0 || name[pos - 1] == ' ') return true;
    ++pos;
  }
  return false;
}

fs::path recent_path() { return ghost_home() / "recent_emoji.json"; }

}  // namespace

std::vector<Hit> search(const std::string& query, size_t limit) {
  std::string q = lower(trim(query));
  while (!q.empty() && q.front() == ':') q.erase(q.begin());
  while (!q.empty() && q.back() == ':') q.pop_back();
  std::replace(q.begin(), q.end(), '_', ' ');
  std::vector<Hit> out;
  if (q.empty()) {
    for (auto& e : recent()) {
      std::string name;
      for (auto& p : index())
        if (p.emoji == e) name = p.names[0];
      out.push_back({e, name});
    }
    return out;
  }
  // rank, (flag or skin tone), not recent, name length, table order, name
  std::vector<std::tuple<int, bool, bool, size_t, size_t, std::string>> scored;
  const auto& idx = index();
  auto rec = recent();
  for (size_t i = 0; i < idx.size(); ++i) {
    int best = 99;
    std::string best_name;
    for (auto& n : idx[i].names) {
      int rank = n == q ? 0 : n.starts_with(q) ? 1 : word_start(n, q) ? 2 : n.find(q) != std::string::npos ? 3 : 99;
      if (rank < best) best = rank, best_name = n;
    }
    if (best == 99) continue;
    // Skin-tone variants and flags (regional indicators U+1F1E6..) rank after plain emoji.
    bool rare = idx[i].names[0].find("skin tone") != std::string::npos ||
                idx[i].emoji.starts_with("\xF0\x9F\x87") || idx[i].names[0].starts_with("flag");
    bool not_recent = std::find(rec.begin(), rec.end(), idx[i].emoji) == rec.end();
    scored.emplace_back(best, rare, not_recent, best_name.size(), i, best_name);
  }
  std::sort(scored.begin(), scored.end());
  for (size_t k = 0; k < scored.size() && k < limit; ++k)
    out.push_back({idx[std::get<4>(scored[k])].emoji, std::get<5>(scored[k])});
  return out;
}

std::optional<std::string> lookup(const std::string& code) {
  std::string c = lower(code);
  auto& t = by_name();
  if (auto it = t.find(c); it != t.end()) return it->second;
  std::replace(c.begin(), c.end(), '-', '_');
  if (auto it = t.find(c); it != t.end()) return it->second;
  return std::nullopt;
}

static bool code_char(unsigned char c) { return c != ':' && c != ' ' && c != '\n' && c != '\t'; }

std::string emojize(const std::string& text) {
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == ':') {
      size_t j = i + 1;
      while (j < text.size() && code_char(text[j])) ++j;
      if (j < text.size() && text[j] == ':' && j - i - 1 >= 2) {
        if (auto e = lookup(text.substr(i + 1, j - i - 1))) {
          out += *e;
          i = j + 1;
          continue;
        }
      }
    }
    out += text[i++];
  }
  return out;
}

std::optional<std::pair<std::string, size_t>> trailing_partial(const std::string& text) {
  size_t i = text.size();
  while (i > 0 && code_char(text[i - 1])) --i;
  if (i == 0 || text[i - 1] != ':') return std::nullopt;
  size_t colon = i - 1;
  if (colon > 0 && text[colon - 1] != ' ') return std::nullopt;
  std::string word = text.substr(i);
  if (word.size() < 2) return std::nullopt;
  return std::pair{word, colon};
}

std::optional<std::pair<std::string, size_t>> trailing_code(const std::string& text) {
  if (text.size() < 4 || text.back() != ':') return std::nullopt;
  size_t end = text.size() - 1, i = end;
  while (i > 0 && code_char(text[i - 1])) --i;
  if (i == 0 || text[i - 1] != ':' || end - i < 2) return std::nullopt;
  return std::pair{text.substr(i, end - i), i - 1};
}

std::vector<std::string> recent() {
  std::ifstream f(recent_path());
  auto j = nlohmann::json::parse(f, nullptr, false);
  if (j.is_array() && !j.empty()) {
    std::vector<std::string> v;
    for (auto& e : j)
      if (e.is_string()) v.push_back(e);
    return v;
  }
  return {"😂", "❤️", "🔥", "👍", "😭", "🥺", "😊", "🙏", "💀", "🤣", "✨", "😍"};
}

void remember(const std::string& e) {
  auto items = recent();
  items.erase(std::remove(items.begin(), items.end(), e), items.end());
  items.insert(items.begin(), e);
  if (items.size() > 24) items.resize(24);
  std::error_code ec;
  fs::create_directories(recent_path().parent_path(), ec);
  std::ofstream(recent_path()) << nlohmann::json(items).dump();
}

}  // namespace ghost::emoji

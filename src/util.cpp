#include "util.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

namespace ghost {

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64decode(std::string_view in) {
  static int T[256];
  static bool init = false;
  if (!init) {
    std::fill(std::begin(T), std::end(T), -1);
    for (int i = 0; i < 64; ++i) T[(unsigned char)B64[i]] = i;
    init = true;
  }
  std::string out;
  out.reserve(in.size() * 3 / 4);
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (T[c] == -1) continue;
    val = (val << 6) + T[c];
    bits += 6;
    if (bits >= 0) {
      out.push_back(char((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

std::string b64encode(std::string_view in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  int val = 0, bits = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    bits += 8;
    while (bits >= 0) {
      out.push_back(B64[(val >> bits) & 0x3F]);
      bits -= 6;
    }
  }
  if (bits > -6) out.push_back(B64[((val << 8) >> (bits + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

std::string trim(std::string s) {
  auto ws = [](unsigned char c) { return std::isspace(c); };
  while (!s.empty() && ws(s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && ws(s[i])) ++i;
  return s.substr(i);
}

std::string squash(std::string_view s) {
  std::string out;
  bool space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      space = !out.empty();
    } else {
      if (space) out.push_back(' ');
      space = false;
      out.push_back(char(c));
    }
  }
  return out;
}

bool contains_ci(std::string_view hay, std::string_view needle) {
  return lower(std::string(hay)).find(lower(std::string(needle))) != std::string::npos;
}

std::vector<std::string> split_lines(std::string_view s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t nl = s.find('\n', start);
    out.emplace_back(s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return out;
}

double parse_iso(const std::string& iso) {
  std::tm tm{};
  double frac = 0;
  int y, mo, d, h, mi;
  double s;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = int(s);
  frac = s - int(s);
  return double(timegm(&tm)) + frac;
}

double now_epoch() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

std::string ago(double epoch) {
  if (epoch <= 0) return "";
  double secs = now_epoch() - epoch;
  if (secs >= 86400) return std::to_string(int(secs / 86400)) + "d";
  if (secs >= 3600) return std::to_string(int(secs / 3600)) + "h";
  if (secs >= 60) return std::to_string(int(secs / 60)) + "m";
  return "now";
}

std::string local_time(double epoch, const std::string& fmt) {
  if (epoch <= 0) return "";
  std::time_t t = std::time_t(epoch);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[128];
  size_t n = std::strftime(buf, sizeof buf, fmt.c_str(), &tm);
  return std::string(buf, n);
}

int spawn_detached(const std::vector<std::string>& argv) {
  if (argv.empty()) return -1;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    dup2(devnull, 0);
    dup2(devnull, 1);
    dup2(devnull, 2);
    std::vector<char*> a;
    for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  return pid;
}

bool on_path(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (!path) return false;
  std::string p = path;
  size_t start = 0;
  while (start <= p.size()) {
    size_t colon = p.find(':', start);
    std::string dir = p.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    std::string full = (dir.empty() ? "." : dir) + "/" + name;
    if (access(full.c_str(), X_OK) == 0) return true;
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
  return false;
}

}  // namespace ghost

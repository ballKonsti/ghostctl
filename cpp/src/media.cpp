#include "media.hpp"

#include <signal.h>
#include <sys/wait.h>

#include <cstdlib>
#include <fstream>

#include "util.hpp"

namespace ghost::media {

fs::path media_dir() { return fs::temp_directory_path() / "ghostctl-media"; }

fs::path save(const Media& m, const std::string& stem) {
  fs::create_directories(media_dir());
  std::string safe;
  for (char c : stem) safe += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
  if (safe.size() > 60) safe.resize(60);
  fs::path p = media_dir() / (safe + m.suffix());
  std::ofstream(p, std::ios::binary) << m.data;
  return p;
}

std::optional<fs::path> video_frame(const fs::path& video) {
  if (!on_path("ffmpeg")) return std::nullopt;
  fs::path out = video;
  out.replace_extension(".frame.png");
  std::string cmd = "ffmpeg -y -loglevel error -i '" + video.string() + "' -frames:v 1 '" + out.string() +
                    "' </dev/null >/dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0 || !fs::exists(out)) return std::nullopt;
  return out;
}

static std::vector<std::string> split_words(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) out.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string play(const fs::path& path, const std::string& player) {
  auto cmd = split_words(player);
  if (!cmd.empty() && on_path(cmd[0])) {
    cmd.push_back(path.string());
    spawn_detached(cmd);
    return "Playing in " + cmd[0];
  }
  if (on_path("xdg-open")) {
    spawn_detached({"xdg-open", path.string()});
    return "Opened with xdg-open";
  }
  return "Saved to " + path.string() + " (no video player found)";
}

int play_audio(const fs::path& path, const std::string& player) {
  auto cmd = split_words(player);
  std::vector<std::string> args;
  if (!cmd.empty() && on_path(cmd[0])) {
    args = cmd;
    if (cmd[0] == "mpv") args.insert(args.end(), {"--no-video", "--really-quiet", "--no-terminal"});
  } else if (on_path("mpv")) {
    args = {"mpv", "--no-video", "--really-quiet", "--no-terminal"};
  } else if (on_path("ffplay")) {
    args = {"ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"};
  } else if (on_path("paplay") && path.extension() == ".wav") {
    args = {"paplay"};
  } else {
    return -1;
  }
  args.push_back(path.string());
  return spawn_detached(args);
}

void stop(int pid) {
  if (pid > 0) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, WNOHANG);
  }
}

bool running(int pid) {
  if (pid <= 0) return false;
  return waitpid(pid, nullptr, WNOHANG) == 0;
}

void desktop_notify(const std::string& title, const std::string& body) {
  if (on_path("notify-send")) spawn_detached({"notify-send", "-a", "ghostctl", title, body});
}

std::string detect_terminal() {
  auto env = [](const char* k) -> std::string {
    const char* v = std::getenv(k);
    return v ? v : "";
  };
  if (!env("KITTY_WINDOW_ID").empty() || env("TERM") == "xterm-kitty") return "kitty";
  std::string tp = env("TERM_PROGRAM");
  if (tp == "WezTerm" || tp == "ghostty" || tp == "iTerm.app" || tp == "vscode") return tp;
  if (!env("KONSOLE_VERSION").empty()) return "konsole";
  if (!env("WT_SESSION").empty()) return "windows-terminal";
  return !tp.empty() ? tp : env("TERM").empty() ? "unknown" : env("TERM");
}

}  // namespace ghost::media

#include "cdp.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace ghost {

Cdp::~Cdp() { close(); }

void Cdp::launch(const std::string& exe, std::vector<std::string> args) {
  int in_pipe[2], out_pipe[2], err_pipe[2];  // in: we write -> fd 3, out: fd 4 -> we read
  if (pipe2(in_pipe, O_CLOEXEC) || pipe2(out_pipe, O_CLOEXEC) || pipe2(err_pipe, O_CLOEXEC))
    throw CdpError(std::string("pipe: ") + std::strerror(errno));

  args.insert(args.begin(), exe);
  args.push_back("--remote-debugging-pipe");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) throw CdpError(std::string("fork: ") + std::strerror(errno));
  if (pid == 0) {
    // Child: fd 3 = commands in, fd 4 = replies out, stderr captured, stdin/out to /dev/null.
    // Move the pipe ends out of the 0-4 range first: dup2 onto fd 3/4 could
    // clobber another pipe end, or keep O_CLOEXEC when old == new.
    int in_r = fcntl(in_pipe[0], F_DUPFD_CLOEXEC, 10);
    int out_w = fcntl(out_pipe[1], F_DUPFD_CLOEXEC, 10);
    int err_w = fcntl(err_pipe[1], F_DUPFD_CLOEXEC, 10);
    int devnull = open("/dev/null", O_RDWR);
    dup2(devnull, 0);
    dup2(devnull, 1);
    dup2(err_w, 2);
    dup2(in_r, 3);
    dup2(out_w, 4);
    for (int fd = 0; fd <= 4; ++fd) fcntl(fd, F_SETFD, 0);  // keep 0-4 across exec
    setsid();  // own process group: a terminal ^C must not hit the browser
    execv(exe.c_str(), argv.data());
    _exit(127);
  }
  pid_ = pid;
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  to_browser_ = in_pipe[1];
  from_browser_ = out_pipe[0];
  stderr_fd_ = err_pipe[0];
  alive_ = true;
  reader_ = std::thread([this] { read_loop(); });
  stderr_reader_ = std::thread([this] { stderr_loop(); });
}

void Cdp::set_event_handler(EventHandler h) {
  std::lock_guard lk(handler_mu_);
  handler_ = std::move(h);
}

json Cdp::call(const std::string& method, json params, const std::string& session,
               std::chrono::milliseconds timeout) {
  if (!alive_) throw CdpError("browser is not running");
  int id = next_id_++;
  json msg = {{"id", id}, {"method", method}, {"params", std::move(params)}};
  if (!session.empty()) msg["sessionId"] = session;
  std::string data = msg.dump(-1, ' ', false, json::error_handler_t::replace);
  data.push_back('\0');
  {
    std::lock_guard lk(mu_);
    pending_[id];
  }
  {
    std::lock_guard lk(write_mu_);
    const char* p = data.data();
    size_t left = data.size();
    while (left) {
      ssize_t n = ::write(to_browser_, p, left);
      if (n < 0) {
        if (errno == EINTR) continue;
        std::lock_guard lk2(mu_);
        pending_.erase(id);
        throw CdpError("browser pipe closed");
      }
      p += n;
      left -= n;
    }
  }
  std::unique_lock lk(mu_);
  bool ok = cv_.wait_for(lk, timeout, [&] { return pending_[id].done; });
  Pending res = std::move(pending_[id]);
  pending_.erase(id);
  if (!ok) throw CdpError(method + ": timed out");
  if (!res.error.empty()) throw CdpError(method + ": " + res.error);
  return res.result;
}

void Cdp::read_loop() {
  std::string buf;
  char chunk[65536];
  while (true) {
    ssize_t n = ::read(from_browser_, chunk, sizeof chunk);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    buf.append(chunk, n);
    size_t start = 0, z;
    while ((z = buf.find('\0', start)) != std::string::npos) {
      json msg = json::parse(buf.begin() + start, buf.begin() + z, nullptr, false);
      start = z + 1;
      if (msg.is_discarded()) continue;
      if (msg.contains("id")) {
        std::lock_guard lk(mu_);
        auto it = pending_.find(msg["id"].get<int>());
        if (it == pending_.end()) continue;
        if (msg.contains("error"))
          it->second.error = msg["error"].value("message", "error");
        else
          it->second.result = msg.value("result", json::object());
        it->second.done = true;
        cv_.notify_all();
      } else if (msg.contains("method")) {
        EventHandler h;
        {
          std::lock_guard lk(handler_mu_);
          h = handler_;
        }
        if (h) h(msg["method"].get<std::string>(), msg.value("params", json::object()),
                 msg.value("sessionId", ""));
      }
    }
    buf.erase(0, start);
  }
  fail_all("browser closed");
}

void Cdp::stderr_loop() {
  char chunk[4096];
  while (true) {
    ssize_t n = ::read(stderr_fd_, chunk, sizeof chunk);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    std::lock_guard lk(stderr_mu_);
    if (stderr_buf_.size() < 64 * 1024) stderr_buf_.append(chunk, n);
  }
}

std::string Cdp::stderr_text() {
  std::lock_guard lk(stderr_mu_);
  return stderr_buf_;
}

void Cdp::fail_all(const std::string& why) {
  alive_ = false;
  std::lock_guard lk(mu_);
  for (auto& [id, p] : pending_) {
    if (!p.done) {
      p.error = why;
      p.done = true;
    }
  }
  cv_.notify_all();
  EventHandler h;
  {
    std::lock_guard lk2(handler_mu_);
    h = handler_;
  }
  // Tell listeners the browser went away (synthetic event).
  if (h) h("ghostctl.closed", json::object(), "");
}

void Cdp::close() {
  if (pid_ > 0) {
    if (alive_) {
      try {
        call("Browser.close", json::object(), "", 5s);
      } catch (...) {
      }
    }
    // Give Chromium a moment to flush the profile, then make sure it's gone.
    for (int i = 0; i < 50; ++i) {
      if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
        pid_ = -1;
        break;
      }
      usleep(100'000);
    }
    if (pid_ > 0) {
      kill(-pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }
  if (to_browser_ >= 0) ::close(to_browser_), to_browser_ = -1;
  if (reader_.joinable()) reader_.join();
  if (stderr_reader_.joinable()) stderr_reader_.join();
  if (from_browser_ >= 0) ::close(from_browser_), from_browser_ = -1;
  if (stderr_fd_ >= 0) ::close(stderr_fd_), stderr_fd_ = -1;
  alive_ = false;
}

}  // namespace ghost

// Chrome DevTools Protocol over --remote-debugging-pipe.
//
// Chromium reads commands from fd 3 and writes replies/events to fd 4, each
// message a JSON object terminated by '\0'. A reader thread matches replies to
// pending calls by id and hands events to one handler (on the reader thread —
// keep it short, post work elsewhere).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ghost {

using json = nlohmann::json;
using namespace std::chrono_literals;

struct CdpError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The profile is already used by another Chromium (e.g. a running ghostctl).
struct ProfileLocked : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Cdp {
 public:
  using EventHandler = std::function<void(const std::string& method, const json& params,
                                          const std::string& session)>;

  Cdp() = default;
  ~Cdp();
  Cdp(const Cdp&) = delete;
  Cdp& operator=(const Cdp&) = delete;

  // Start Chromium. `args` must not include --remote-debugging-pipe (added here).
  void launch(const std::string& exe, std::vector<std::string> args);

  // Send a command and wait for its result. Throws CdpError on protocol errors,
  // timeouts, or when the browser is gone.
  json call(const std::string& method, json params = json::object(), const std::string& session = "",
            std::chrono::milliseconds timeout = 30s);

  void set_event_handler(EventHandler h);
  bool alive() const { return alive_; }
  // Ask the browser to close, then make sure the process is gone.
  void close();
  // Stderr lines collected so far (for diagnosing launch failures).
  std::string stderr_text();

 private:
  struct Pending {
    bool done = false;
    json result;
    std::string error;
  };

  void read_loop();
  void stderr_loop();
  void fail_all(const std::string& why);

  int to_browser_ = -1;    // our end of fd 3
  int from_browser_ = -1;  // our end of fd 4
  int stderr_fd_ = -1;
  int pid_ = -1;
  std::atomic<bool> alive_{false};
  std::atomic<int> next_id_{1};
  std::mutex write_mu_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::map<int, Pending> pending_;
  EventHandler handler_;
  std::mutex handler_mu_;
  std::thread reader_;
  std::thread stderr_reader_;
  std::mutex stderr_mu_;
  std::string stderr_buf_;
};

}  // namespace ghost

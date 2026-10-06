// Small shared helpers.
#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ghost {

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string b64decode(std::string_view in);
std::string b64encode(std::string_view in);

std::string lower(std::string s);
std::string trim(std::string s);
// Collapse runs of whitespace to single spaces and trim.
std::string squash(std::string_view s);
bool contains_ci(std::string_view hay, std::string_view needle);
std::vector<std::string> split_lines(std::string_view s);

// Seconds since the epoch for an ISO-8601 UTC time like "2026-10-05T17:29:48.707Z"; 0 if invalid.
double parse_iso(const std::string& iso);
// "now", "5m", "3h", "2d" relative to the current time.
std::string ago(double epoch);
// strftime in local time.
std::string local_time(double epoch, const std::string& fmt);

double now_epoch();

// Run a program detached (new session, output discarded). Returns pid or -1.
int spawn_detached(const std::vector<std::string>& argv);
// Is `name` an executable on PATH?
bool on_path(const std::string& name);

}  // namespace ghost

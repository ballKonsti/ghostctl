// Media files, video frames, playback, desktop notifications.
#pragma once

#include <optional>
#include <string>

#include "bridge.hpp"

namespace ghost::media {

fs::path media_dir();  // /tmp/ghostctl-media
fs::path save(const Media& m, const std::string& stem);
// First frame of a video as PNG (needs ffmpeg).
std::optional<fs::path> video_frame(const fs::path& video);
// Start a video player detached. Returns a status message.
std::string play(const fs::path& path, const std::string& player);
// Play audio in the background; returns the pid (to stop it) or -1.
int play_audio(const fs::path& path, const std::string& player);
void stop(int pid);
bool running(int pid);
void desktop_notify(const std::string& title, const std::string& body);
std::string detect_terminal();

}  // namespace ghost::media

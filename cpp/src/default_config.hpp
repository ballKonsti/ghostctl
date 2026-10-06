// Generated from the Python version's DEFAULT_TOML; keep both in sync.
#pragma once
#include <string_view>

namespace ghost {
inline constexpr std::string_view DEFAULT_TOML = R"TOML(# ghostctl configuration. Delete a line to fall back to its default.

[browser]
# Where the logged-in Chromium profile lives.
profile_dir = "~/.ghostctl/profile"
# "auto": headless, falling back to a hidden window if Snapchat refuses it.
# "always": headless only. "never": always use a (hidden) window.
headless = "auto"
# Override the browser user agent ("" = current Chromium, minus "Headless").
user_agent = ""
# Chromium to run ("" = Playwright's Chromium in ~/.cache/ms-playwright, else
# chromium / google-chrome from PATH).
executable = ""

[ui]
# "ghost" (Snapchat colours), "nord", "gruvbox", "dracula", "tokyo-night",
# "catppuccin-mocha" or "monokai".
theme = "ghost"
chat_list_width = 38
# One line per chat instead of two.
compact_chat_list = false
show_streaks = true
show_badges = true
show_group_tag = true
# strftime format for message times.
time_format = "%H:%M"
show_date_separators = true
show_reactions = true
# Colours for sender names and the unread marker ("#RRGGBB").
me_color = "#2EA8FF"
them_color = "#F23C57"
unread_color = "#F23C57"
unread_marker = "●"

[notifications]
# Ring the terminal bell for new messages in other chats.
bell = true
# Also send a desktop notification (uses notify-send).
desktop = false
# Show "New Chat from X" in the status bar.
status_line = true

[behavior]
# Minimum seconds between actions on the page (keep it human-paced).
action_gap = 1.2
# Close the chat in the page when you leave it, so new messages there stay unread.
close_chat_on_back = true
# Ask before opening a snap (opening marks it as viewed).
confirm_snap_open = true
# Mirror what you type to Snapchat's composer so friends see "typing...".
send_typing = true

[images]
# "auto" (detect), "kitty", "sixel", "iterm" (uses sixel), "halfblock", "unicode",
# or "none" (never draw images).
protocol = "auto"
# Command used to play videos (snaps and chat media). "" = xdg-open.
video_player = "mpv"

[debug]
# Save the page (DOM, accessibility tree, screenshot) to ~/.ghostctl/debug the first
# time a snap or story viewer opens, so selectors can be fixed if it misbehaves.
dump_viewer = true

[keys]
# Comma-separated key names per action: a letter ("q", "C"), or enter, escape,
# tab, space, up/down/left/right, slash, question_mark, ctrl+<letter>.
down = "j,down"
up = "k,up"
open = "enter"
back = "escape"
compose = "i"
reply = "r"
menu = "e"
open_media = "o"
camera = "c"
send_file = "a"
emoji = "ctrl+e"
search = "slash"
mark_read = "m"
stories = "s"
refresh = "ctrl+r"
call = "C"
help = "question_mark"
quit = "q"
)TOML";
}  // namespace ghost

"""User configuration: ~/.config/ghostctl/config.toml, merged over DEFAULT_TOML."""

from __future__ import annotations

import os
import tomllib
from pathlib import Path
from typing import Any

CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "ghostctl"
CONFIG_PATH = CONFIG_DIR / "config.toml"

# Written out verbatim by `ghostctl config`, so it doubles as documentation.
DEFAULT_TOML = """\
# ghostctl configuration. Delete a line to fall back to its default.

[browser]
# Where the logged-in Chromium profile lives.
profile_dir = "~/.ghostctl/profile"
# "auto": headless, falling back to a hidden window if Snapchat refuses it.
# "always": headless only. "never": always use a (hidden) window.
headless = "auto"
# Override the browser user agent ("" = current Chromium, minus "Headless").
user_agent = ""

[ui]
# Any Textual theme: textual-dark, textual-light, nord, gruvbox, catppuccin-mocha,
# dracula, tokyo-night, monokai, flexoki, solarized-light, ... (ctrl+p to preview).
theme = "textual-dark"
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
# Colours for sender names and the unread marker (any Rich colour).
me_color = "bold cyan"
them_color = "bold red"
unread_color = "bold red"
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
# Comma-separated Textual key names per action.
down = "j,down"
up = "k,up"
open = "enter"
back = "escape"
compose = "i"
reply = "r"
menu = "e"
open_media = "o"
send_file = "a"
search = "slash"
mark_read = "m"
stories = "s"
refresh = "ctrl+r"
call = "c"
help = "question_mark"
quit = "q"
"""


def _merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in over.items():
        out[k] = _merge(base[k], v) if isinstance(v, dict) and isinstance(base.get(k), dict) else v
    return out


class Config:
    def __init__(self, data: dict[str, Any], error: str = "") -> None:
        self.data = data
        self.error = error  # problem loading the user file, shown in the TUI

    def __getitem__(self, section: str) -> dict[str, Any]:
        return self.data[section]

    @property
    def profile_dir(self) -> Path:
        return Path(self["browser"]["profile_dir"]).expanduser()


def load() -> Config:
    data = tomllib.loads(DEFAULT_TOML)
    error = ""
    if CONFIG_PATH.exists():
        try:
            data = _merge(data, tomllib.loads(CONFIG_PATH.read_text()))
        except tomllib.TOMLDecodeError as e:
            error = f"{CONFIG_PATH}: {e} (using defaults)"
    return Config(data, error)


def write_default() -> bool:
    """Create the config file if missing. Returns True if it was created."""
    if CONFIG_PATH.exists():
        return False
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(DEFAULT_TOML)
    return True

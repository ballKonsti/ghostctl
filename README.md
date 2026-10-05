# ghostctl

A terminal client for Snapchat Web. ghostctl drives the real Snapchat Web page in
a Playwright-controlled Chromium (invisible, headless) and puts a Textual TUI on
top of it. It is meant for your own account, on your own machine.

## Setup

You need Python 3.12+ (textual-image requires it) and [uv](https://docs.astral.sh/uv/).

```sh
cd snapchat_cli
uv sync                              # creates .venv and installs dependencies
uv run playwright install chromium   # downloads the Chromium build Playwright drives
uv tool install --editable .         # puts `ghostctl` on your PATH (~/.local/bin)
```

Optional: `mpv` (or any player) for video snaps, `ffmpeg` for video preview
frames, `notify-send` for desktop notifications.

## Run

```sh
ghostctl                    # start the TUI (shows a login screen if you're logged out)
ghostctl login              # or log in from the terminal (username, password, 2FA code)
ghostctl login --remember   # already logged in: save your login for automatic re-login
ghostctl login --window     # log in yourself in a visible browser window
ghostctl forget             # delete the saved login from the keyring
ghostctl logout             # delete the session and the saved login
ghostctl check              # confirm the saved session works
ghostctl config --edit      # create/edit ~/.config/ghostctl/config.toml
ghostctl inspect            # debug: dump the live page to debug/ to fix selectors
```

### Staying logged in

You log in once: the session lives in the Chromium profile at `~/.ghostctl/profile`
(like a browser remembering you). Only one ghostctl process can use it at a time.

If Snapchat ever ends the session (after a long time, a password change, ...),
ghostctl notices within two minutes. With **Remember me** ticked on the login
screen (or after `ghostctl login --remember`), your username and password are
kept in the system keyring (GNOME Keyring / KWallet via Secret Service, macOS
Keychain, Windows Credential Locker — never in a file), and ghostctl logs back in
by itself. A 2FA code can't be automated: if Snapchat asks for one, ghostctl asks you.

Your password is only ever typed into Snapchat's own login form. If Snapchat
shows something ghostctl can't handle (an interactive captcha), it offers to open
a browser window so you can finish there.

### Headless

Snapchat refuses browsers whose user agent says "HeadlessChrome", so ghostctl
sends the normal Chrome user agent of the installed Chromium. If Snapchat ever
refuses headless anyway, ghostctl falls back to a hidden window (forced onto
X11, since Wayland compositors ignore off-screen placement) and says so.
Set `[browser] headless` in the config to change this.

## Keys

Press `?` in the app for the list. All keys can be rebound in the config.

| key | action |
| --- | --- |
| `j` / `k` | move (`k` at the top of a chat loads older messages; `j` at the end of the list loads more chats) |
| `enter` | open chat; on a message: the message menu |
| `esc` | back / cancel / clear search |
| `i` | write a message (`/send ~/pic.jpg` sends an image) |
| `r` | reply to the selected message |
| `e` | message menu: react (love, laugh, fire, 👍, 👎, cry, shock, ?), save/unsave, reply, copy, delete |
| `o` | open the selected snap or photo/video |
| `c` | live snap: webcam preview, photo (`space`) or video (`v`), lenses (`←/→`), caption, Send To |
| `a` | send an image (file picker) |
| `/` | search chats |
| `m` | mark the selected chat as read |
| `s` | stories |
| `ctrl+r` | re-read the page |
| `C` | calls (not supported in the terminal) |
| `q` | quit |

In the snap viewer: `n`/`space` next, `1`–`8` react, `p` play video, `esc` close.

## What Snapchat Web allows (checked 2026-10-05)

- **Snaps (receiving):** opening an unopened snap marks it as viewed for the
  sender. ghostctl only opens one when you press `o`, and asks first (turn the
  prompt off with `confirm_snap_open = false`). Opening a chat never opens its snaps.
- **Snaps (sending):** the web client sends snaps from the webcam only. `c` opens
  Snapchat's real camera with a live preview in the terminal; take a photo or
  video, add a caption, pick recipients (the open chat is preselected; My Story
  and groups are listed too) and send. ghostctl doesn't fake a webcam, so a snap
  can't come from a file; send images **in chat** instead (`a` or `/send`).
- **Chat media upload:** png, jpeg and gif only. No video.
- **Saved status:** the page only reveals it in a message's menu. ghostctl shows
  💾 once you've opened the menu (`e`) on a message.
- **Typing:** while you write, ghostctl mirrors your draft into Snapchat's
  composer so friends see you typing (`send_typing = false` to disable). An
  incoming "typing" shows under the conversation when the page shows it.
- **Stories:** the web client shows a "View stories" tile; ghostctl opens it with `s`.
- **Not supported:** voice/video calls, live Lenses.
- Opening a chat marks its messages as read, the same as on the web. Leaving a
  chat (`esc`) closes it in the page too, so new messages there stay unread.

## Configuration

`ghostctl config` writes `~/.config/ghostctl/config.toml` with every option and
its default, commented. Highlights: theme (any Textual theme; `ctrl+p` previews
them live), colours for you/others/unread, compact chat list, which badges to
show, time format, notifications (bell, status line, desktop), action pacing,
image protocol (`auto`, `kitty`, `sixel`, `iterm`, `halfblock`, `unicode`, `none`),
video player, and every key binding.

## When Snapchat changes its layout

Every DOM selector is in `src/ghostctl/selectors.py`, each with a comment saying
what it targets. If an error names a selector, run `ghostctl inspect`, go to the
view that broke, type a label and press Enter. Each dump in `debug/<time>-<label>/`
contains `aria.yaml`, `elements.json`, `page.html`, `screenshot.png` and `url.txt`.
The first snap/story you open is also dumped to `~/.ghostctl/debug/` (turn off
with `[debug] dump_viewer = false`). Dumps contain your messages; `debug/` is
gitignored.

"""Terminal image protocol selection, media files, video playback, desktop notifications."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from .bridge import Media


def _import_textual_image():
    """textual-image probes the terminal on import (must happen before the Textual
    app starts). The probe crashes on terminals that report a zero size; then
    seed its cache with "no graphics" so it falls back to half-block images."""
    try:
        import textual_image.renderable  # noqa: F401
    except Exception:  # noqa: BLE001
        from textual_image import _terminal as t

        t.probe_terminal._result = t.TerminalCapabilities(t.CellSize(10, 20), False, False)
        for name in [m for m in sys.modules if m.startswith("textual_image.renderable")]:
            del sys.modules[name]
        import textual_image.renderable  # noqa: F401
    import textual_image.renderable as renderable
    from textual_image import widget

    return renderable, widget


textual_image_renderable, timg = _import_textual_image()

MEDIA_DIR = Path(tempfile.gettempdir()) / "ghostctl-media"

_PROTOCOLS = {
    "kitty": timg.TGPImage,
    "sixel": timg.SixelImage,
    "iterm": timg.SixelImage,  # iTerm2 speaks sixel; textual-image has no native iTerm2 mode
    "halfblock": timg.HalfcellImage,
    "unicode": timg.UnicodeImage,
}


def detect_terminal() -> str:
    """Best guess at the terminal, for the status line."""
    env = os.environ
    if env.get("KITTY_WINDOW_ID") or env.get("TERM") == "xterm-kitty":
        return "kitty"
    if env.get("TERM_PROGRAM") in ("WezTerm", "ghostty", "iTerm.app", "vscode"):
        return env["TERM_PROGRAM"]
    if env.get("KONSOLE_VERSION"):
        return "konsole"
    if env.get("WT_SESSION"):
        return "windows-terminal"
    return env.get("TERM_PROGRAM") or env.get("TERM", "unknown")


def image_widget_class(protocol: str):
    """Widget class for the configured protocol; "auto" lets textual-image decide
    (kitty graphics, then sixel, then half-blocks)."""
    if protocol == "none":
        return None
    return _PROTOCOLS.get(protocol, timg.Image)


def auto_protocol_name() -> str:
    """Which protocol textual-image picked for this terminal."""
    r = textual_image_renderable
    names = {r.TGPImage: "kitty", r.SixelImage: "sixel", r.HalfcellImage: "halfblock",
             r.UnicodeImage: "unicode"}
    return names.get(r.Image, "unknown")


def save(media: Media, stem: str) -> Path:
    MEDIA_DIR.mkdir(parents=True, exist_ok=True)
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in stem)[:60]
    path = MEDIA_DIR / f"{safe}{media.suffix}"
    path.write_bytes(media.data)
    return path


def play(path: Path, player: str) -> str:
    """Start a video player detached from the TUI. Returns a status message."""
    cmd = player.split() if player else []
    if cmd and shutil.which(cmd[0]):
        subprocess.Popen([*cmd, str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return f"Playing in {cmd[0]}"
    opener = "open" if sys.platform == "darwin" else "xdg-open"
    if shutil.which(opener):
        subprocess.Popen([opener, str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return f"Opened with {opener}"
    return f"Saved to {path} (no video player found)"


def desktop_notify(title: str, body: str) -> None:
    if shutil.which("notify-send"):
        subprocess.Popen(["notify-send", "-a", "ghostctl", title, body],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif sys.platform == "darwin" and shutil.which("osascript"):
        script = f'display notification {body!r} with title {title!r}'
        subprocess.Popen(["osascript", "-e", script])

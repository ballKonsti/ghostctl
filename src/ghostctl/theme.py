"""Look and feel: the "ghost" theme, Snapchat-style status icons, message bars."""

from __future__ import annotations

from rich.console import Console, ConsoleOptions, RenderableType, RenderResult
from rich.segment import Segment
from rich.style import Style
from textual.theme import Theme

# Snapchat's palette.
YELLOW = "#FFFC00"
SNAP_RED = "#F23C57"
CHAT_BLUE = "#2EA8FF"
VIDEO_PURPLE = "#A05DCD"
CALL_GREEN = "#2FD07A"

GHOST_THEME = Theme(
    name="ghost",
    primary=YELLOW,
    secondary=CHAT_BLUE,
    accent=YELLOW,
    warning="#FFB224",
    error=SNAP_RED,
    success=CALL_GREEN,
    foreground="#ECECF1",
    background="#0B0B0F",
    surface="#13131A",
    panel="#1D1D26",
    boost="#24242F",
    dark=True,
    variables={
        "footer-key-foreground": YELLOW,
        "input-selection-background": f"{YELLOW} 30%",
        # Selected rows: a soft lift, never dark text on a bright bar.
        "block-cursor-background": "#2C2C3A",
        "block-cursor-foreground": "#ECECF1",
        "block-cursor-text-style": "none",
        "block-cursor-blurred-background": "#1F1F29",
        "block-cursor-blurred-foreground": "#ECECF1",
        "block-cursor-blurred-text-style": "none",
        "scrollbar": "#2A2A36",
        "scrollbar-hover": "#55555F",
        "scrollbar-active": YELLOW,
        "scrollbar-background": "#13131A",
        "scrollbar-background-hover": "#13131A",
        "scrollbar-background-active": "#13131A",
    },
)

# Status text from the chat list -> (icon, colour, bold). Mirrors Snapchat's icons:
# filled = new/unopened, arrow = sent, hollow = opened/received.
_STATUS = [
    ("new snap", "■", SNAP_RED, True),
    ("new video", "■", VIDEO_PURPLE, True),
    ("new chat", "■", CHAT_BLUE, True),
    ("new", "■", CHAT_BLUE, True),
    ("typing", "✎", YELLOW, True),
    ("call active", "✆", CALL_GREEN, True),
    ("missed", "✆", SNAP_RED, False),
    ("call", "✆", CALL_GREEN, False),
    ("delivered", "➤", CHAT_BLUE, False),
    ("sent", "➤", CHAT_BLUE, False),
    ("opened", "▷", "#8A8A99", False),
    ("received", "□", "#8A8A99", False),
    ("screenshot", "⧉", YELLOW, False),
    ("replayed", "↻", "#8A8A99", False),
    ("saved", "◆", "#8A8A99", False),
]


def status_style(status: str) -> tuple[str, str, bool]:
    s = status.lower()
    for key, icon, colour, bold in _STATUS:
        if key in s:
            return icon, colour, bold
    return "·", "#8A8A99", False


REACTION_EMOJI = {
    "love": "❤️",
    "laugh": "😂",
    "fire": "🔥",
    "thumbs-up": "👍",
    "thumbs-down": "👎",
    "cry": "😢",
    "shock": "😮",
    "question mark": "❓",
}
REACTION_ORDER = list(REACTION_EMOJI)


def reaction_label(r: str) -> str:
    """'love · Konstantin' -> '❤️ Konstantin'."""
    name, _, who = r.partition(" · ")
    return f"{REACTION_EMOJI.get(name, name)} {who}".strip()


GHOST_ART = """\
      ▄▄██████▄▄
    ▄████████████▄
   ████████████████
   ████████████████
   ████████████████
 ▄▄████████████████▄▄
  ▀████████████████▀
   ████████████████
   ████▀▀████▀▀████
   ▀▀▀    ▀▀    ▀▀▀"""


class Barred:
    """Renders content with a coloured bar on every line, like Snapchat's
    message bubbles: '▎ text'."""

    def __init__(self, renderable: RenderableType, colour: str, bar: str = "▎ ") -> None:
        self.renderable = renderable
        self.style = Style(color=colour, bold=True)
        self.bar = bar

    def __rich_console__(self, console: Console, options: ConsoleOptions) -> RenderResult:
        width = max(1, options.max_width - len(self.bar))
        lines = console.render_lines(self.renderable, options.update(width=width), pad=False)
        for line in lines:
            yield Segment(self.bar, self.style)
            yield from line
            yield Segment.line()

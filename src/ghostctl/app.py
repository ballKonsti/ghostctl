"""Textual UI: chat list, conversation, composer, snap viewer, camera."""

from __future__ import annotations

import asyncio
import io
import shutil
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from PIL import Image as PILImage
from rich.cells import cell_len
from rich.console import Group
from rich.text import Text
from textual import events, on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Center, Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Checkbox, DirectoryTree, Footer, Input, OptionList, Static
from textual.widgets.option_list import Option

from . import config as cfgmod
from . import creds as C
from . import media as M
from .bridge import (
    ActionError,
    Bridge,
    Chat,
    Conversation,
    DateMark,
    Media,
    Message,
    Notice,
    SelectorError,
)
from .browser import HOME, ProfileLocked, Session, from_config
from .login import run_login
from .theme import (
    CALL_GREEN,
    CHAT_BLUE,
    GHOST_ART,
    GHOST_THEME,
    REACTION_EMOJI,
    REACTION_ORDER,
    SNAP_RED,
    VIDEO_PURPLE,
    YELLOW,
    Barred,
    reaction_label,
    status_style,
)

ACTIONS = {
    # config key -> (app action, help text)
    "down": ("down", "move down (at the end of the list: load more chats)"),
    "up": ("up", "move up (at the top of a chat: load older messages)"),
    "open": ("open", "open chat / message menu"),
    "back": ("back", "back / cancel / clear search"),
    "compose": ("compose", "write a message (/send <path> attaches an image)"),
    "reply": ("reply", "reply to the selected message"),
    "menu": ("menu", "react / save / copy / delete the selected message"),
    "open_media": ("open_media", "open snap or media (snaps: marks it viewed)"),
    "camera": ("camera", "take a live photo snap with your webcam"),
    "send_file": ("send_file", "send an image from a file"),
    "search": ("search", "search chats"),
    "mark_read": ("mark_read", "mark the selected chat as read"),
    "stories": ("stories", "view stories"),
    "refresh": ("refresh", "re-read the page"),
    "call": ("call", "start a call (not supported)"),
    "help": ("help", "this help"),
    "quit": ("quit", "quit"),
}
FOOTER = {"compose": "chat", "camera": "snap", "reply": "reply", "menu": "react", "open_media": "open",
          "send_file": "photo", "search": "search", "help": "keys", "quit": "quit"}

DEBUG_DIR = HOME / "debug"
DIM = "#8A8A99"


def _ago(t: datetime | None) -> str:
    if t is None:
        return ""
    secs = (datetime.now(t.tzinfo) - t).total_seconds()
    for unit, n in (("d", 86400), ("h", 3600), ("m", 60)):
        if secs >= n:
            return f"{int(secs // n)}{unit}"
    return "now"


def _cell_size() -> tuple[int, int]:
    try:
        from textual_image._terminal import get_cell_size

        cw, ch = get_cell_size()
        if cw and ch:
            return cw, ch
    except Exception:  # noqa: BLE001
        pass
    return 10, 20


def fit_cells(img_w: int, img_h: int, max_cols: int, max_rows: int) -> tuple[int, int]:
    """Largest cell box with the image's aspect ratio that fits max_cols x max_rows."""
    cw, ch = _cell_size()
    if img_w <= 0 or img_h <= 0:
        return max_cols, max_rows
    scale = min(max_cols * cw / img_w, max_rows * ch / img_h)
    return max(1, int(img_w * scale / cw)), max(1, int(img_h * scale / ch))


def _video_frame(path: Path) -> Path | None:
    """First frame of a video as PNG (needs ffmpeg)."""
    if not shutil.which("ffmpeg"):
        return None
    out = path.with_suffix(".frame.png")
    try:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(path), "-frames:v", "1", str(out)],
                       timeout=15, check=True)
        return out
    except (subprocess.SubprocessError, OSError):
        return None


@dataclass
class Pending:
    """A message being sent (shown until it appears in the chat)."""

    chat_id: str
    text: str
    started: float
    failed: str = ""


# --- picture frame: fits an image into its area, keeping the aspect ratio ---


class Picture(Vertical):
    DEFAULT_CSS = """
    Picture { align: center middle; height: 1fr; width: 1fr; }
    Picture > .pic { width: auto; height: auto; }
    """

    def __init__(self, protocol: str, **kw) -> None:
        super().__init__(**kw)
        self.cls = M.image_widget_class(protocol)
        self.widget = None
        self.size_px = (0, 0)

    async def show(self, image: PILImage.Image | Path | None, note: str = "") -> None:
        if image is None or self.cls is None:
            await self.remove_children()
            self.widget = None
            await self.mount(Static(Text(note or "[image]", style=DIM)))
            return
        pil = image if isinstance(image, PILImage.Image) else PILImage.open(image)
        pil.load()
        self.size_px = pil.size
        if self.widget is None:
            await self.remove_children()
            self.widget = self.cls(pil, classes="pic")
            await self.mount(self.widget)
        else:
            self.widget.image = pil
        self._fit()

    def _fit(self) -> None:
        if self.widget is None or not self.size.width:
            return
        cols, rows = fit_cells(*self.size_px, self.size.width, self.size.height)
        self.widget.styles.width = cols
        self.widget.styles.height = rows

    def on_resize(self, event: events.Resize) -> None:
        self._fit()


# --- modal screens ---


class HelpScreen(ModalScreen[None]):
    BINDINGS = [Binding("escape,q,question_mark", "dismiss", "close")]

    def __init__(self, keys: dict[str, str], config_path: Path) -> None:
        super().__init__()
        self.keys, self.config_path = keys, config_path

    def compose(self) -> ComposeResult:
        t = Text()
        t.append("👻 ghostctl", style=f"bold {YELLOW}")
        t.append("  keys\n\n", style=DIM)
        for name, (_, desc) in ACTIONS.items():
            t.append(f"  {self.keys.get(name, ''):<14}", style=f"bold {YELLOW}")
            t.append(f"{desc}\n")
        t.append("\n  Snap viewer  ", style="bold")
        t.append("n next · 1-8 react · p play video · esc close\n", style=DIM)
        t.append("  Camera       ", style="bold")
        t.append("space take snap · ←/→ lens · enter send to · esc retake/close\n", style=DIM)
        t.append(f"\n  Config: {self.config_path}\n", style=DIM)
        t.append("  `ghostctl config --edit` to change keys, colours, behaviour. ctrl+p: themes.", style=DIM)
        yield VerticalScroll(Static(t), classes="dialog")


class ConfirmScreen(ModalScreen[bool]):
    BINDINGS = [Binding("y,enter", "yes", "yes"), Binding("n,escape", "no", "no")]

    def __init__(self, question: str | Text, yes: str = "yes") -> None:
        super().__init__()
        self.question, self.yes = question, yes

    def compose(self) -> ComposeResult:
        hint = Text.assemble(("  y ", f"bold {YELLOW}"), (self.yes, ""), ("    n ", f"bold {YELLOW}"), ("cancel", ""))
        yield Vertical(Static(self.question), Static(""), Static(hint), classes="dialog")

    def action_yes(self) -> None:
        self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)


class MenuScreen(ModalScreen[str | None]):
    BINDINGS = [Binding("escape,q", "cancel", "cancel"), Binding("j", "down", show=False),
                Binding("k", "up", show=False)]

    def __init__(self, title: Text, options: list[tuple[str, Text | str]]) -> None:
        super().__init__()
        self.title_text, self.options = title, options

    def compose(self) -> ComposeResult:
        with Vertical(classes="dialog"):
            yield Static(self.title_text)
            yield OptionList(*[Option(label, id=value) for value, label in self.options], id="menu")

    def on_mount(self) -> None:
        self.query_one("#menu").focus()

    @on(OptionList.OptionSelected)
    def picked(self, event: OptionList.OptionSelected) -> None:
        self.dismiss(event.option.id)

    def action_down(self) -> None:
        self.query_one("#menu", OptionList).action_cursor_down()

    def action_up(self) -> None:
        self.query_one("#menu", OptionList).action_cursor_up()

    def action_cancel(self) -> None:
        self.dismiss(None)


class FilePickerScreen(ModalScreen[Path | None]):
    """Pick an image: type a path, or browse with the tree."""

    BINDINGS = [Binding("escape", "cancel", "cancel")]

    def compose(self) -> ComposeResult:
        with Vertical(classes="dialog wide"):
            yield Static(Text("Send a photo  ", style=f"bold {YELLOW}") + Text("png · jpeg · gif", style=DIM))
            yield Input(placeholder="~/Pictures/photo.jpg", id="path")
            yield DirectoryTree(Path.home(), id="tree")

    @on(Input.Submitted, "#path")
    def typed(self, event: Input.Submitted) -> None:
        self._choose(Path(event.value).expanduser())

    @on(DirectoryTree.FileSelected)
    def picked(self, event: DirectoryTree.FileSelected) -> None:
        self._choose(event.path)

    def _choose(self, path: Path) -> None:
        if path.is_file():
            self.dismiss(path)
        else:
            self.notify(f"Not a file: {path}", severity="error")

    def action_cancel(self) -> None:
        self.dismiss(None)


class ViewerScreen(ModalScreen[None]):
    """Full-screen snap / story / media viewer."""

    BINDINGS = [
        Binding("escape,q", "close", "close"),
        Binding("n,space,right,l", "next", "next"),
        Binding("p", "play", "play video"),
        *[Binding(str(i + 1), f"react({i})", show=False) for i in range(8)],
    ]

    def __init__(self, app_: GhostctlApp, items: list[tuple[Media, dict]], kind: str, viewer: bool) -> None:
        super().__init__()
        self.app_, self.items, self.kind, self.viewer = app_, items, kind, viewer
        self.index = 0
        self.paths: list[Path] = []

    def compose(self) -> ComposeResult:
        with Vertical(id="viewer"):
            yield Static("", id="viewer-top")
            yield Picture(self.app_.cfg["images"]["protocol"], id="viewer-pic")
            yield Static("", id="viewer-bottom")

    async def on_mount(self) -> None:
        await self.show()

    async def show(self) -> None:
        media, info = self.items[self.index]
        stem = f"{datetime.now():%Y%m%d-%H%M%S}-{self.index}"
        path = M.save(media, stem)
        self.paths.append(path)
        colour = VIDEO_PURPLE if media.kind == "video" else (SNAP_RED if self.kind in ("Snap", "Story") else CHAT_BLUE)
        top = Text()
        top.append(" ■ " if self.kind in ("Snap", "Story") else " ▣ ", style=f"bold {colour}")
        top.append(f"{self.kind}", style=f"bold {colour}")
        if info.get("sender"):
            top.append(f"  {info['sender']}", style="bold")
        if info.get("when"):
            top.append(f"  · {info['when']}", style=DIM)
        if len(self.items) > 1 or self.viewer:
            top.append(f"   {self.index + 1}", style=DIM)
        self.query_one("#viewer-top", Static).update(top)

        img = path
        if media.kind == "video":
            img = _video_frame(path)
            player = self.app_.cfg["images"]["video_player"]
            if player:
                self.notify(M.play(path, player))
        await self.query_one(Picture).show(img, note=f"{media.kind} saved to {path}")

        bottom = Text(" ")
        if self.viewer:
            for i, name in enumerate(REACTION_ORDER):
                bottom.append(f"{i + 1}", style=f"bold {YELLOW}")
                bottom.append(f" {REACTION_EMOJI[name]}  ")
            bottom.append("  ")
        for key, label in (("n", "next"), ("p", "play") if media.kind == "video" else ("", ""), ("esc", "close")):
            if key:
                bottom.append(f" {key} ", style=f"bold {YELLOW}")
                bottom.append(f"{label} ", style=DIM)
        bottom.append(f"   {path.name}", style=DIM)
        self.query_one("#viewer-bottom", Static).update(bottom)

    def action_play(self) -> None:
        if self.items[self.index][0].kind == "video":
            self.notify(M.play(self.paths[-1], self.app_.cfg["images"]["video_player"] or ""))

    async def action_react(self, i: int) -> None:
        if not self.viewer or not self.app_.bridge:
            return
        name = REACTION_ORDER[i]
        try:
            await self.app_.bridge.viewer_react(name)
            self.notify(f"Reacted {REACTION_EMOJI[name]}")
        except Exception as e:  # noqa: BLE001
            self.notify(str(e), severity="warning")

    async def action_next(self) -> None:
        try:
            if self.index + 1 < len(self.items):
                self.index += 1
                await self.show()
                return
            if self.viewer and self.app_.bridge:
                nxt = await self.app_.bridge.viewer_next()
                if nxt is not None:
                    self.items.append((nxt[0], self.app_.viewer_info(nxt[1])))
                    self.index += 1
                    await self.show()
                    return
        except Exception as e:  # noqa: BLE001 - never crash the app from the viewer
            self.notify(f"Couldn't advance: {e}", severity="warning")
        await self.action_close()

    async def action_close(self) -> None:
        if self.viewer and self.app_.bridge:
            try:
                await self.app_.bridge.close_viewer()
            except Exception as e:  # noqa: BLE001
                self.notify(f"Couldn't close the viewer in the page: {e}", severity="warning")
        self.dismiss(None)


class SendToScreen(ModalScreen[set[int] | None]):
    """Choose snap recipients. Rows come from Snapchat's own Send To list."""

    BINDINGS = [
        Binding("escape", "cancel", "back"),
        Binding("space", "toggle", "select", priority=True),
        Binding("enter", "send", "send", priority=True),
        Binding("down,j", "down", show=False),
        Binding("up,k", "up", show=False),
    ]

    def __init__(self, rows: list[dict], chat_name: str) -> None:
        super().__init__()
        self.rows = rows
        self.chosen = {r["idx"] for r in rows if r["selected"]}
        self.chat_name = chat_name
        self.filter = ""

    def compose(self) -> ComposeResult:
        with Vertical(classes="dialog tall"):
            yield Static(Text("Send To", style=f"bold {YELLOW}"))
            yield Input(placeholder="search friends, groups, My Story", id="who")
            yield OptionList(id="rcpts")
            yield Static("", id="chosen")

    def on_mount(self) -> None:
        self.render_rows()
        self.query_one("#rcpts").focus()

    def render_rows(self) -> None:
        ol = self.query_one("#rcpts", OptionList)
        keep = ol.highlighted_option.id if ol.highlighted_option else None
        ol.clear_options()
        section = None
        f = self.filter.lower()
        for r in self.rows:
            if f and f not in r["name"].lower():
                continue
            if r["section"] != section and not f:
                section = r["section"]
                ol.add_option(Option(Text(f" {section}", style=f"bold {DIM}"), disabled=True))
            on_ = r["idx"] in self.chosen
            t = Text()
            t.append(" ● " if on_ else " ○ ", style=f"bold {CHAT_BLUE}" if on_ else DIM)
            t.append(r["name"], style="bold" if on_ else "")
            if r["extra"]:
                t.append(f"  {r['extra']}", style=DIM)
            ol.add_option(Option(t, id=str(r["idx"])))
        if keep:
            try:
                ol.highlighted = ol.get_option_index(keep)
            except Exception:  # noqa: BLE001
                pass
        names = [r["name"] for r in self.rows if r["idx"] in self.chosen]
        line = Text(" enter ", style=f"bold {YELLOW}")
        line.append(f"send to {', '.join(names) or 'nobody yet'}", style="bold" if names else DIM)
        line.append("   space ", style=f"bold {YELLOW}")
        line.append("select   ", style=DIM)
        line.append("esc ", style=f"bold {YELLOW}")
        line.append("back", style=DIM)
        self.query_one("#chosen", Static).update(line)

    @on(Input.Changed, "#who")
    def search(self, event: Input.Changed) -> None:
        self.filter = event.value
        self.render_rows()

    @on(Input.Submitted, "#who")
    def search_done(self) -> None:
        self.query_one("#rcpts").focus()

    @on(OptionList.OptionSelected, "#rcpts")
    def clicked(self) -> None:
        self.action_toggle()

    def action_toggle(self) -> None:
        if self.focused and self.focused.id == "who":
            self.query_one("#who", Input).insert_text_at_cursor(" ")
            return
        opt = self.query_one("#rcpts", OptionList).highlighted_option
        if opt and opt.id is not None:
            self.chosen ^= {int(opt.id)}
            self.render_rows()

    def action_down(self) -> None:
        self.query_one("#rcpts", OptionList).action_cursor_down()

    def action_up(self) -> None:
        self.query_one("#rcpts", OptionList).action_cursor_up()

    def action_send(self) -> None:
        if self.focused and self.focused.id == "who":
            self.query_one("#rcpts").focus()
            return
        if not self.chosen:
            self.notify("Pick at least one recipient (space).", severity="warning")
            return
        self.dismiss(set(self.chosen))

    def action_cancel(self) -> None:
        self.dismiss(None)


class CameraScreen(ModalScreen[None]):
    """Live webcam preview, capture, caption and send — through Snapchat's camera."""

    BINDINGS = [
        Binding("escape", "back", "back"),
        Binding("space", "shutter", "photo"),
        Binding("v", "record", "video", show=False),
        Binding("left,h", "lens(-1)", "lens"),
        Binding("right,l", "lens(1)", "lens", show=False),
        Binding("enter", "send_to", "send to", priority=True),
    ]

    def __init__(self, app_: GhostctlApp, chat_name: str) -> None:
        super().__init__()
        self.app_, self.chat_name = app_, chat_name
        self.state = "starting"  # starting | live | capturing | preview | sending | closing
        self._loop: asyncio.Task | None = None

    def compose(self) -> ComposeResult:
        with Vertical(id="viewer"):
            yield Static("", id="viewer-top")
            yield Picture(self.app_.cfg["images"]["protocol"], id="cam-pic")
            caption = Input(placeholder="Add a caption (optional) — enter to choose recipients", id="caption")
            caption.display = caption.can_focus = False
            yield caption
            yield Static("", id="viewer-bottom")

    @property
    def bridge(self) -> Bridge:
        return self.app_.bridge

    def hud(self) -> None:
        top = Text()
        if self.state == "preview":
            top.append(" ■ Snap ready ", style=f"bold black on {YELLOW}")
        elif self.state in ("starting", "capturing", "sending"):
            top.append(f" ◌ {self.state}… ", style=DIM)
        else:
            top.append(" ● LIVE ", style=f"bold white on {SNAP_RED}")
        top.append(f"  to {self.chat_name}", style="bold")
        self.query_one("#viewer-top", Static).update(top)
        keys = {
            "live": [("space", "take snap"), ("←/→", "lens"), ("esc", "close")],
            "preview": [("type", "caption"), ("enter", "send to…"), ("esc", "retake")],
        }.get(self.state, [("esc", "close")])
        bottom = Text(" ")
        for k, label in keys:
            bottom.append(f" {k} ", style=f"bold {YELLOW}")
            bottom.append(f"{label}  ", style=DIM)
        self.query_one("#viewer-bottom", Static).update(bottom)
        # The caption box only exists in the preview. While live it must not hold
        # focus, or space/v get typed into it instead of taking the snap.
        caption = self.query_one("#caption", Input)
        caption.display = caption.can_focus = self.state == "preview"
        if self.state != "preview" and self.focused is caption:
            self.set_focus(None)

    async def on_mount(self) -> None:
        self.hud()
        try:
            await self.bridge.open_camera()
        except Exception as e:  # noqa: BLE001
            self.app_.notify(str(e), severity="error")
            self.dismiss(None)
            return
        self.state = "live"
        self.hud()
        self._loop = asyncio.create_task(self.live_loop())

    async def live_loop(self) -> None:
        pic = self.query_one("#cam-pic", Picture)
        while self.state == "live":
            data = await self.bridge.camera_frame()
            if data and self.state == "live":
                try:
                    await pic.show(PILImage.open(io.BytesIO(data)))
                except Exception:  # noqa: BLE001 - skip a bad frame
                    pass
            await asyncio.sleep(0.08)

    def _restart_live(self) -> None:
        self.state = "live"
        self.hud()
        self._loop = asyncio.create_task(self.live_loop())

    async def _show_preview(self) -> None:
        self.state = "preview"
        self.hud()
        try:
            media = await self.bridge.preview_media()
            path = M.save(media, f"snap-{datetime.now():%Y%m%d-%H%M%S}")
            img = _video_frame(path) if media.kind == "video" else path
            await self.query_one("#cam-pic", Picture).show(img, note=f"video saved to {path}")
        except Exception as e:  # noqa: BLE001
            self.notify(f"Preview: {e}", severity="warning")
        self.query_one("#caption").focus()

    async def action_shutter(self) -> None:
        if self.state != "live":
            return
        self.state = "capturing"
        self.hud()
        try:
            await self.bridge.capture_photo()
        except Exception as e:  # noqa: BLE001
            self.notify(str(e), severity="error")
            self._restart_live()
            return
        await self._show_preview()

    def action_record(self) -> None:
        # The web camera's shutter only handles clicks (checked 2026-10-05):
        # Snapchat Web takes photo snaps; video snaps need the phone app.
        self.notify("Snapchat Web can only take photo snaps — video snaps need the phone app.",
                    severity="warning")

    async def action_lens(self, step: int) -> None:
        if self.state == "live":
            try:
                await self.bridge.camera_lens(step)
            except Exception as e:  # noqa: BLE001
                self.notify(str(e), severity="warning")

    async def action_back(self) -> None:
        """esc: in the preview, throw the snap away and go back to the camera;
        otherwise close the camera."""
        if self.state == "preview":
            await self.action_discard()
        else:
            await self.action_close()

    async def action_discard(self) -> None:
        if self.state != "preview":
            return
        await self.bridge.discard_preview()
        self.query_one("#caption", Input).value = ""
        self._restart_live()

    @on(Input.Submitted, "#caption")
    async def caption_done(self) -> None:
        await self.action_send_to()

    async def action_send_to(self) -> None:
        if self.state != "preview":
            return
        caption = self.query_one("#caption", Input).value.strip()
        try:
            if caption:
                await self.bridge.set_caption(caption)
            rows = await self.bridge.open_send_to()
        except Exception as e:  # noqa: BLE001
            self.notify(str(e), severity="error")
            return
        self.app_.push_screen(SendToScreen(rows, self.chat_name), self._recipients_chosen)

    async def _recipients_chosen(self, chosen: set[int] | None) -> None:
        if chosen is None:
            await self.bridge.close_send_to()
            self.query_one("#caption").focus()
            return
        self.state = "sending"
        self.hud()
        try:
            rows = await self.bridge.set_recipients(chosen)
            got = {r["idx"] for r in rows if r["selected"]}
            if got != chosen:
                raise ActionError("Snapchat's recipient list didn't match your selection; not sent.")
            names = [r["name"] for r in rows if r["selected"]]
            await self.bridge.send_snap()
        except Exception as e:  # noqa: BLE001
            self.notify(str(e), severity="error")
            self.state = "preview"
            self.hud()
            return
        self.app_.notify(f"Snap sent to {', '.join(names)}", title="👻 Sent")
        await self.action_close()

    async def action_close(self) -> None:
        self.state = "closing"
        if self._loop:
            self._loop.cancel()
        try:
            await self.bridge.close_camera()
        except Exception as e:  # noqa: BLE001
            self.app_.notify(f"Camera: {e}", severity="warning")
        self.dismiss(None)


class LoginScreen(ModalScreen[tuple[str, str, bool] | None]):
    """Username + password + remember me. Extra steps (2FA...) come as PromptScreens."""

    BINDINGS = [Binding("escape", "cancel", "quit")]

    def __init__(self, can_remember: bool, error: str = "") -> None:
        super().__init__()
        self.can_remember, self.error = can_remember, error

    def compose(self) -> ComposeResult:
        with Vertical(classes="dialog login"):
            yield Center(Static(Text(GHOST_ART, style=YELLOW)))
            yield Static("")
            yield Center(Static(Text("Log in to Snapchat", style="bold")))
            yield Static(Text(self.error, style=SNAP_RED), id="login-msg")
            yield Input(placeholder="Username or email", id="user")
            yield Input(placeholder="Password", password=True, id="pw")
            yield Checkbox("Remember me — stored encrypted in your system keyring",
                           value=self.can_remember, disabled=not self.can_remember, id="remember")
            yield Static(Text.assemble(
                ("enter", f"bold {YELLOW}"), (" log in   ", DIM), ("esc", f"bold {YELLOW}"), (" quit", DIM),
                ("\nYour password goes only into Snapchat's own login form.", DIM)))

    def on_mount(self) -> None:
        self.query_one("#user").focus()

    @on(Input.Submitted, "#user")
    def next_field(self) -> None:
        self.query_one("#pw").focus()

    @on(Input.Submitted, "#pw")
    def submit(self) -> None:
        user = self.query_one("#user", Input).value.strip()
        pw = self.query_one("#pw", Input).value
        if not user:
            self.query_one("#user").focus()
            return
        if not pw:
            return
        self.dismiss((user, pw, self.query_one("#remember", Checkbox).value))

    def action_cancel(self) -> None:
        self.dismiss(None)


class PromptScreen(ModalScreen[str | None]):
    """One question during login (2FA code, password again, ...)."""

    BINDINGS = [Binding("escape", "cancel", "cancel")]

    def __init__(self, prompt: str, secret: bool, messages: list[str]) -> None:
        super().__init__()
        self.prompt, self.secret, self.messages = prompt, secret, messages

    def compose(self) -> ComposeResult:
        with Vertical(classes="dialog"):
            yield Static(Text("Snapchat", style=f"bold {YELLOW}"))
            for m in self.messages[-3:]:
                yield Static(Text(m, style=DIM))
            yield Input(placeholder=self.prompt, password=self.secret, id="answer")

    def on_mount(self) -> None:
        self.query_one("#answer").focus()

    @on(Input.Submitted, "#answer")
    def done(self, event: Input.Submitted) -> None:
        if event.value.strip():
            self.dismiss(event.value.strip())

    def action_cancel(self) -> None:
        self.dismiss(None)


class AppLoginUI:
    """Bridges run_login() to the TUI."""

    def __init__(self, app: GhostctlApp) -> None:
        self.app = app
        self.messages: list[str] = []

    async def ask(self, prompt: str, secret: bool = False) -> str | None:
        return await self.app.push_screen_wait(PromptScreen(prompt, secret, self.messages))

    def say(self, text: str) -> None:
        self.messages.append(text)
        self.app.status(f"Snapchat: {text}")

    async def confirm(self, question: str) -> bool:
        return await self.app.push_screen_wait(ConfirmScreen(question))


# --- main app ---


class GhostctlApp(App[int]):
    TITLE = "ghostctl"
    CSS = f"""
    Screen {{ background: $background; }}
    .dialog {{ width: 76; height: auto; max-height: 90%; border: round $primary; padding: 1 2;
               background: $surface; }}
    .dialog.wide {{ width: 90%; height: 80%; }}
    .dialog.tall {{ height: 85%; }}
    .dialog OptionList {{ height: auto; max-height: 24; border: none; background: $surface; }}
    .dialog.tall OptionList {{ height: 1fr; max-height: 100%; }}
    .dialog Input {{ margin: 1 0; }}
    .dialog.login {{ width: 64; }}
    .dialog.login Center {{ height: auto; }}
    .dialog.login Center Static {{ width: auto; }}
    .dialog.login Input {{ margin: 0 0 1 0; }}
    .dialog Checkbox {{ background: $surface; border: none; margin-bottom: 1; }}
    LoginScreen, PromptScreen {{ align: center middle; background: $background; }}
    HelpScreen, ConfirmScreen, MenuScreen, FilePickerScreen, SendToScreen {{ align: center middle; }}
    #login-msg {{ height: auto; margin-bottom: 1; }}
    ViewerScreen, CameraScreen {{ background: black; }}
    #viewer {{ background: black; height: 100%; }}
    #viewer-top, #viewer-bottom {{ height: 1; padding: 0 1; background: black; }}
    #viewer-top {{ margin-bottom: 1; }}
    #caption {{ margin: 0 4; border: tall {YELLOW}; background: black; }}

    #topbar {{ height: 1; background: $surface; }}
    #brand {{ width: auto; padding: 0 1; }}
    #topstatus {{ width: 1fr; content-align: right middle; padding: 0 1; color: $text-muted; }}
    #main {{ height: 1fr; }}
    #left {{ background: $surface; border-right: tall $panel; }}
    #left-title {{ height: 1; padding: 0 1; }}
    #search {{ display: none; margin: 0 1; border: tall $panel; }}
    #search.visible {{ display: block; }}
    #search:focus {{ border: tall $primary; }}
    #chats {{ height: 1fr; border: none; background: $surface; }}
    OptionList {{ scrollbar-size-vertical: 1; }}
    OptionList > .option-list--option-highlighted {{ color: $foreground; text-style: none; }}
    #chats > .option-list--option-highlighted {{ background: $boost; }}
    #chats:focus > .option-list--option-highlighted {{ background: $primary 16%; }}
    #chats > .option-list--option-hover {{ background: $boost; }}
    #right {{ background: $background; }}
    #conv-head {{ height: 3; padding: 0 2; border-bottom: solid $panel; }}
    #conv {{ height: 1fr; border: none; background: $background; padding: 0 1; }}
    #conv > .option-list--option-highlighted {{ background: $surface; }}
    #conv:focus > .option-list--option-highlighted {{ background: $boost; }}
    #conv > .option-list--option-hover {{ background: $surface; }}
    #activity {{ height: 1; padding: 0 2; color: $text-muted; }}
    #compose {{ margin: 0 1; border: tall $panel; background: $surface; }}
    #compose:focus {{ border: tall $primary; }}
    #empty {{ height: 1fr; align: center middle; }}
    #empty Static {{ width: auto; text-align: center; }}
    #status {{ height: 1; padding: 0 1; background: $surface; color: $text-muted; }}
    Footer {{ background: $surface; }}
    """

    def __init__(self) -> None:
        super().__init__()
        self.cfg = cfgmod.load()
        self.register_theme(GHOST_THEME)
        self.browser = from_config(self.cfg)
        self.bridge: Bridge | None = None
        self.chats: dict[str, Chat] = {}
        self.conv: Conversation | None = None
        self.open_id: str | None = None
        self.filter = ""
        self.mode_note = ""
        self.live = False
        self.saved: dict[str, bool] = {}  # message key -> saved in chat (learned from its menu)
        self.pending: list[Pending] = []
        self.reply_key: str | None = None
        self._feed_seen = False
        self._draft_task: asyncio.Task | None = None
        self._dumped_viewer = False
        self.keys = {name: self.cfg["keys"].get(name, "") for name in ACTIONS}
        for name, (action, _desc) in ACTIONS.items():
            if self.keys[name]:
                self.bind(self.keys[name], action, description=FOOTER.get(name, name), show=name in FOOTER)

    # --- layout ---

    def compose(self) -> ComposeResult:
        with Horizontal(id="topbar"):
            yield Static(Text.assemble(("👻 ", ""), ("ghostctl", f"bold {YELLOW}")), id="brand")
            yield Static("", id="topstatus")
        with Horizontal(id="main"):
            with Vertical(id="left"):
                yield Static("", id="left-title")
                yield Input(placeholder="Search", id="search")
                yield OptionList(id="chats")
            with Vertical(id="right"):
                with Vertical(id="empty"):
                    yield Center(Static(Text(GHOST_ART, style=YELLOW)))
                    yield Center(Static(""))
                    yield Center(Static(Text("Select a chat", style="bold")))
                    yield Center(Static(Text.assemble(
                        ("enter", f"bold {YELLOW}"), (" open · ", DIM), ("c", f"bold {YELLOW}"),
                        (" live snap · ", DIM), ("s", f"bold {YELLOW}"), (" stories · ", DIM),
                        ("?", f"bold {YELLOW}"), (" keys", DIM))))
                yield Static("", id="conv-head")
                yield OptionList(id="conv")
                yield Static("", id="activity")
                yield Input(placeholder="Send a chat", id="compose")
        yield Static("", id="status")
        yield Footer()

    def on_mount(self) -> None:
        ui = self.cfg["ui"]
        self.theme = ui["theme"] if ui["theme"] in self.available_themes else "ghost"
        self.query_one("#left").styles.width = int(ui["chat_list_width"])
        self.show_chat_pane(False)
        self.query_one("#chats").focus()
        if self.cfg.error:
            self.notify(self.cfg.error, severity="error", timeout=10)
        self.render_topbar()
        self.connect()

    def show_chat_pane(self, on_: bool) -> None:
        self.query_one("#empty").display = not on_
        for wid in ("#conv-head", "#conv", "#activity", "#compose"):
            self.query_one(wid).display = on_

    def render_topbar(self) -> None:
        t = Text()
        unread = sum(1 for c in self.chats.values() if c.unread)
        if unread:
            t.append(f" {unread} new ", style=f"bold black on {SNAP_RED}")
            t.append("  ")
        if self.bridge:
            t.append("● ", style=CALL_GREEN if self.live else YELLOW)
            t.append("live" if self.live else "polling", style=DIM)
        else:
            t.append("◌ connecting", style=DIM)
        if self.mode_note:
            t.append(f"  ·  {self.mode_note}", style=DIM)
        self.query_one("#topstatus", Static).update(t)
        title = Text("Chats", style="bold")
        if unread:
            title.append(f"  {unread}", style=f"bold {SNAP_RED}")
        self.query_one("#left-title", Static).update(title)

    def status(self, msg: str, error: bool = False) -> None:
        self.query_one("#status", Static).update(Text(msg, style=SNAP_RED if error else DIM))

    def ready_status(self) -> None:
        self.status("")

    def fail(self, e: Exception) -> None:
        msg = str(e).splitlines()[0] if str(e) else type(e).__name__
        self.status(f"✗ {msg}", error=True)
        if isinstance(e, ActionError):
            self.notify(msg, severity="warning")

    # --- connection ---

    @work(exclusive=True, group="connect", exit_on_error=False)
    async def connect(self) -> None:
        self.status("Starting browser…")
        try:
            st = await self.browser.start()
        except ProfileLocked as e:
            self.status(str(e), error=True)
            return
        except Exception as e:  # noqa: BLE001 - surface anything to the user
            self.status(f"Browser failed to start: {e}", error=True)
            return
        if st.session is Session.LOGGED_OUT:
            if not await self.log_in():
                self.status("Not logged in. Press q to quit, or restart ghostctl to try again.", error=True)
                return
        elif st.session is not Session.LOGGED_IN:
            self.status(st.note or f"Session {st.session.value}.", error=True)
            return
        proto = self.cfg["images"]["protocol"]
        proto = M.auto_protocol_name() if proto == "auto" else proto
        self.mode_note = f"{st.mode.value} · {proto}"
        if st.note:
            self.notify(st.note, timeout=8)
        self.status("Loading chats…")
        self.bridge = Bridge(self.browser.page, self.on_bridge_event, float(self.cfg["behavior"]["action_gap"]))
        try:
            await self.bridge.start()
        except SelectorError as e:
            self.status(str(e), error=True)
            return
        self.live = self.bridge.live
        self.render_topbar()
        self.ready_status()
        self.watch_session()

    async def log_in(self, error: str = "") -> bool:
        """Log in through the TUI: saved login first, else the login screen."""
        ui = AppLoginUI(self)
        saved = C.load()
        got = None
        if saved:
            self.status(f"Logging in as {saved.username} (saved login)…")
            got = await run_login(self.browser, ui, saved)
        while got is None:
            form = await self.push_screen_wait(LoginScreen(C.available(), error))
            if form is None:
                return False
            user, pw, remember = form
            self.status(f"Logging in as {user}…")
            got = await run_login(self.browser, ui, C.Creds(user, pw))
            if got is None:
                error = ui.messages[-1] if ui.messages else "Login didn't complete."
                await self.browser.goto_web()
                continue
            if remember and got.password:
                self.notify("Login saved to your keyring." if C.save(got) else "Couldn't write to the keyring.")
        self.status("Logged in. Saving session…")
        await asyncio.sleep(3)  # let the session reach the profile
        return True

    @work(exclusive=True, group="watch", exit_on_error=False)
    async def watch_session(self) -> None:
        """If Snapchat ends the session, log back in (saved login or login screen)."""
        while True:
            await asyncio.sleep(120)
            try:
                state = await self.browser._probe()
            except Exception:  # noqa: BLE001
                continue
            if state is not Session.LOGGED_OUT:
                continue
            self.notify("Snapchat logged you out — logging back in…", severity="warning")
            if self.bridge:
                await self.bridge.stop()
            if not await self.log_in():
                self.status("Logged out. Restart ghostctl to log in.", error=True)
                return
            await self.browser.goto_web()
            if self.bridge is None or self.bridge.page is not self.browser.page:
                # The browser was restarted (e.g. login finished in a window).
                self.bridge = Bridge(self.browser.page, self.on_bridge_event,
                                     float(self.cfg["behavior"]["action_gap"]))
            try:
                await self.bridge.start()
            except Exception as e:  # noqa: BLE001
                self.fail(e)
                return
            self.ready_status()

    def on_worker_state_changed(self, event) -> None:
        """Background tasks never crash the app; their errors go to the status bar."""
        from textual.worker import WorkerState

        if event.state is WorkerState.ERROR and event.worker.error is not None:
            self.fail(event.worker.error)

    def on_bridge_event(self, region: str, data: object) -> None:
        if region == "feed":
            self.notify_new(data)
            for chat in data:  # the list is virtualized: merge what is rendered
                self.chats[chat.id] = chat
            self._feed_seen = True
            self.render_chats()
            self.render_topbar()
        elif region == "conv":
            if data.id == self.open_id:
                self.conv = data
                self.settle_pending()
                self.render_conv()
        elif region == "error":
            self.status(f"page script error: {data}", error=True)

    def notify_new(self, rows: list[Chat]) -> None:
        if not self._feed_seen:
            return
        n = self.cfg["notifications"]
        for c in rows:
            old = self.chats.get(c.id)
            if not c.unread or c.id == self.open_id:
                continue
            if old and old.status == c.status and old.time == c.time:
                continue
            if n["bell"]:
                self.bell()
            if n["status_line"]:
                icon, _colour, _ = status_style(c.status)
                self.notify(f"{icon} {c.status}", title=c.name, timeout=5)
            if n["desktop"]:
                M.desktop_notify(c.name, c.status)

    # --- rendering ---

    def visible_chats(self) -> list[Chat]:
        chats = sorted(self.chats.values(), key=lambda c: c.time.timestamp() if c.time else 0, reverse=True)
        if self.filter:
            f = self.filter.lower()
            chats = [c for c in chats if f in c.name.lower()]
        return chats

    def render_chats(self) -> None:
        ui = self.cfg["ui"]
        ol = self.query_one("#chats", OptionList)
        # Real row width: the list minus its scrollbar, with a cell of slack for
        # emoji whose terminal width differs from Rich's estimate.
        width = (ol.scrollable_content_region.width or int(ui["chat_list_width"]) - 3) - 2
        keep = ol.highlighted_option.id if ol.highlighted_option else None
        ol.clear_options()
        for c in self.visible_chats():
            icon, colour, strong = status_style(c.status)
            when = _ago(c.time)
            name = c.name
            room = width - 3 - cell_len(when)
            if cell_len(name) > room:
                while cell_len(name) > room - 1 and len(name) > 1:
                    name = name[:-1]
                name += "…"
            line1 = Text(" ")
            line1.append(f"{icon} ", style=f"bold {colour}")
            line1.append(name, style="bold" if c.unread else "")
            line1.append(" " * max(1, room - cell_len(name) + 1))
            line1.append(when, style=f"bold {colour}" if c.unread else DIM)
            if ui["compact_chat_list"]:
                ol.add_option(Option(line1, id=c.id))
                continue
            line2 = Text("   ")
            line2.append(c.status or " ", style=f"bold {colour}" if strong else DIM)
            if c.streak and ui["show_streaks"]:
                line2.append(f"  {c.streak.replace(' ', '')}")
            if c.badge and ui["show_badges"]:
                line2.append(f" {c.badge}")
            if c.group and ui["show_group_tag"]:
                line2.append("  group", style=DIM)
            ol.add_option(Option(Group(line1, line2, Text("")), id=c.id))
        if keep:
            try:
                ol.highlighted = ol.get_option_index(keep)
            except Exception:  # noqa: BLE001 - filtered out
                pass

    def _sender_colour(self, m: Message) -> str:
        ui = self.cfg["ui"]
        return (ui["me_color"] if m.mine else ui["them_color"]).replace("bold", "").strip()

    def _msg(self, m: Message, show_header: bool) -> Group:
        ui = self.cfg["ui"]
        colour = self._sender_colour(m)
        parts = []
        if show_header:
            head = Text()
            head.append((m.sender or "?").upper(), style=f"bold {colour}")
            if m.time:
                head.append(f"  {m.time.strftime(ui['time_format'])}", style=DIM)
            parts.append(head)
        body = Text()
        if m.quote_text:
            body.append(f"╭ {m.quote_sender}\n", style=DIM)
            body.append(f"│ {m.quote_text}\n", style=f"italic {DIM}")
        lines = []
        if m.text:
            lines.append(Text(m.text))
        for kind in m.media:
            c = VIDEO_PURPLE if kind == "video" else CHAT_BLUE
            lines.append(Text.assemble(("▣ ", f"bold {c}"), ("Video" if kind == "video" else "Photo", f"bold {c}"),
                                       ("   o to view", DIM)))
        if m.snap_new:
            lines.append(Text.assemble(("■ ", f"bold {SNAP_RED}"), (m.snap_status or "New Snap", f"bold {SNAP_RED}"),
                                       ("   o to open", DIM)))
        elif m.snap_status:
            icon, c, _ = status_style(m.snap_status)
            lines.append(Text.assemble((f"{icon} ", f"bold {c}"), (m.snap_status, c)))
        if m.not_supported:
            lines.append(Text("◇ Not supported on web — check your phone", style=DIM))
        if not lines:
            lines.append(Text("▣ Media", style=f"bold {CHAT_BLUE}"))
        body.append_text(Text("\n").join(lines))
        if self.saved.get(m.key):
            body.append("  ◆ saved", style=f"bold {DIM}")
        parts.append(Barred(body, colour))
        if m.reactions and ui["show_reactions"]:
            parts.append(Text("  " + "   ".join(reaction_label(r) for r in m.reactions), style=DIM))
        return Group(*parts)

    def render_conv(self) -> None:
        ui = self.cfg["ui"]
        ol = self.query_one("#conv", OptionList)
        at_end = ol.highlighted is None or ol.highlighted >= ol.option_count - 1
        keep = ol.highlighted_option.id if ol.highlighted_option else None
        ol.clear_options()
        prev: Message | None = None
        conv = self.conv
        for item in conv.items if conv else []:
            if isinstance(item, DateMark):
                if ui["show_date_separators"]:
                    ol.add_option(Option(Text(f"\n─────  {item.label}  ─────", style=DIM, justify="center"),
                                         disabled=True))
                prev = None
            elif isinstance(item, Notice):
                ol.add_option(Option(Text(item.text, style=f"italic {DIM}", justify="center"), disabled=True))
                prev = None
            else:
                header = prev is None or prev.sender != item.sender or prev.time != item.time
                if header and prev is not None:
                    ol.add_option(Option(Text(""), disabled=True))
                ol.add_option(Option(self._msg(item, header), id=item.key))
                prev = item
        for p in self.pending:
            if p.chat_id != self.open_id:
                continue
            body = Text(p.text)
            body.append("\n")
            if p.failed:
                body.append(f"✗ not sent: {p.failed}", style=f"bold {SNAP_RED}")
            else:
                body.append("◌ sending…", style=f"italic {DIM}")
            ol.add_option(Option(Barred(body, DIM), disabled=True))
        if ol.option_count:
            idx = ol.option_count - 1
            if keep and not at_end:
                try:
                    idx = ol.get_option_index(keep)
                except Exception:  # noqa: BLE001 - message vanished
                    pass
            while idx > 0 and ol.get_option_at_index(idx).disabled:
                idx -= 1
            ol.highlighted = idx
            if at_end:
                ol.scroll_end(animate=False)
        act = Text()
        if conv and conv.typing:
            act.append("✎ ", style=f"bold {YELLOW}")
            act.append(conv.activity or "typing…", style=YELLOW)
        elif conv and conv.activity:
            act.append(conv.activity)
        if conv and conv.seen_by:
            if act:
                act.append("   ")
            act.append("seen by ", style=DIM)
            act.append(", ".join(conv.seen_by))
        self.query_one("#activity", Static).update(act)

    def render_conv_head(self, chat: Chat) -> None:
        icon, colour, _ = status_style(chat.status)
        t = Text()
        t.append(chat.name, style="bold")
        if chat.badge:
            t.append(f"  {chat.badge}")
        t.append("\n")
        sub = []
        if chat.group:
            sub.append(("group", DIM))
        if chat.streak:
            sub.append((chat.streak.replace(" ", ""), ""))
        sub.append((f"{icon} {chat.status}", colour))
        for i, (s, st) in enumerate(sub):
            if i:
                t.append("  ·  ", style=DIM)
            t.append(s, style=st)
        self.query_one("#conv-head", Static).update(t)

    def settle_pending(self) -> None:
        """Drop pending messages that now show up in the chat."""
        if not self.pending or not self.conv:
            return
        mine = [" ".join(m.text.split()) for m in self.conv.items if isinstance(m, Message) and m.mine]
        recent = set(mine[-15:])
        self.pending = [p for p in self.pending if p.failed or " ".join(p.text.split()) not in recent]

    def selected_message(self) -> Message | None:
        ol = self.query_one("#conv", OptionList)
        opt = ol.highlighted_option
        if not opt or not self.conv:
            return None
        return next((m for m in self.conv.items if isinstance(m, Message) and m.key == opt.id), None)

    def viewer_info(self, v: dict, fallback_sender: str = "") -> dict:
        when = ""
        if v.get("time"):
            try:
                when = _ago(datetime.fromisoformat(v["time"].replace("Z", "+00:00")).astimezone()) + " ago"
            except ValueError:
                pass
        return {"sender": v.get("sender") or fallback_sender, "when": when}

    # --- background page actions ---

    def page_action(self, coro_fn, *args, done: str = "", group: str = "page"):
        """Run a bridge coroutine in a worker, reporting errors in the status bar."""

        async def run():
            if not self.bridge:
                self.notify("Not connected yet.")
                return None
            try:
                result = await coro_fn(*args)
            except Exception as e:  # noqa: BLE001 - shown in the status bar
                self.fail(e)
                return None
            if done:
                self.notify(done)
            self.ready_status()
            return result

        return self.run_worker(run(), group=group, exit_on_error=False)

    async def _safe(self, coro) -> None:
        try:
            await coro
        except Exception as e:  # noqa: BLE001
            self.fail(e)

    # --- navigation actions ---

    def _focused(self) -> str | None:
        return self.focused.id if self.focused else None

    def action_down(self) -> None:
        ol = self.focused
        if not isinstance(ol, OptionList):
            return
        if ol.id == "chats" and ol.highlighted is not None and ol.highlighted >= ol.option_count - 1:
            self.page_action(self.bridge.load_more_chats)
        ol.action_cursor_down()

    def action_up(self) -> None:
        ol = self.focused
        if not isinstance(ol, OptionList):
            return
        first = next((i for i in range(ol.option_count) if not ol.get_option_at_index(i).disabled), 0)
        if ol.id == "conv" and (ol.highlighted is None or ol.highlighted <= first):
            self.status("Loading older messages…")
            self.page_action(self.bridge.load_older)
        ol.action_cursor_up()

    def action_open(self) -> None:
        if self._focused() == "chats":
            opt = self.query_one("#chats", OptionList).highlighted_option
            if opt and opt.id in self.chats:
                self.open_chat(self.chats[opt.id])
        elif self._focused() == "conv":
            self.action_menu()

    @on(OptionList.OptionSelected, "#chats")
    def chat_selected(self, event: OptionList.OptionSelected) -> None:
        chat = self.chats.get(event.option.id)
        if chat:
            self.open_chat(chat)

    @on(OptionList.OptionSelected, "#conv")
    def message_selected(self) -> None:
        self.action_menu()

    @work(exclusive=True, group="open", exit_on_error=False)
    async def open_chat(self, chat: Chat) -> None:
        if not self.bridge:
            return
        self.open_id = chat.id
        self.conv = None
        self.show_chat_pane(True)
        self.render_conv_head(chat)
        self.render_conv()
        self.status(f"Opening {chat.name}…")
        try:
            await self.bridge.open_chat(chat.id)
        except Exception as e:  # noqa: BLE001
            self.fail(e)
            return
        self.query_one("#conv").focus()
        self.ready_status()

    def action_search(self) -> None:
        box = self.query_one("#search", Input)
        box.add_class("visible")
        box.focus()

    @on(Input.Changed, "#search")
    def search_changed(self, event: Input.Changed) -> None:
        self.filter = event.value
        self.render_chats()

    @on(Input.Submitted, "#search")
    def search_done(self) -> None:
        self.query_one("#chats").focus()

    async def action_back(self) -> None:
        focus = self._focused()
        if focus == "compose":
            self.end_compose(clear_page=True)
            return
        box = self.query_one("#search", Input)
        if focus == "search" or (self.filter and focus == "chats"):
            box.value = ""
            box.remove_class("visible")
            self.query_one("#chats").focus()
            return
        if focus == "conv":
            self.query_one("#chats").focus()
            if self.cfg["behavior"]["close_chat_on_back"]:
                self.open_id = None
                self.show_chat_pane(False)
                self.page_action(self.bridge.close_chat)

    def action_refresh(self) -> None:
        self.page_action(self.bridge.refresh, done="Refreshed")

    def action_mark_read(self) -> None:
        if self._focused() != "chats":
            self.notify("Select a chat in the list first.")
            return
        opt = self.query_one("#chats", OptionList).highlighted_option
        if opt:
            chat = self.chats[opt.id]
            if "snap" in chat.status.lower():
                self.notify("Opening a chat doesn't open its snaps; they stay unopened.")
            self.page_action(self.bridge.mark_read, chat.id, done=f"Marked {chat.name} as read")

    def action_call(self) -> None:
        self.notify("Voice/video calls are not supported in the terminal.", severity="warning")

    def action_help(self) -> None:
        self.push_screen(HelpScreen(self.keys, cfgmod.CONFIG_PATH))

    async def action_quit(self) -> None:
        if self.bridge:
            await self.bridge.stop()
        await self.browser.close()
        self.exit(0)

    # --- composing ---

    def _need_chat(self) -> bool:
        if not self.open_id:
            self.notify("Open a chat first.")
            return False
        return True

    def action_compose(self, reply_key: str | None = None) -> None:
        if not self._need_chat():
            return
        self.reply_key = reply_key
        box = self.query_one("#compose", Input)
        box.placeholder = "Reply…" if reply_key else "Send a chat   (/send ~/pic.jpg attaches a photo)"
        box.focus()

    def action_reply(self) -> None:
        m = self.selected_message()
        if not m:
            self.notify("Select a message in the chat to reply to.")
            return
        self.action_compose(reply_key=m.key)
        self.status(f"↩ replying to {m.sender}: {(m.text or 'media')[:60]}")

    def end_compose(self, clear_page: bool = False) -> None:
        box = self.query_one("#compose", Input)
        box.value = ""
        box.placeholder = "Send a chat"
        self.reply_key = None
        self.query_one("#conv").focus()
        if clear_page and self.cfg["behavior"]["send_typing"] and self.bridge:
            self.page_action(self.bridge.set_draft, "", group="draft")
        self.ready_status()

    @on(Input.Changed, "#compose")
    def draft_changed(self, event: Input.Changed) -> None:
        if not self.cfg["behavior"]["send_typing"] or event.value.startswith("/") or not self.bridge:
            return
        if self._draft_task:
            self._draft_task.cancel()
        if not event.value:
            return

        async def later(text: str) -> None:
            await asyncio.sleep(0.6)  # debounce: mirror after a pause in typing
            try:
                await self.bridge.set_draft(text)
            except Exception:  # noqa: BLE001 - typing mirror is best-effort
                pass

        self._draft_task = asyncio.create_task(later(event.value))

    @on(Input.Submitted, "#compose")
    def draft_submitted(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        if self._draft_task:
            self._draft_task.cancel()
        if not text:
            return
        if text.startswith("/send "):
            path = Path(text[6:].strip()).expanduser()
            self.end_compose()
            self.confirm_send_file(path)
            return
        if text.split()[0] in ("/call", "/video", "/lens"):
            self.end_compose(clear_page=True)
            self.notify("Calls are not supported in the terminal. Lenses: press c for the camera.",
                        severity="warning")
            return
        key = self.reply_key
        chat_id = self.open_id
        self.end_compose()
        self.query_one("#compose", Input).focus()  # keep chatting
        self.send_message(chat_id, text, key)

    @work(group="send", exit_on_error=False)
    async def send_message(self, chat_id: str, text: str, reply_key: str | None) -> None:
        p = Pending(chat_id, text, time.monotonic())
        self.pending.append(p)
        self.render_conv()
        try:
            if reply_key:
                await self.bridge.reply(reply_key, text)
            else:
                await self.bridge.send_text(text)
        except Exception as e:  # noqa: BLE001
            p.failed = str(e).splitlines()[0]
            self.render_conv()
            self.fail(e)
            return
        # Accepted by Snapchat; the pending line goes once the message shows up in
        # the conversation (settle_pending), or after 10 s at the latest.
        await asyncio.sleep(10)
        if p in self.pending and not p.failed:
            self.pending.remove(p)
            self.render_conv()

    # --- message menu ---

    @work(group="menu", exit_on_error=False)
    async def action_menu(self) -> None:
        m = self.selected_message()
        if not m or not self.bridge:
            self.notify("Select a message first.")
            return
        try:
            entries = await self.bridge.open_menu(m.key)
        except Exception as e:  # noqa: BLE001
            self.fail(e)
            return
        if "Unsave in Chat" in entries:
            self.saved[m.key] = True
        elif "Save in Chat" in entries:
            self.saved[m.key] = False
        self.render_conv()
        options: list[tuple[str, Text | str]] = []
        for e in entries:
            if e.startswith("react:"):
                name = e.split(":", 1)[1]
                options.append((e, Text.assemble(f" {REACTION_EMOJI.get(name, '•')}  ", (name, ""))))
            else:
                icon = {"Save in Chat": "◆", "Unsave in Chat": "◇", "Reply": "↩", "Copy Text": "⧉",
                        "Delete": "✗"}.get(e, "•")
                options.append((e, Text.assemble(f" {icon}  ", (e, f"bold {SNAP_RED}" if e == "Delete" else "bold"))))
        state = "◆ saved" if self.saved.get(m.key) else "not saved"
        title = Text.assemble((m.sender, f"bold {self._sender_colour(m)}"), ("  ", ""),
                              ((m.text or "media")[:40], ""), (f"   {state}", DIM))
        choice = await self.push_screen_wait(MenuScreen(title, options))
        if choice is None:
            await self._safe(self.bridge.close_menu())
            return
        if choice == "Copy Text":
            await self._safe(self.bridge.close_menu())
            self.copy_to_clipboard(m.text)
            self.notify("Copied")
            return
        if choice == "Reply":
            await self._safe(self.bridge.close_menu())
            self.action_reply()
            return
        if choice == "Delete":
            await self._safe(self.bridge.close_menu())
            if not await self.push_screen_wait(ConfirmScreen("Delete this message for everyone?", "delete")):
                return
            try:
                await self.bridge.open_menu(m.key)
                await self.bridge.choose_menu("Delete")
                await asyncio.sleep(0.8)
                await self.bridge.choose_menu_confirm("Delete")
            except Exception as e:  # noqa: BLE001
                self.fail(e)
            return
        try:
            await self.bridge.choose_menu(choice)
        except Exception as e:  # noqa: BLE001
            self.fail(e)
            return
        if choice in ("Save in Chat", "Unsave in Chat"):
            self.saved[m.key] = choice == "Save in Chat"
            self.render_conv()
            self.notify("Saved in chat" if self.saved[m.key] else "Unsaved")
        elif choice.startswith("react:"):
            self.notify(f"Reacted {REACTION_EMOJI.get(choice[6:], '')}")

    # --- media, snaps, camera ---

    def _dump_dir(self) -> Path | None:
        if self.cfg["debug"]["dump_viewer"] and not self._dumped_viewer:
            self._dumped_viewer = True
            return DEBUG_DIR
        return None

    @work(group="media", exit_on_error=False)
    async def action_open_media(self) -> None:
        m = self.selected_message()
        if not m or not self.bridge:
            self.notify("Select a snap or media message first.")
            return
        if m.not_supported:
            self.notify("Snapchat Web can't show this one; check your phone.")
            return
        try:
            if m.snap_new:
                if self.cfg["behavior"]["confirm_snap_open"]:
                    ok = await self.push_screen_wait(ConfirmScreen(
                        Text.assemble(("■ ", f"bold {SNAP_RED}"), (f"Open the snap from {m.sender}?\n\n", "bold"),
                                      ("Opening marks it as viewed; it can't be opened again.", DIM)), "open"))
                    if not ok:
                        return
                self.status("Opening snap…")
                media, v = await self.bridge.open_snap(m.key, self._dump_dir())
                self.ready_status()
                await self.push_screen_wait(ViewerScreen(
                    self, [(media, self.viewer_info(v, m.sender))], "Snap", viewer=True))
            elif m.media:
                self.status("Loading media…")
                items = await self.bridge.message_media(m.key)
                self.ready_status()
                if not items:
                    self.notify("No media found in that message.")
                    return
                when = m.time.strftime(self.cfg["ui"]["time_format"]) if m.time else ""
                await self.push_screen_wait(ViewerScreen(
                    self, [(i, {"sender": m.sender, "when": when}) for i in items], "Photo", viewer=False))
            elif m.snap_status:
                self.notify(f"This snap was already {m.snap_status.lower()}; snaps can only be viewed once.")
            else:
                self.notify("Nothing to open in this message.")
        except Exception as e:  # noqa: BLE001
            self.fail(e)

    @work(group="media", exit_on_error=False)
    async def action_stories(self) -> None:
        if not self.bridge:
            return
        try:
            if self.open_id:
                self.open_id = None
                self.show_chat_pane(False)
                await self.bridge.close_chat()
                self.query_one("#chats").focus()
            label = await self.bridge.stories_available()
            if "no stories" in label.lower():
                self.notify("No stories to view right now.")
                return
            self.status("Opening stories…")
            media, v = await self.bridge.open_stories(self._dump_dir())
            self.ready_status()
            await self.push_screen_wait(ViewerScreen(self, [(media, self.viewer_info(v))], "Story", viewer=True))
        except Exception as e:  # noqa: BLE001
            self.fail(e)

    def action_camera(self) -> None:
        if not self._need_chat() or not self.bridge:
            return
        name = self.chats[self.open_id].name if self.open_id in self.chats else "this chat"
        self.push_screen(CameraScreen(self, name))

    @work(group="media", exit_on_error=False)
    async def action_send_file(self) -> None:
        if not self._need_chat():
            return
        path = await self.push_screen_wait(FilePickerScreen())
        if path:
            self.confirm_send_file(path)

    @work(group="media", exit_on_error=False)
    async def confirm_send_file(self, path: Path) -> None:
        if not self._need_chat():
            return
        if not path.is_file():
            self.notify(f"No such file: {path}", severity="error")
            return
        name = self.chats[self.open_id].name if self.open_id in self.chats else "this chat"
        if not await self.push_screen_wait(ConfirmScreen(f"Send {path.name} to {name}?", "send")):
            return
        self.status(f"Sending {path.name}…")
        try:
            result = await self.bridge.send_file(path)
        except Exception as e:  # noqa: BLE001
            self.fail(e)
            return
        self.notify(f"{path.name}: {result}")
        self.ready_status()


def run_app() -> int:
    return GhostctlApp().run() or 0

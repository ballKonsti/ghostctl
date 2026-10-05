"""Textual UI: chat list, conversation, composer, snaps/media viewer."""

from __future__ import annotations

import asyncio
import shutil
import subprocess
from datetime import datetime
from pathlib import Path

from rich.text import Text
from textual import on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import DirectoryTree, Footer, Input, OptionList, Static
from textual.widgets.option_list import Option

from . import config as cfgmod
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
    "send_file": ("send_file", "send an image from a file"),
    "search": ("search", "search chats"),
    "mark_read": ("mark_read", "mark the selected chat as read"),
    "stories": ("stories", "view stories"),
    "refresh": ("refresh", "re-read the page"),
    "call": ("call", "start a call (not supported)"),
    "help": ("help", "this help"),
    "quit": ("quit", "quit"),
}

DEBUG_DIR = HOME / "debug"


def _ago(t: datetime | None) -> str:
    if t is None:
        return ""
    secs = (datetime.now(t.tzinfo) - t).total_seconds()
    for unit, n in (("d", 86400), ("h", 3600), ("m", 60)):
        if secs >= n:
            return f"{int(secs // n)}{unit}"
    return "now"


# --- modal screens ---


class HelpScreen(ModalScreen[None]):
    BINDINGS = [Binding("escape,q,question_mark", "dismiss", "close")]

    def __init__(self, keys: dict[str, str], config_path: Path) -> None:
        super().__init__()
        self.keys, self.config_path = keys, config_path

    def compose(self) -> ComposeResult:
        t = Text("ghostctl keys\n\n", style="bold")
        for name, (_, desc) in ACTIONS.items():
            t.append(f"  {self.keys.get(name, ''):<14}", style="bold cyan")
            t.append(f"{desc}\n")
        t.append(f"\nRebind keys and change colours/behaviour in\n  {self.config_path}\n", style="dim")
        t.append("Run `ghostctl config` to create it. ctrl+p switches themes live.", style="dim")
        yield VerticalScroll(Static(t), id="dialog")


class ConfirmScreen(ModalScreen[bool]):
    BINDINGS = [Binding("y", "yes", "yes"), Binding("n,escape", "no", "no")]

    def __init__(self, question: str) -> None:
        super().__init__()
        self.question = question

    def compose(self) -> ComposeResult:
        yield Vertical(Static(self.question), Static("\n[b]y[/b] yes   [b]n[/b] no", classes="dim"), id="dialog")

    def action_yes(self) -> None:
        self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)


class MenuScreen(ModalScreen[str | None]):
    BINDINGS = [Binding("escape,q", "cancel", "cancel"), Binding("j", "down", show=False),
                Binding("k", "up", show=False)]

    def __init__(self, title: str, options: list[tuple[str, str]]) -> None:
        super().__init__()
        self.title_text, self.options = title, options

    def compose(self) -> ComposeResult:
        with Vertical(id="dialog"):
            yield Static(self.title_text, classes="bold")
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
        with Vertical(id="picker"):
            yield Static("Send an image (png/jpeg/gif). Type a path or pick one below.")
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


class MediaScreen(ModalScreen[None]):
    """Shows an image (or a video's first frame) in the terminal."""

    BINDINGS = [
        Binding("escape,q", "close", "close"),
        Binding("n,space", "next", "next snap/story"),
        Binding("p", "play", "play video"),
    ]

    def __init__(self, app_: GhostctlApp, items: list[Media], title: str, viewer: bool) -> None:
        super().__init__()
        self.app_, self.items, self.title_text, self.viewer = app_, items, title, viewer
        self.index = 0
        self.paths: list[Path] = []

    def compose(self) -> ComposeResult:
        with Vertical(id="media"):
            yield Static(self.title_text, id="media-title")
            yield Vertical(id="media-body")
            yield Static("", id="media-hint")

    async def on_mount(self) -> None:
        await self.show()

    async def show(self) -> None:
        item = self.items[self.index]
        stem = f"{datetime.now():%Y%m%d-%H%M%S}-{self.index}"
        path = M.save(item, stem)
        self.paths.append(path)
        body = self.query_one("#media-body")
        await body.remove_children()
        img_path = path
        hint = f"saved: {path}"
        if item.kind == "video":
            img_path = _video_frame(path)
            hint = f"video · p to play · {hint}"
            if self.app_.cfg["images"]["video_player"] and len(self.items) == 1:
                self.notify(M.play(path, self.app_.cfg["images"]["video_player"]))
        cls = M.image_widget_class(self.app_.cfg["images"]["protocol"])
        if cls is None or img_path is None:
            await body.mount(Static(f"[{item.kind}] {path}"))
        else:
            await body.mount(cls(img_path, classes="img"))
        more = "n next · " if self.viewer or self.index + 1 < len(self.items) else ""
        self.query_one("#media-hint", Static).update(f"{more}esc close · {hint}")

    def action_play(self) -> None:
        if self.items[self.index].kind == "video":
            self.notify(M.play(self.paths[-1], self.app_.cfg["images"]["video_player"]))

    async def action_next(self) -> None:
        if self.index + 1 < len(self.items):
            self.index += 1
            await self.show()
            return
        if self.viewer and self.app_.bridge:
            nxt = await self.app_.bridge.viewer_next()
            if nxt is not None:
                self.items.append(nxt)
                self.index += 1
                await self.show()
                return
        await self.action_close()

    async def action_close(self) -> None:
        if self.viewer and self.app_.bridge:
            await self.app_.bridge.close_viewer()
        self.dismiss(None)


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


# --- main app ---


class GhostctlApp(App[int]):
    TITLE = "ghostctl"
    CSS = """
    #dialog { width: 72; height: auto; max-height: 90%; border: round $accent; padding: 1 2; background: $surface; }
    #menu { height: auto; max-height: 20; border: none; }
    #picker { width: 90%; height: 80%; border: round $accent; padding: 0 1; background: $surface; }
    #media { width: 95%; height: 95%; border: round $accent; background: $surface; }
    #media-title, #media-hint { height: 1; padding: 0 1; }
    #media-body { height: 1fr; align: center middle; }
    .img { width: auto; height: 100%; }
    HelpScreen, ConfirmScreen, MenuScreen, FilePickerScreen, MediaScreen { align: center middle; }
    .bold { text-style: bold; }
    .dim { color: $text-muted; }
    #main { height: 1fr; }
    #left { border-right: tall $panel; }
    #search { display: none; }
    #search.visible { display: block; }
    #chats, #conv { height: 1fr; border: none; }
    #conv-title { height: 1; padding: 0 1; background: $boost; text-style: bold; }
    #activity { height: 1; padding: 0 1; color: $text-muted; }
    #compose { display: none; }
    #compose.visible { display: block; }
    #status { height: 1; background: $boost; padding: 0 1; }
    """

    def __init__(self) -> None:
        super().__init__()
        self.cfg = cfgmod.load()
        self.browser = from_config(self.cfg)
        self.bridge: Bridge | None = None
        self.chats: dict[str, Chat] = {}
        self.conv: Conversation | None = None
        self.open_id: str | None = None
        self.filter = ""
        self.mode_note = ""
        self.saved: dict[str, bool] = {}  # message key -> saved in chat (learned from its menu)
        self.reply_key: str | None = None
        self._feed_seen = False
        self._draft_task: asyncio.Task | None = None
        self._dumped_viewer = False
        self.keys = {name: self.cfg["keys"].get(name, "") for name in ACTIONS}
        footer = {"compose": "write", "reply": "reply", "menu": "react/save", "open_media": "open snap",
                  "send_file": "send image", "search": "search", "stories": "stories", "help": "help",
                  "quit": "quit"}
        for name, (action, desc) in ACTIONS.items():
            if self.keys[name]:
                self.bind(self.keys[name], action, description=footer.get(name, name), show=name in footer)

    # --- layout ---

    def compose(self) -> ComposeResult:
        with Horizontal(id="main"):
            with Vertical(id="left"):
                yield Input(placeholder="search chats", id="search")
                yield OptionList(id="chats")
            with Vertical():
                yield Static("", id="conv-title")
                yield OptionList(id="conv")
                yield Static("", id="activity")
                yield Input(placeholder="message  (enter send · esc cancel · /send <path>)", id="compose")
        yield Static("Starting browser...", id="status")
        yield Footer()

    def on_mount(self) -> None:
        ui = self.cfg["ui"]
        if ui["theme"] in self.available_themes:
            self.theme = ui["theme"]
        self.query_one("#left").styles.width = int(ui["chat_list_width"])
        self.query_one("#chats").focus()
        if self.cfg.error:
            self.notify(self.cfg.error, severity="error", timeout=10)
        self.connect()

    def status(self, msg: str, error: bool = False) -> None:
        line = Text(msg, style="red" if error else "")
        if self.mode_note and not error:
            line.append(f"  | {self.mode_note}", style="dim")
        self.query_one("#status", Static).update(line)

    def ready_status(self) -> None:
        if self.bridge:
            self.status("live updates" if self.bridge.live else "polling (observer failed to start)")

    def fail(self, e: Exception) -> None:
        self.status(str(e), error=True)
        if isinstance(e, ActionError):
            self.notify(str(e), severity="warning")

    # --- connection ---

    @work(exclusive=True, group="connect")
    async def connect(self) -> None:
        self.status("Starting browser...")
        try:
            st = await self.browser.start()
        except ProfileLocked as e:
            self.status(str(e), error=True)
            return
        except Exception as e:  # noqa: BLE001 - surface anything to the user
            self.status(f"Browser failed to start: {e}", error=True)
            return
        if st.session is not Session.LOGGED_IN:
            self.status(st.note or f"Session {st.session.value}. Run `ghostctl login`.", error=True)
            return
        proto = self.cfg["images"]["protocol"]
        proto = f"auto→{M.auto_protocol_name()}" if proto == "auto" else proto
        self.mode_note = f"{st.mode.value} · images: {proto} ({M.detect_terminal()})"
        if st.note:
            self.notify(st.note, timeout=8)
        self.status("Loading chats...")
        self.bridge = Bridge(self.browser.page, self.on_bridge_event, float(self.cfg["behavior"]["action_gap"]))
        try:
            await self.bridge.start()
        except SelectorError as e:
            self.status(str(e), error=True)
            return
        self.ready_status()

    def on_bridge_event(self, region: str, data: object) -> None:
        if region == "feed":
            self.notify_new(data)
            for chat in data:  # the list is virtualized: merge what is rendered
                self.chats[chat.id] = chat
            self._feed_seen = True
            self.render_chats()
        elif region == "conv":
            if data.id == self.open_id:
                self.conv = data
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
                self.status(f"{c.status} from {c.name}")
            if n["desktop"]:
                M.desktop_notify("ghostctl", f"{c.status} from {c.name}")

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
        keep = ol.highlighted_option.id if ol.highlighted_option else None
        ol.clear_options()
        for c in self.visible_chats():
            t = Text()
            t.append(f"{ui['unread_marker']} " if c.unread else "  ", style=ui["unread_color"])
            t.append(c.name, style="bold" if c.unread else "")
            if c.group and ui["show_group_tag"]:
                t.append("  [group]", style="dim")
            if c.badge and ui["show_badges"]:
                t.append(f" {c.badge}")
            t.append(" " if ui["compact_chat_list"] else "\n  ")
            t.append(c.status, style=ui["unread_color"] if c.unread else "dim")
            t.append(f" · {_ago(c.time)}", style="dim")
            if c.streak and ui["show_streaks"]:
                t.append(f" · {c.streak}")
            ol.add_option(Option(t, id=c.id))
        if keep:
            try:
                ol.highlighted = ol.get_option_index(keep)
            except Exception:  # noqa: BLE001 - filtered out
                pass

    def _msg_text(self, m: Message, show_header: bool) -> Text:
        ui = self.cfg["ui"]
        t = Text()
        if show_header:
            when = m.time.strftime(ui["time_format"]) if m.time else ""
            t.append(m.sender or "?", style=ui["me_color"] if m.mine else ui["them_color"])
            t.append(f"  {when}\n", style="dim")
        if m.quote_text:
            t.append(f"┃ {m.quote_sender}: {m.quote_text}\n", style="dim italic")
        body = []
        if m.text:
            body.append(Text(m.text))
        for kind in m.media:
            body.append(Text(f"[{kind} · o to view]", style="magenta"))
        if m.snap_new:
            body.append(Text(f"[{m.snap_status or 'New Snap'} · o to open]", style="bold yellow"))
        elif m.snap_status:
            body.append(Text(f"[snap · {m.snap_status}]", style="yellow"))
        if m.not_supported:
            body.append(Text("[not supported on web — check your phone]", style="dim"))
        if not body:
            body.append(Text("[media]", style="magenta"))
        t.append_text(Text("\n").join(body))
        if self.saved.get(m.key):
            t.append("  💾 saved", style="green")
        if m.reactions and ui["show_reactions"]:
            t.append("\n  " + ", ".join(m.reactions), style="dim")
        return t

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
                    ol.add_option(Option(Text(f"── {item.label} ──", style="dim", justify="center"), disabled=True))
                prev = None
            elif isinstance(item, Notice):
                ol.add_option(Option(Text(item.text, style="dim italic", justify="center"), disabled=True))
                prev = None
            else:
                header = prev is None or prev.sender != item.sender or prev.time != item.time
                ol.add_option(Option(self._msg_text(item, header), id=item.key))
                prev = item
        if ol.option_count:
            idx = ol.option_count - 1
            if keep and not at_end:
                try:
                    idx = ol.get_option_index(keep)
                except Exception:  # noqa: BLE001 - message vanished
                    pass
            ol.highlighted = idx
        act = []
        if conv and conv.typing:
            act.append(conv.activity or "typing…")
        elif conv and conv.activity:
            act.append(conv.activity)
        if conv and conv.seen_by:
            act.append("seen: " + ", ".join(conv.seen_by))
        self.query_one("#activity", Static).update(" · ".join(act))

    def selected_message(self) -> Message | None:
        ol = self.query_one("#conv", OptionList)
        opt = ol.highlighted_option
        if not opt or not self.conv:
            return None
        return next((m for m in self.conv.items if isinstance(m, Message) and m.key == opt.id), None)

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

        return self.run_worker(run(), group=group)

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
            self.status("Loading older messages...")
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

    @work(exclusive=True, group="open")
    async def open_chat(self, chat: Chat) -> None:
        if not self.bridge:
            return
        self.open_id = chat.id
        self.conv = None
        self.render_conv()
        self.query_one("#conv-title", Static).update(chat.name + ("  [group]" if chat.group else ""))
        self.status(f"Opening {chat.name}...")
        try:
            await self.bridge.open_chat(chat.id)
        except Exception as e:  # noqa: BLE001 - shown in the status bar
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
            if any(w in chat.status.lower() for w in ("snap",)):
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
        box.placeholder = (
            "reply  (enter send · esc cancel)" if reply_key else "message  (enter send · esc cancel · /send <path>)"
        )
        box.add_class("visible")
        box.focus()

    def action_reply(self) -> None:
        m = self.selected_message()
        if not m:
            self.notify("Select a message in the chat to reply to.")
            return
        self.action_compose(reply_key=m.key)
        preview = (m.text or "media")[:50]
        self.status(f"Replying to {m.sender}: {preview}")

    def end_compose(self, clear_page: bool = False) -> None:
        box = self.query_one("#compose", Input)
        box.value = ""
        box.remove_class("visible")
        self.reply_key = None
        self.query_one("#conv").focus()
        if clear_page and self.cfg["behavior"]["send_typing"] and self.bridge:
            self.page_action(self.bridge.set_draft, "", group="draft")
        self.ready_status()

    @on(Input.Changed, "#compose")
    def draft_changed(self, event: Input.Changed) -> None:
        if not self.cfg["behavior"]["send_typing"] or event.value.startswith("/"):
            return
        if self._draft_task:
            self._draft_task.cancel()

        async def later(text: str) -> None:
            await asyncio.sleep(0.6)  # debounce: mirror after a pause in typing
            if self.bridge:
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
            self.notify("Calls and live Lenses are not supported in the terminal.", severity="warning")
            return
        key = self.reply_key
        self.end_compose()
        if key:
            self.page_action(self.bridge.reply, key, text, done="Reply sent")
        else:
            self.page_action(self.bridge.send_text, text)

    # --- message menu ---

    @work(group="menu")
    async def action_menu(self) -> None:
        m = self.selected_message()
        if not m or not self.bridge:
            self.notify("Select a message first.")
            return
        try:
            entries = await self.bridge.open_menu(m.key)
        except Exception as e:  # noqa: BLE001 - shown in the status bar
            self.fail(e)
            return
        if "Unsave in Chat" in entries:
            self.saved[m.key] = True
        elif "Save in Chat" in entries:
            self.saved[m.key] = False
        self.render_conv()
        options = [(e, ("react " + e.split(":", 1)[1]) if e.startswith("react:") else e) for e in entries]
        state = "saved" if self.saved.get(m.key) else "not saved"
        choice = await self.push_screen_wait(MenuScreen(f"{m.sender}: {(m.text or 'media')[:40]}  ({state})", options))
        if choice is None:
            await self.bridge.close_menu()
            return
        if choice == "Copy Text":
            await self.bridge.close_menu()
            self.copy_to_clipboard(m.text)
            self.notify("Copied (via your terminal's clipboard support)")
            return
        if choice == "Reply":
            await self.bridge.close_menu()
            self.action_reply()
            return
        if choice == "Delete":
            await self.bridge.close_menu()
            if not await self.push_screen_wait(ConfirmScreen("Delete this message for everyone?")):
                return
            try:
                await self.bridge.open_menu(m.key)
                await self.bridge.choose_menu("Delete")
                await asyncio.sleep(0.8)
                await self.bridge.choose_menu_confirm("Delete")
            except Exception as e:  # noqa: BLE001 - shown in the status bar
                self.fail(e)
            return
        try:
            await self.bridge.choose_menu(choice)
        except Exception as e:  # noqa: BLE001 - shown in the status bar
            self.fail(e)
            return
        if choice in ("Save in Chat", "Unsave in Chat"):
            self.saved[m.key] = choice == "Save in Chat"
            self.render_conv()
        self.notify(options[[o[0] for o in options].index(choice)][1])

    # --- media and snaps ---

    def _dump_dir(self) -> Path | None:
        if self.cfg["debug"]["dump_viewer"] and not self._dumped_viewer:
            self._dumped_viewer = True
            return DEBUG_DIR
        return None

    @work(group="media")
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
                        f"Open the snap from {m.sender}?\n\n"
                        "Opening marks it as viewed; it can't be opened again."))
                    if not ok:
                        return
                self.status("Opening snap...")
                item = await self.bridge.open_snap(m.key, self._dump_dir())
                await self.push_screen_wait(MediaScreen(self, [item], f"Snap from {m.sender}", viewer=True))
            elif m.media:
                self.status("Loading media...")
                items = await self.bridge.message_media(m.key)
                if not items:
                    self.notify("No media found in that message.")
                    return
                await self.push_screen_wait(MediaScreen(self, items, f"{m.sender}", viewer=False))
            elif m.snap_status:
                self.notify(f"This snap was already {m.snap_status.lower()}; snaps can only be viewed once.")
            else:
                self.notify("Nothing to open in this message.")
        except Exception as e:  # noqa: BLE001 - shown in the status bar
            self.fail(e)
            return
        self.ready_status()

    @work(group="media")
    async def action_stories(self) -> None:
        if not self.bridge:
            return
        if self.open_id:
            self.open_id = None
            await self.bridge.close_chat()
            self.query_one("#chats").focus()
        try:
            label = await self.bridge.stories_available()
            if "no stories" in label.lower():
                self.notify("No stories to view right now.")
                return
            self.status("Opening stories...")
            item = await self.bridge.open_stories(self._dump_dir())
            await self.push_screen_wait(MediaScreen(self, [item], "Stories", viewer=True))
        except Exception as e:  # noqa: BLE001 - shown in the status bar
            self.fail(e)
            return
        self.ready_status()

    @work(group="media")
    async def action_send_file(self) -> None:
        if not self._need_chat():
            return
        path = await self.push_screen_wait(FilePickerScreen())
        if path:
            self.confirm_send_file(path)

    @work(group="media")
    async def confirm_send_file(self, path: Path) -> None:
        if not self._need_chat():
            return
        if not path.is_file():
            self.notify(f"No such file: {path}", severity="error")
            return
        name = self.chats[self.open_id].name if self.open_id in self.chats else "this chat"
        if not await self.push_screen_wait(ConfirmScreen(f"Send {path.name} to {name}?")):
            return
        self.status(f"Sending {path.name}...")
        try:
            result = await self.bridge.send_file(path)
        except Exception as e:  # noqa: BLE001 - shown in the status bar
            self.fail(e)
            return
        self.notify(f"{path.name}: {result}")
        self.ready_status()


def run_app() -> int:
    return GhostctlApp().run() or 0

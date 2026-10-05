"""Owns Playwright: one persistent Chromium profile, headless with headed fallback."""

from __future__ import annotations

import asyncio
import enum
import subprocess
from dataclasses import dataclass
from pathlib import Path

from playwright.async_api import (
    BrowserContext,
    Error as PlaywrightError,
    Page,
    Playwright,
    async_playwright,
)

from . import selectors as S

HOME = Path.home() / ".ghostctl"
PROFILE_DIR = HOME / "profile"

# Off-screen and small so the fallback window stays out of the way. Forced onto
# X11 (XWayland): Wayland compositors ignore requested window positions.
OFFSCREEN_ARGS = ["--ozone-platform=x11", "--window-position=-32000,-32000", "--window-size=1280,900"]


class Session(enum.Enum):
    LOGGED_IN = "logged in"
    LOGGED_OUT = "logged out"
    BLOCKED = "blocked"
    UNKNOWN = "unknown"


class Mode(enum.Enum):
    HEADLESS = "headless"
    HEADED_OFFSCREEN = "headed (off-screen)"
    HEADED = "headed"


class ProfileLocked(RuntimeError):
    pass


@dataclass
class Status:
    mode: Mode
    session: Session
    note: str = ""


class Browser:
    """Async wrapper around a persistent Chromium context."""

    def __init__(self, profile_dir: Path = PROFILE_DIR, headless: str = "auto", user_agent: str = "") -> None:
        self.profile_dir = profile_dir
        self.headless_pref = headless  # "auto" | "always" | "never"
        self.user_agent = user_agent
        self._pw: Playwright | None = None
        self.context: BrowserContext | None = None
        self.page: Page | None = None
        self.mode: Mode | None = None
        self.closed = True  # True until a context is launched, and again once it dies

    async def _launch(self, mode: Mode) -> Page:
        self.profile_dir.mkdir(parents=True, exist_ok=True)
        if self._pw is None:
            self._pw = await async_playwright().start()
        args = OFFSCREEN_ARGS if mode is Mode.HEADED_OFFSCREEN else []
        if not self.user_agent:
            self.user_agent = self._default_user_agent()
        try:
            self.context = await self._pw.chromium.launch_persistent_context(
                str(self.profile_dir),
                # Full Chromium in new-headless mode, not the stripped headless shell.
                channel="chromium",
                headless=mode is Mode.HEADLESS,
                args=args,
                # Snapchat refuses "HeadlessChrome" in the user agent; send the plain one.
                user_agent=self.user_agent,
                # Pin the UI language: selectors match English labels and visible text.
                locale="en-US",
                extra_http_headers={"Accept-Language": "en-US,en;q=0.9"},
                viewport=None if mode is Mode.HEADED else {"width": 1280, "height": 900},
            )
        except PlaywrightError as e:
            msg = str(e)
            if any(m in msg for m in ("ProcessSingleton", "SingletonLock", "existing browser session")):
                raise ProfileLocked(
                    f"Profile {self.profile_dir} is in use by another ghostctl "
                    "or Chromium process. Close it and retry."
                ) from e
            raise
        self.mode = mode
        self.closed = False
        self.context.on("close", lambda _: setattr(self, "closed", True))
        pages = self.context.pages
        self.page = pages[0] if pages else await self.context.new_page()
        return self.page

    def _default_user_agent(self) -> str:
        """The user agent of the installed Chromium, as a normal (headed) Chrome."""
        major = "153"
        try:
            out = subprocess.run(
                [self._pw.chromium.executable_path, "--version"],
                capture_output=True, text=True, timeout=10,
            ).stdout
            major = out.split()[-1].split(".")[0] or major
        except (OSError, IndexError, subprocess.SubprocessError):
            pass
        return (
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
            f"Chrome/{major}.0.0.0 Safari/537.36"
        )

    async def close(self) -> None:
        if self.context is not None:
            try:
                await self.context.close()
            except PlaywrightError:
                pass
            self.context = None
        if self._pw is not None:
            await self._pw.stop()
            self._pw = None

    async def goto_web(self) -> None:
        assert self.page is not None
        await self.page.goto(S.WEB_URL, wait_until="domcontentloaded")

    async def _probe_page(self, page: Page) -> Session:
        try:
            if await page.locator(S.BLOCKED_TEXT.css).first.is_visible():
                return Session.BLOCKED
            if await page.locator(S.LOGGED_IN_MARKER.css).first.is_visible():
                return Session.LOGGED_IN
            if await page.locator(S.LOGIN_HEADING.css).first.is_visible():
                return Session.LOGGED_OUT
            if await page.locator(S.LOGIN_FORM.css).first.is_visible():
                return Session.LOGGED_OUT
        except PlaywrightError:
            pass  # mid-navigation or page closed
        return Session.UNKNOWN

    async def _probe(self) -> Session:
        """Check every tab; login may finish in a popup or a tab other than the first."""
        if self.closed or self.context is None:
            return Session.UNKNOWN
        states = []
        for page in list(self.context.pages):
            state = await self._probe_page(page)
            if state is Session.LOGGED_IN:
                self.page = page
                return state
            states.append(state)
        for preferred in (Session.BLOCKED, Session.LOGGED_OUT):
            if preferred in states:
                return preferred
        return Session.UNKNOWN

    async def session_state(self, timeout: float = 20.0) -> Session:
        """Wait until the page settles into the same recognisable state twice in a row."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        last = Session.UNKNOWN
        while loop.time() < deadline and not self.closed:
            state = await self._probe()
            if state is not Session.UNKNOWN and state is last:
                return state
            last = state
            await asyncio.sleep(1.0)
        return Session.UNKNOWN

    async def start(self) -> Status:
        """Normal run: headless, falling back to an off-screen window if refused."""
        state = Session.UNKNOWN
        if self.headless_pref != "never":
            await self._launch(Mode.HEADLESS)
            await self.goto_web()
            state = await self.session_state()
            if state is Session.LOGGED_IN or self.headless_pref == "always":
                note = "" if state is Session.LOGGED_IN else self._note(state)
                return Status(Mode.HEADLESS, state, note)
            await self.close()

        await self._launch(Mode.HEADED_OFFSCREEN)
        await self.goto_web()
        state2 = await self.session_state()
        note = self._note(state2)
        if not note and self.headless_pref != "never":
            note = f"Headless was {state.value}; using a hidden window."
        return Status(Mode.HEADED_OFFSCREEN, state2, note)

    @staticmethod
    def _note(state: Session) -> str:
        if state is Session.LOGGED_OUT:
            return "Not logged in. Quit and run `ghostctl login`."
        if state is Session.LOGGED_IN:
            return ""
        return (
            f"Could not confirm login ({state.value}). Selector "
            f"'{S.LOGGED_IN_MARKER.name}' may be stale; run `ghostctl inspect`."
        )

    async def open_headed(self) -> Page:
        """Visible window for login and inspect."""
        page = await self._launch(Mode.HEADED)
        await self.goto_web()
        return page

    async def reveal(self) -> None:
        """Move the (off-screen) window on-screen so the user can interact with it."""
        assert self.context is not None and self.page is not None
        cdp = await self.context.new_cdp_session(self.page)
        win = await cdp.send("Browser.getWindowForTarget")
        await cdp.send(
            "Browser.setWindowBounds",
            {"windowId": win["windowId"], "bounds": {"left": 80, "top": 80, "width": 1280, "height": 900}},
        )
        await self.page.bring_to_front()

    async def open_offscreen(self) -> Page:
        page = await self._launch(Mode.HEADED_OFFSCREEN)
        await self.goto_web()
        return page

    async def wait_closed(self) -> None:
        """Block until the user closes every window of the visible browser."""
        while not self.closed and self.context is not None and self.context.pages:
            await asyncio.sleep(0.5)


def from_config(cfg=None) -> Browser:
    from . import config

    cfg = cfg or config.load()
    b = cfg["browser"]
    return Browser(cfg.profile_dir, b["headless"], b["user_agent"])

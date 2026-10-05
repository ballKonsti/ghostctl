"""Log in by relaying Snapchat's login screens to a UI (terminal prompts or the TUI).

The browser runs headless. Each screen (username, password, 2FA code...) is
recognised and turned into a question for the UI. Saved credentials (creds.py)
answer the username/password questions automatically, once. The password is
typed into Snapchat's own form; ghostctl only keeps it if you choose "remember".
Screens that need a human (an interactive captcha, an unknown verification
step) are handed over to a visible browser window.
"""

from __future__ import annotations

import asyncio
import getpass
from typing import Protocol
from urllib.parse import urlparse

from playwright.async_api import Error as PlaywrightError, Locator, Page

from . import selectors as S
from .browser import Browser, Mode, Session
from .creds import Creds

STUCK_AFTER = 45.0  # seconds on an unrecognised screen before offering the window
TYPE_DELAY_MS = 70  # per-keystroke delay, human-ish


class LoginUI(Protocol):
    async def ask(self, prompt: str, secret: bool = False) -> str | None: ...
    def say(self, text: str) -> None: ...
    async def confirm(self, question: str) -> bool: ...


class TerminalUI:
    """Plain prompts for `ghostctl login`."""

    async def ask(self, prompt: str, secret: bool = False) -> str | None:
        fn = getpass.getpass if secret else input
        try:
            return (await asyncio.to_thread(fn, f"{prompt}: ")).strip()
        except EOFError:
            return None

    def say(self, text: str) -> None:
        print(f"  snapchat: {text}")

    async def confirm(self, question: str) -> bool:
        a = await self.ask(f"{question} [Y/n]")
        return a is not None and not a.lower().startswith("n")


async def _visible(loc: Locator) -> bool:
    try:
        return await loc.first.is_visible()
    except PlaywrightError:
        return False


async def _texts(loc: Locator) -> list[str]:
    try:
        return [t.strip() for t in await loc.all_inner_texts() if t.strip()]
    except PlaywrightError:
        return []


async def _input_label(loc: Locator) -> str:
    return await loc.evaluate(
        "el => (el.labels && el.labels[0] && el.labels[0].innerText) || "
        "el.getAttribute('aria-label') || el.placeholder || el.name || 'Code'"
    )


async def _type(loc: Locator, value: str) -> None:
    await loc.fill("")
    await loc.press_sequentially(value, delay=TYPE_DELAY_MS)


async def _submit(page: Page, field: Locator) -> None:
    btn = page.locator(S.ACC_SUBMIT.css).first
    if await _visible(btn) and await btn.is_enabled():
        await btn.click()
    else:
        await field.press("Enter")


async def _signature(page: Page) -> tuple:
    return (
        page.url,
        tuple(await _texts(page.locator(S.ACC_HEADING.css))),
        tuple(await _texts(page.locator(S.ACC_MESSAGE.css))),
        await _visible(page.locator(S.ACC_PASSWORD.css)),
    )


async def _wait_change(b: Browser, before: tuple, timeout: float = 20.0) -> None:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while loop.time() < deadline and not b.closed:
        await asyncio.sleep(1.0)
        if await b._probe() is Session.LOGGED_IN:
            return
        try:
            if await _signature(b.page) != before:
                return
        except PlaywrightError:
            pass


async def _hand_over(b: Browser, ui: LoginUI, reason: str) -> bool:
    """Finish in a visible window, then go back to headless."""
    if not await ui.confirm(f"{reason} Open a browser window to finish there?"):
        return False
    headless = b.mode is Mode.HEADLESS
    await b.close()
    await b.open_headed()
    ui.say("Finish logging in in the window; it closes by itself afterwards.")
    ok = False
    while not b.closed:
        if await b.session_state(timeout=5) is Session.LOGGED_IN:
            ok = True
            await asyncio.sleep(3)  # let the session save
            break
    await b.close()
    if headless:
        await b.start()
    return ok


async def run_login(b: Browser, ui: LoginUI, creds: Creds | None = None) -> Creds | None:
    """Drive the login. The browser must already be on Snapchat Web.
    Returns the username/password that worked (for "remember me"), or None."""
    loop = asyncio.get_running_loop()
    used_saved_user = used_saved_pw = False
    username = password = ""
    shown: tuple = ()
    last_change = loop.time()
    last_sig: tuple = ()

    while not b.closed:
        state = await b._probe()
        if state is Session.LOGGED_IN:
            return Creds(username, password) if username and password else Creds(username or "?", "")
        if state is Session.BLOCKED:
            ui.say('Snapchat refused this browser ("Browser not supported").')
            return None
        page = b.page
        if await _visible(page.locator(S.COOKIE_ESSENTIAL.css)):
            await page.locator(S.COOKIE_ESSENTIAL.css).first.click()
            await asyncio.sleep(1)
            continue

        sig = await _signature(page)
        if sig != last_sig:
            last_sig, last_change = sig, loop.time()
        on_accounts = urlparse(page.url).hostname == S.ACCOUNTS_HOST

        # Pass on what Snapchat says (errors, instructions) once per screen.
        if on_accounts and sig[1:3] != shown:
            shown = sig[1:3]
            for line in (*sig[1], *sig[2]):
                if line != username:
                    ui.say(line)

        if await _visible(page.locator(S.ACC_VERIFYING.css)):
            if loop.time() - last_change > STUCK_AFTER:
                ok = await _hand_over(b, ui, "The security check needs a human (captcha).")
                return Creds(username, password) if ok else None
            await asyncio.sleep(1)
            continue

        landing = page.locator(S.LOGIN_FORM.css).first
        acc_user = page.locator(S.ACC_USERNAME.css).first
        pw_box = page.locator(S.ACC_PASSWORD.css).first
        other = page.locator(S.ACC_OTHER_INPUT.css).first

        async def get_username() -> str | None:
            # With given credentials (login form or keyring) a second username
            # prompt means they were rejected: give up so the caller can show
            # Snapchat's message on its own form.
            nonlocal used_saved_user
            if creds:
                if used_saved_user:
                    return None
                used_saved_user = True
                return creds.username
            return await ui.ask("Username or email")

        if await _visible(landing) or (on_accounts and await _visible(acc_user) and not await _visible(pw_box)):
            field = landing if await _visible(landing) else acc_user
            u = await get_username()
            if not u:
                return None
            username = u
            await _type(field, username)
            if field is landing:
                await page.locator(S.LOGIN_SUBMIT.css).first.click()
            else:
                await _submit(page, field)
            await _wait_change(b, sig)
        elif on_accounts and await _visible(pw_box):
            if creds:
                if used_saved_pw:
                    return None  # wrong password: back to the caller's form
                used_saved_pw = True
                pw = creds.password
            else:
                pw = await ui.ask("Password", secret=True)
            if not pw:
                return None
            password = pw
            await _type(pw_box, pw)
            await _submit(page, pw_box)
            await _wait_change(b, sig)
        elif on_accounts and await _visible(other):
            label = (await _input_label(other)).strip() or "Code"
            value = await ui.ask(label)
            if not value:
                return None
            await _type(other, value)
            await _submit(page, other)
            await _wait_change(b, sig)
        else:
            if loop.time() - last_change > STUCK_AFTER:
                ok = await _hand_over(b, ui, "ghostctl doesn't recognise this login screen.")
                return Creds(username, password) if ok else None
            await asyncio.sleep(1)
    return None

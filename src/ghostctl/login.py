"""`ghostctl login`: log in from the terminal by relaying Snapchat's login screens.

The browser runs off-screen. Each screen (username, password, 2FA code...) is
recognised and turned into a terminal prompt. The password is read with getpass,
typed into the page and dropped; it is never stored or logged. Screens ghostctl
can't handle (an interactive captcha, an unknown verification step) are handed
over by moving the browser window on-screen.
"""

from __future__ import annotations

import asyncio
import getpass
from urllib.parse import urlparse

from playwright.async_api import Error as PlaywrightError, Locator, Page

from . import selectors as S
from .browser import Browser, Session

STUCK_AFTER = 45.0  # seconds on an unrecognised screen before offering the window
TYPE_DELAY_MS = 70  # per-keystroke delay, human-ish


async def _ask(prompt: str, secret: bool = False) -> str:
    fn = getpass.getpass if secret else input
    return (await asyncio.to_thread(fn, prompt)).strip()


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


async def _hand_over(b: Browser, reason: str) -> bool:
    print(f"\n{reason}")
    answer = await _ask("Show the browser window so you can finish there? [Y/n] ")
    if answer.lower().startswith("n"):
        return False
    await b.reveal()
    print("Finish logging in in the window. Waiting...")
    while not b.closed:
        if await b.session_state(timeout=5) is Session.LOGGED_IN:
            return True
    return False


async def terminal_login(b: Browser) -> bool:
    page = await b.open_offscreen()
    print("Connecting to Snapchat Web (browser runs off-screen)...")
    loop = asyncio.get_running_loop()
    username: str | None = None
    shown: tuple = ()
    last_change = loop.time()
    last_sig: tuple = ()

    while not b.closed:
        state = await b._probe()
        if state is Session.LOGGED_IN:
            return True
        if state is Session.BLOCKED:
            print("Snapchat refused this browser (\"Browser not supported\").")
            return False
        page = b.page
        if await _visible(page.locator(S.COOKIE_ESSENTIAL.css)):
            await page.locator(S.COOKIE_ESSENTIAL.css).first.click()
            await asyncio.sleep(1)
            continue

        sig = await _signature(page)
        if sig != last_sig:
            last_sig, last_change = sig, loop.time()
        on_accounts = urlparse(page.url).hostname == S.ACCOUNTS_HOST

        # Show what Snapchat says (errors, instructions) once per screen.
        if on_accounts and sig[1:3] != shown:
            shown = sig[1:3]
            for line in (*sig[1], *sig[2]):
                print(f"  snapchat: {line}")

        if await _visible(page.locator(S.ACC_VERIFYING.css)):
            if loop.time() - last_change > STUCK_AFTER:
                return await _hand_over(b, "The security check needs a human (captcha).")
            await asyncio.sleep(1)
            continue

        landing = page.locator(S.LOGIN_FORM.css).first
        acc_user = page.locator(S.ACC_USERNAME.css).first
        password = page.locator(S.ACC_PASSWORD.css).first
        other = page.locator(S.ACC_OTHER_INPUT.css).first

        if await _visible(landing):
            username = username or await _ask("Username or email: ")
            await _type(landing, username)
            await page.locator(S.LOGIN_SUBMIT.css).first.click()
            await _wait_change(b, sig)
        elif on_accounts and await _visible(password):
            secret = await _ask("Password (not stored): ", secret=True)
            await _type(password, secret)
            del secret
            await _submit(page, password)
            await _wait_change(b, sig)
        elif on_accounts and await _visible(acc_user):
            # Back on the username step: usually an error was shown above.
            username = await _ask("Username or email: ")
            await _type(acc_user, username)
            await _submit(page, acc_user)
            await _wait_change(b, sig)
        elif on_accounts and await _visible(other):
            label = (await _input_label(other)).strip() or "Code"
            value = await _ask(f"{label}: ")
            await _type(other, value)
            await _submit(page, other)
            await _wait_change(b, sig)
        else:
            if loop.time() - last_change > STUCK_AFTER:
                return await _hand_over(b, "ghostctl doesn't recognise this login screen.")
            await asyncio.sleep(1)
    return False

"""Turns the Snapchat Web page into events (chat list, conversation) and actions.

A script injected into the page reads the chat list and the open conversation
into plain JSON. A MutationObserver re-reads whichever region changed and pushes
it to Python through `page.expose_function`. If injection fails, the bridge
falls back to polling the same read functions every few seconds.

Actions on a specific message find it again by key in the page, tag it with
`data-ghostctl-target`, and then use Playwright on that element.
"""

from __future__ import annotations

import asyncio
import base64
import json
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Awaitable, Callable

from playwright.async_api import Error as PlaywrightError, Page

from . import selectors as S

POLL_INTERVAL = 4.0  # seconds, fallback mode only
TARGET = "[data-ghostctl-target]"
VIEWER = "[data-ghostctl-viewer]"


class SelectorError(RuntimeError):
    """A selector matched nothing where the page should have it."""

    def __init__(self, sel: S.Sel | str, context: str = "") -> None:
        name, css = (sel.name, sel.css) if isinstance(sel, S.Sel) else (sel, sel)
        self.sel = sel
        msg = f"Selector '{name}' ({css}) matched nothing"
        if context:
            msg += f" while {context}"
        super().__init__(msg + ". Snapchat may have changed its layout; run `ghostctl inspect`.")


class ActionError(RuntimeError):
    """An action that can't be done (not a broken selector)."""


@dataclass
class Chat:
    id: str
    name: str
    status: str = ""
    time: datetime | None = None
    extras: list[str] = field(default_factory=list)  # e.g. "941 🔥"
    badge: str = ""  # friend emoji shown on the avatar
    group: bool = False

    @property
    def unread(self) -> bool:
        return self.status.lower().startswith("new")

    @property
    def streak(self) -> str:
        return next((e for e in self.extras if "🔥" in e), "")


@dataclass
class Message:
    key: str
    sender: str
    time: datetime | None
    text: str = ""
    quote_sender: str = ""
    quote_text: str = ""
    media: list[str] = field(default_factory=list)  # "image", "video"
    snap_status: str = ""  # e.g. "Opened", "New Snap"
    snap_new: bool = False  # unopened snap ("Click to view")
    reactions: list[str] = field(default_factory=list)  # "love · Konstantin"
    not_supported: bool = False  # "Not Supported on Web"

    @property
    def mine(self) -> bool:
        return self.sender == "Me"


@dataclass
class DateMark:
    label: str


@dataclass
class Notice:
    text: str


ConvItem = Message | DateMark | Notice


@dataclass
class Conversation:
    id: str
    items: list[ConvItem]
    seen_by: list[str] = field(default_factory=list)
    activity: str = ""  # typing / presence text, if any
    typing: bool = False


@dataclass
class Media:
    kind: str  # "image" | "video"
    data: bytes
    mime: str = ""

    @property
    def suffix(self) -> str:
        sub = self.mime.split("/")[-1].split(";")[0] if self.mime else ""
        return "." + (sub or ("mp4" if self.kind == "video" else "png"))


_PAGE_JS = r"""
(SEL) => {
  if (window.__ghostctl) return window.__ghostctl.ok;
  // innerText keeps line breaks but applies CSS text-transform (sender names are
  // uppercased); use it for message text only and textContent for labels.
  const txt = (e) => (e ? (e.innerText || e.textContent || '').trim() : '');
  const raw = (e) => (e ? (e.textContent || '').replace(/\s+/g, ' ').trim() : '');

  function readFeed() {
    const feed = document.querySelector(SEL.feed);
    if (!feed) return null;
    const rows = [];
    for (const row of feed.querySelectorAll(SEL.feed_row)) {
      const title = row.querySelector(SEL.feed_row_title);
      if (!title) continue;
      const id = title.id.slice('title-'.length);
      const status = row.querySelector(SEL.feed_row_status);
      const time = row.querySelector(SEL.feed_row_time);
      // Extras: text after the time ("941 🔥"); badge: text on the avatar (friend emoji).
      const known = [title, status, time].filter(Boolean);
      const extras = [], badges = [];
      const seen = new Set();
      const walker = document.createTreeWalker(row, NodeFilter.SHOW_TEXT);
      for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const el = n.parentElement;
        const t = raw(el);
        if (!t || t === '·' || t === 'View' || seen.has(el) || known.some(k => k.contains(n))) continue;
        seen.add(el);  // one entry per element, so "941" + "🔥" stay together
        const beforeTitle = title.compareDocumentPosition(n) & Node.DOCUMENT_POSITION_PRECEDING;
        (beforeTitle ? badges : extras).push(t);
      }
      rows.push({
        id, name: raw(title), status: raw(status),
        time: time ? time.getAttribute('datetime') : null,
        extras, badge: badges.join(' '),
        group: row.querySelectorAll(SEL.feed_row_avatar).length > 1,
        top: row.getBoundingClientRect().top,
      });
    }
    rows.sort((a, b) => a.top - b.top);
    return rows;
  }

  function readMessage(li, ctx) {
    const header = li.querySelector(SEL.msg_header);
    if (header) {
      const t = header.querySelector('time[datetime]');
      ctx.sender = raw(header).replace(t ? raw(t) : '', '').trim();
      ctx.time = t ? t.getAttribute('datetime') : null;
      ctx.idx = 0;
    }
    const body = [...li.children].find(c => c.tagName === 'DIV');
    const reactions = li.querySelector(SEL.msg_reactions);
    const m = {kind: 'msg', sender: ctx.sender, time: ctx.time, text: '', quote_sender: '',
               quote_text: '', media: [], snap_status: '', snap_new: false, reactions: [],
               not_supported: false};
    if (reactions) {
      const imgs = [...reactions.querySelectorAll(SEL.msg_reaction_img)];
      m.reactions = imgs.length
        ? imgs.map(i => i.alt.replace(/^Reaction /, '').replace(/ from /, ' · '))
        : (raw(reactions) ? [raw(reactions)] : []);
    }
    if (body) {
      const qul = body.querySelector(SEL.msg_quote_list);
      const quote = qul ? qul.parentElement : null;
      if (quote) {
        m.quote_text = txt(qul);
        m.quote_sender = raw(quote).replace(raw(qul), '').trim();
      }
      m.text = [...body.querySelectorAll(SEL.msg_text)]
        .filter(s => !(quote && quote.contains(s))).map(txt).join('\n');
      for (const v of body.querySelectorAll('video')) m.media.push('video');
      for (const i of body.querySelectorAll('img')) if (!(quote && quote.contains(i))) m.media.push('image');
      const all = raw(body);
      if (all.includes(SEL.__not_supported)) { m.not_supported = true; m.text = ''; }
      else if (!m.text && !m.media.length) {
        m.snap_new = all.includes('Click to view');
        m.snap_status = all.replace('Click to view', '').trim();
      }
    }
    m.key = `${ctx.time}|${ctx.sender}|${ctx.idx++}`;
    return [m, li];
  }

  // Walk the open conversation. Returns [items, Map(key -> li element)].
  function walk() {
    const ul = document.querySelector(SEL.conv_list);
    if (!ul) return null;
    const items = [], els = new Map();
    for (const li of ul.querySelectorAll(SEL.conv_item)) {
      const msgs = li.querySelectorAll(SEL.msg_item);
      if (msgs.length) {
        const ctx = {sender: '', time: null, idx: 0};
        for (const el of msgs) { const [m, e] = readMessage(el, ctx); items.push(m); els.set(m.key, e); }
        continue;
      }
      const t = li.querySelector('time');
      if (t && !raw(li).replace(raw(t), '').trim()) { items.push({kind: 'date', label: raw(t)}); continue; }
      const n = txt(li);
      if (n) items.push({kind: 'notice', text: n});
    }
    return [ul, items, els];
  }

  function readActivity(ul) {
    const region = ul.parentElement.querySelector(':scope > ul[id^="cv-"] + div') || ul.nextElementSibling;
    if (!region) return {seen_by: [], activity: '', typing: false};
    const chips = [...region.querySelectorAll('[role=link]')].map(raw).filter(Boolean);
    let rest = raw(region);
    for (const c of chips) rest = rest.replace(c, '');
    rest = rest.trim();
    return {seen_by: chips, activity: rest, typing: /typing/i.test(rest)};
  }

  function readConversation() {
    const w = walk();
    if (!w) return null;
    const [ul, items] = w;
    return {id: ul.id.slice('cv-'.length), items, ...readActivity(ul)};
  }

  // Tag one message element so Playwright can act on it.
  function target(key) {
    document.querySelectorAll('[data-ghostctl-target]').forEach(e => e.removeAttribute('data-ghostctl-target'));
    const w = walk();
    const el = w && w[2].get(key);
    if (!el) return false;
    el.setAttribute('data-ghostctl-target', '1');
    el.scrollIntoView({block: 'center'});
    return true;
  }

  // Find the snap/story viewer: the largest visible img/video/canvas outside the
  // chat list and message list. Tags it and returns a description.
  function findViewer() {
    document.querySelectorAll('[data-ghostctl-viewer]').forEach(e => e.removeAttribute('data-ghostctl-viewer'));
    const vw = innerWidth, vh = innerHeight;
    let best = null, bestArea = 0;
    for (const el of document.querySelectorAll('img, video, canvas')) {
      if (el.closest(SEL.feed) || el.closest(SEL.conv_list)) continue;
      const r = el.getBoundingClientRect();
      if (r.width < 120 || r.height < 120 || r.bottom < 0 || r.top > vh) continue;
      const area = r.width * r.height;
      if (area > bestArea && area > 0.12 * vw * vh) { best = el; bestArea = area; }
    }
    if (!best) return null;
    best.setAttribute('data-ghostctl-viewer', '1');
    return {tag: best.tagName.toLowerCase(), src: best.currentSrc || best.src || ''};
  }

  async function fetchSrc(src) {
    const r = await fetch(src);
    const buf = new Uint8Array(await r.arrayBuffer());
    let s = '';
    for (let i = 0; i < buf.length; i += 0x8000) s += String.fromCharCode.apply(null, buf.subarray(i, i + 0x8000));
    return {mime: r.headers.get('content-type') || '', b64: btoa(s)};
  }

  // Media elements inside the tagged message (excluding quotes and reactions).
  function targetMedia() {
    const el = document.querySelector('[data-ghostctl-target]');
    if (!el) return [];
    const body = [...el.children].find(c => c.tagName === 'DIV');
    if (!body) return [];
    const q = body.querySelector(SEL.msg_quote_list);
    return [...body.querySelectorAll('img, video')]
      .filter(m => !(q && q.parentElement.contains(m)))
      .map(m => ({tag: m.tagName.toLowerCase(), src: m.currentSrc || m.src || ''}));
  }

  // Push changes: debounce per region, then hand a fresh read to Python.
  const pending = new Set();
  let timer = null;
  const flush = () => {
    timer = null;
    for (const region of pending) {
      try {
        const data = region === 'feed' ? readFeed() : readConversation();
        if (data) window.__ghostctl_emit(region, JSON.stringify(data));
      } catch (e) { window.__ghostctl_emit('error', String(e)); }
    }
    pending.clear();
  };
  const obs = new MutationObserver((records) => {
    for (const r of records) {
      const el = r.target.nodeType === 1 ? r.target : r.target.parentElement;
      if (!el) continue;
      if (el.closest(SEL.feed)) pending.add('feed');
      else if (el.closest(SEL.conv_list) || el.querySelector?.(SEL.conv_list)
               || el.closest('[id^="cv-"] + div')) pending.add('conv');
    }
    if (pending.size && !timer) timer = setTimeout(flush, 250);
  });
  obs.observe(document.body, {childList: true, subtree: true, characterData: true});

  window.__ghostctl = {ok: true, readFeed, readConversation, target, findViewer, fetchSrc, targetMedia};
  return true;
}
"""

Handler = Callable[[str, object], Awaitable[None] | None]


class Bridge:
    def __init__(self, page: Page, on_event: Handler, action_gap: float = 1.2) -> None:
        self.page = page
        self.on_event = on_event
        self.action_gap = action_gap
        self.live = False  # True when the MutationObserver is pushing events
        self._poll_task: asyncio.Task | None = None
        self._last_action = 0.0
        self._lock = asyncio.Lock()  # one page action at a time

    # --- parsing ---

    @staticmethod
    def _dt(s: str | None) -> datetime | None:
        if not s:
            return None
        try:
            return datetime.fromisoformat(s.replace("Z", "+00:00")).astimezone()
        except ValueError:
            return None

    def _parse(self, region: str, data) -> object:
        if region == "feed":
            return [
                Chat(r["id"], r["name"], r["status"], self._dt(r["time"]), r["extras"], r["badge"], r["group"])
                for r in data
            ]
        items: list[ConvItem] = []
        for it in data["items"]:
            if it["kind"] == "date":
                items.append(DateMark(it["label"]))
            elif it["kind"] == "notice":
                items.append(Notice(it["text"]))
            else:
                items.append(
                    Message(
                        it["key"], it["sender"], self._dt(it["time"]), it["text"], it["quote_sender"],
                        it["quote_text"], it["media"], it["snap_status"], it["snap_new"],
                        it["reactions"], it["not_supported"],
                    )
                )
        return Conversation(data["id"], items, data["seen_by"], data["activity"], data["typing"])

    async def _emit(self, region: str, payload: str) -> None:
        if region == "error":
            res = self.on_event("error", payload)
        else:
            res = self.on_event(region, self._parse(region, json.loads(payload)))
        if asyncio.iscoroutine(res):
            await res

    # --- lifecycle ---

    def _sel(self) -> dict:
        return {**S.js_selectors(), "__not_supported": S.NOT_SUPPORTED_ON_WEB}

    async def _inject(self) -> bool:
        return await self.page.evaluate(_PAGE_JS, self._sel())

    async def start(self) -> None:
        # Fail fast: a stuck click should surface as an error, not hang for 30s.
        self.page.set_default_timeout(8_000)
        await self.dismiss_popups()
        try:
            await self.page.wait_for_selector(S.FEED.css, timeout=20_000)
        except PlaywrightError as e:
            raise SelectorError(S.FEED, "waiting for the chat list") from e
        try:
            await self.page.expose_function("__ghostctl_emit", self._emit)
            self.live = await self._inject()
        except PlaywrightError:
            self.live = False
        if not self.live:
            self._poll_task = asyncio.create_task(self._poll())
        await self.refresh()

    async def stop(self) -> None:
        if self._poll_task:
            self._poll_task.cancel()

    async def _poll(self) -> None:
        while True:
            try:
                await self.refresh()
            except PlaywrightError:
                pass
            await asyncio.sleep(POLL_INTERVAL)

    async def _call(self, fn: str, *args):
        """Call a helper of the injected script, re-injecting if the page lost it."""
        if not await self.page.evaluate("() => !!window.__ghostctl"):
            await self.page.evaluate("() => { window.__ghostctl_emit = window.__ghostctl_emit || (() => {}); }")
            await self._inject()
        return await self.page.evaluate(f"(a) => window.__ghostctl.{fn}(...a)", list(args))

    async def refresh(self) -> None:
        """Read both regions now and emit them."""
        for region, fn in (("feed", "readFeed"), ("conv", "readConversation")):
            data = await self._call(fn)
            if data is not None:
                await self._emit(region, json.dumps(data))

    # --- pacing ---

    async def _pace(self, clear: bool = True) -> None:
        gap = time.monotonic() - self._last_action
        if gap < self.action_gap:
            await asyncio.sleep(self.action_gap - gap)
        self._last_action = time.monotonic()
        if clear:
            await self._clear_overlay()

    async def _target(self, key: str) -> None:
        if not await self._call("target", key):
            raise ActionError("That message is no longer on the page (scrolled away or deleted).")

    # --- navigation ---

    async def dismiss_popups(self) -> None:
        btn = self.page.locator(S.NOTIFY_NOT_NOW.css).first
        try:
            await btn.wait_for(state="visible", timeout=4_000)
            await btn.click()
        except PlaywrightError:
            pass

    async def open_chat(self, chat_id: str) -> None:
        """Switch chats with an in-app route change (no reload, no click on the row,
        so a "New Snap" row can't open the snap viewer)."""
        async with self._lock:
            await self._pace()
            await self.page.evaluate(
                "id => { history.pushState({}, '', '/web/' + id);"
                " dispatchEvent(new PopStateEvent('popstate', {state: {}})); }",
                chat_id,
            )
            try:
                await self.page.wait_for_selector(f"ul#cv-{chat_id}", timeout=10_000)
            except PlaywrightError as e:
                raise SelectorError(S.CONV_LIST, f"opening chat {chat_id}") from e
        await self.refresh()

    async def close_chat(self) -> None:
        async with self._lock:
            await self._pace()
            btn = self.page.locator(S.CONV_CLOSE.css).first
            if await btn.count():
                await btn.click()

    async def mark_read(self, chat_id: str) -> None:
        """Open and close the chat: Snapchat marks messages read when a chat is opened."""
        await self.open_chat(chat_id)
        await asyncio.sleep(1.5)
        await self.close_chat()

    async def load_older(self) -> None:
        """Scroll the open conversation to the top; Snapchat loads older messages."""
        async with self._lock:
            await self._pace()
            found = await self.page.evaluate(
                "sel => { const ul = document.querySelector(sel); if (!ul) return false;"
                " ul.scrollTop = 0; return true; }",
                S.CONV_LIST.css,
            )
        if not found:
            raise SelectorError(S.CONV_LIST, "loading older messages")

    async def load_more_chats(self) -> None:
        """Scroll the chat list to the bottom; Snapchat loads more chats."""
        async with self._lock:
            await self._pace()
            found = await self.page.evaluate(
                "sel => { const f = document.querySelector(sel); if (!f) return false;"
                " f.scrollTop = f.scrollHeight; return true; }",
                S.FEED.css,
            )
        if not found:
            raise SelectorError(S.FEED, "loading more chats")

    # --- composing ---

    async def _composer(self):
        box = self.page.locator(S.COMPOSER.css).first
        if not await box.count():
            raise SelectorError(S.COMPOSER, "finding the message box")
        return box

    async def _set_composer(self, text: str) -> None:
        await self._clear_overlay()
        box = await self._composer()
        await box.click()
        await self.page.keyboard.press("Control+A")
        await self.page.keyboard.press("Backspace")
        for i, line in enumerate(text.split("\n")):
            if i:
                await self.page.keyboard.press("Shift+Enter")
            if line:
                await self.page.keyboard.insert_text(line)

    async def set_draft(self, text: str) -> None:
        """Mirror the TUI draft into the composer, which makes Snapchat send "typing"."""
        async with self._lock:
            await self._set_composer(text)

    async def send_text(self, text: str) -> None:
        text = text.strip()
        if not text:
            return
        async with self._lock:
            await self._pace()
            await self._set_composer(text)
            await self.page.keyboard.press("Enter")
            box = await self._composer()
            for _ in range(20):  # wait for the composer to clear = sent
                if not (await box.inner_text()).strip():
                    return
                await asyncio.sleep(0.25)
        raise ActionError("Message may not have been sent (the message box did not clear).")

    # --- message menu (right-click) ---

    async def open_menu(self, key: str) -> list[str]:
        """Open the right-click menu on a message. Returns its entries: reaction
        names prefixed "react:", then "Save in Chat"/"Unsave in Chat", "Reply", ...
        The menu stays open until choose_menu() or close_menu()."""
        async with self._lock:
            await self._pace()
            await self._target(key)
            await self.page.locator(f"{TARGET} > div").first.click(button="right")
            await asyncio.sleep(0.6)
            reactions = await self.page.evaluate(
                "sel => [...document.querySelectorAll(sel)].filter(i => i.getBoundingClientRect().width > 0)"
                ".map(i => i.alt.replace(/^Reaction /, '').replace(/ from .*/, ''))",
                S.MENU_REACTION.css,
            )
            items = []
            for label in S.MENU_ITEMS:
                loc = self.page.get_by_text(label, exact=True)
                for i in range(await loc.count()):
                    if await loc.nth(i).is_visible():
                        items.append(label)
                        break
        if not items and not reactions:
            await self.close_menu()
            raise SelectorError("message menu", "reading the right-click menu")
        # de-duplicate reactions (one per kind)
        return [f"react:{r}" for r in dict.fromkeys(reactions)] + items

    async def choose_menu(self, entry: str) -> None:
        async with self._lock:
            await self._pace(clear=False)
            if entry.startswith("react:"):
                name = entry.split(":", 1)[1]
                loc = self.page.locator(f"img[alt^='Reaction {name} from ']")
            else:
                loc = self.page.get_by_text(entry, exact=True)
            for i in range(await loc.count()):
                if await loc.nth(i).is_visible():
                    await loc.nth(i).click()
                    if entry != "Delete":  # Delete may open a confirm dialog
                        await asyncio.sleep(0.4)
                        await self._clear_overlay()
                    return
            await self._clear_overlay()
        raise ActionError(f"Menu entry '{entry}' disappeared (the menu closed). Try again.")

    async def choose_menu_confirm(self, label: str) -> None:
        """Click a confirmation button (e.g. "Delete") if Snapchat shows a dialog."""
        async with self._lock:
            btn = self.page.get_by_role("button", name=label, exact=True)
            for i in range(await btn.count()):
                if await btn.nth(i).is_visible():
                    await btn.nth(i).click()
                    return

    async def _overlay_open(self) -> bool:
        return await self.page.evaluate(
            "sel => [...document.querySelectorAll(sel)].some(e => {"
            " const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0; })",
            S.OVERLAY.css,
        )

    async def _clear_overlay(self) -> None:
        """Close a leftover menu/pop-up: Escape doesn't, a click on its backdrop does.
        The backdrop covers the whole viewport, so the corner click lands on it."""
        for _ in range(3):
            if not await self._overlay_open():
                return
            await self.page.mouse.click(4, 4)
            await asyncio.sleep(0.3)

    async def close_menu(self) -> None:
        async with self._lock:
            await self._clear_overlay()

    async def reply(self, key: str, text: str) -> None:
        await self.open_menu(key)
        await self.choose_menu("Reply")
        await asyncio.sleep(0.4)
        await self.send_text(text)

    async def react(self, key: str, name: str) -> None:
        await self.open_menu(key)
        await self.choose_menu(f"react:{name}")

    # --- media ---

    async def _fetch(self, src: str, kind: str, shot_selector: str) -> Media:
        try:
            got = await self._call("fetchSrc", src)
            return Media(kind, base64.b64decode(got["b64"]), got["mime"])
        except PlaywrightError:
            # Cross-origin or revoked blob: fall back to a screenshot of the element.
            png = await self.page.locator(shot_selector).first.screenshot()
            return Media("image", png, "image/png")

    async def message_media(self, key: str) -> list[Media]:
        """Download the images/videos shown in a (saved) chat message."""
        async with self._lock:
            await self._target(key)
            found = await self._call("targetMedia")
            return [
                await self._fetch(m["src"], "video" if m["tag"] == "video" else "image",
                                  f"{TARGET} {m['tag']} >> nth={i}")
                for i, m in enumerate(found)
            ]

    async def _capture_viewer(self, timeout: float = 10.0) -> Media | None:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while loop.time() < deadline:
            v = await self._call("findViewer")
            if v:
                if v["tag"] == "video":
                    await asyncio.sleep(0.3)
                    return await self._fetch(v["src"], "video", VIEWER)
                if v["tag"] == "img" and v["src"]:
                    return await self._fetch(v["src"], "image", VIEWER)
                png = await self.page.locator(VIEWER).first.screenshot()
                return Media("image", png, "image/png")
            await asyncio.sleep(0.4)
        return None

    async def open_snap(self, key: str, dump_dir: Path | None = None) -> Media:
        """Open an unopened snap. This marks it as viewed for the sender."""
        async with self._lock:
            await self._pace()
            await self._target(key)
            btn = self.page.locator(f"{TARGET} >> {S.SNAP_CLICK_TO_VIEW.css}").first
            if not await btn.count():
                raise ActionError("That message isn't an unopened snap.")
            await btn.click()
            media = await self._capture_viewer()
            if dump_dir is not None:
                await self._dump(dump_dir, "snap-viewer")
        if media is None:
            raise SelectorError("snap viewer", "waiting for the snap to appear (no large image/video found)")
        return media

    async def viewer_next(self) -> Media | None:
        """Advance the snap/story viewer (click it). None when the viewer closed."""
        async with self._lock:
            await self._pace(clear=False)  # the viewer itself may be a pop-up
            loc = self.page.locator(VIEWER).first
            if not await loc.count():
                return None
            old = await self.page.evaluate("s => { const e = document.querySelector(s); return e && (e.currentSrc || e.src); }", VIEWER)
            await loc.click()
            await asyncio.sleep(1.0)
            v = await self._call("findViewer")
            if not v or v["src"] == old:
                return None
            return await self._capture_viewer(timeout=3)

    async def close_viewer(self) -> None:
        async with self._lock:
            for _ in range(3):
                if not await self._call("findViewer"):
                    return
                await self.page.keyboard.press("Escape")
                await asyncio.sleep(0.6)

    async def stories_available(self) -> str:
        """Text of the stories tile, e.g. "No Stories". Needs no chat open."""
        loc = self.page.locator(S.STORIES.css).first
        if not await loc.count():
            raise SelectorError(S.STORIES, "finding the stories tile (close the chat first)")
        return " ".join((await loc.text_content() or "").split())

    async def open_stories(self, dump_dir: Path | None = None) -> Media:
        async with self._lock:
            await self._pace()
            await self.page.locator(S.STORIES.css).first.click()
            media = await self._capture_viewer()
            if dump_dir is not None:
                await self._dump(dump_dir, "story-viewer")
        if media is None:
            raise SelectorError("story viewer", "waiting for the story to appear")
        return media

    async def send_file(self, path: Path) -> str:
        """Attach an image to the open chat. Returns what happened."""
        if path.suffix.lower() not in (".png", ".jpg", ".jpeg", ".gif"):
            raise ActionError(
                "Snapchat Web only accepts png/jpeg/gif in chat (no video). "
                "Snaps can only be sent from a webcam on the web."
            )
        async with self._lock:
            await self._pace()
            inp = self.page.locator(S.UPLOAD_INPUT.css).first
            if not await inp.count():
                raise SelectorError(S.UPLOAD_INPUT, "attaching a file")
            before = await self.page.evaluate("s => document.querySelectorAll(s).length", "ul[id^='cv-'] li")
            await inp.set_input_files(str(path))
            for _ in range(24):
                await asyncio.sleep(0.5)
                after = await self.page.evaluate("s => document.querySelectorAll(s).length", "ul[id^='cv-'] li")
                if after > before:
                    return "sent"
                send = self.page.get_by_role("button", name="Send", exact=True)
                if await send.count() and await send.first.is_visible():
                    await self._pace()
                    await send.first.click()
                    return "sent (confirmed preview)"
        return "attached; no confirmation seen — check the chat"

    async def _dump(self, dump_dir: Path, label: str) -> None:
        from .inspect import dump

        try:
            await dump(self.page, label, dump_dir)
        except PlaywrightError:
            pass

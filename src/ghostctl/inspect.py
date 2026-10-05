"""`ghostctl inspect`: dump the live page so selectors can be built from reality."""

from __future__ import annotations

import asyncio
import json
import re
import time
from pathlib import Path

from playwright.async_api import Page

from .browser import Browser, from_config

DEBUG_DIR = Path.cwd() / "debug"

# Collects every element that carries a semantic hook (role, aria-*, title,
# data-testid, placeholder, contenteditable) plus a short ancestor path, so we
# can choose selectors that do not depend on generated class names.
_COLLECT_JS = r"""
() => {
  const hooks = ['role','aria-label','aria-labelledby','aria-selected','aria-expanded',
                 'aria-live','title','data-testid','placeholder','contenteditable','alt','type','name'];
  const out = [];
  const pathOf = (el) => {
    const parts = [];
    for (let e = el; e && e.nodeType === 1 && parts.length < 6; e = e.parentElement) {
      let p = e.tagName.toLowerCase();
      const r = e.getAttribute('role'); if (r) p += `[role=${r}]`;
      const a = e.getAttribute('aria-label'); if (a) p += `[aria-label="${a.slice(0,30)}"]`;
      parts.unshift(p);
    }
    return parts.join(' > ');
  };
  for (const el of document.querySelectorAll('*')) {
    const attrs = {};
    for (const h of hooks) if (el.hasAttribute(h)) attrs[h] = el.getAttribute(h);
    if (!Object.keys(attrs).length && !['BUTTON','INPUT','TEXTAREA','VIDEO','IMG','CANVAS'].includes(el.tagName)) continue;
    const rect = el.getBoundingClientRect();
    out.push({
      tag: el.tagName.toLowerCase(),
      attrs,
      cls: (typeof el.className === 'string' ? el.className : '').slice(0, 80),
      text: (el.innerText || '').trim().replace(/\s+/g, ' ').slice(0, 80),
      visible: rect.width > 0 && rect.height > 0,
      box: [Math.round(rect.x), Math.round(rect.y), Math.round(rect.width), Math.round(rect.height)],
      path: pathOf(el),
    });
  }
  return out;
}
"""


async def dump(page: Page, label: str, base: Path = DEBUG_DIR) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    slug = re.sub(r"[^a-z0-9]+", "-", label.lower()).strip("-") or "view"
    out = base / f"{stamp}-{slug}"
    out.mkdir(parents=True, exist_ok=True)

    (out / "url.txt").write_text(page.url + "\n")
    (out / "aria.yaml").write_text(await page.locator("body").aria_snapshot())
    (out / "page.html").write_text(await page.content())
    elements = await page.evaluate(_COLLECT_JS)
    (out / "elements.json").write_text(json.dumps(elements, indent=1, ensure_ascii=False))
    await page.screenshot(path=str(out / "screenshot.png"))
    return out


async def run() -> None:
    b = from_config()
    page = await b.open_headed()
    print(
        "Inspect mode. In the browser, go to the view you want captured\n"
        "(chat list, an open chat, a snap, stories, ...), then come back here.\n"
        "  <label> + Enter  dump the current page under that label\n"
        "  q + Enter        quit\n"
        f"Dumps go to {DEBUG_DIR}/ (contains your messages; it is gitignored)."
    )
    try:
        while True:
            label = (await asyncio.to_thread(input, "label> ")).strip()
            if label.lower() in ("q", "quit", "exit"):
                break
            if not b.context or not b.context.pages:
                print("Browser window was closed.")
                break
            page = b.context.pages[-1]
            path = await dump(page, label)
            print(f"  dumped {page.url} -> {path}")
    except (EOFError, KeyboardInterrupt):
        pass
    finally:
        await b.close()

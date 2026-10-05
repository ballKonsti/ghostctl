"""Emoji search for the composer: :shortcodes:, suggestions and the picker.

Names come from the `emoji` package in English (plus GitHub/Slack aliases) and
German, so both :fire: and :feuer: work.
"""

from __future__ import annotations

import json
import re
from functools import lru_cache

import emoji

from .browser import HOME

RECENT_PATH = HOME / "recent_emoji.json"
RECENT_MAX = 24

# ":fi" at the end of the draft (start of text or after a space).
PARTIAL = re.compile(r"(?:^|\s):([^\s:]{2,})$")
# A finished ":fire:" anywhere.
CODE = re.compile(r":([^\s:]{2,}):")


def _clean(name: str) -> str:
    return name.strip(":").replace("_", " ").lower()


@lru_cache(maxsize=1)
def index() -> list[tuple[str, list[str]]]:
    """(emoji, names) for every fully-qualified emoji."""
    emoji.config.load_language("de")
    out = []
    for char, data in emoji.EMOJI_DATA.items():
        if data.get("status") != emoji.STATUS["fully_qualified"]:
            continue
        names = [_clean(data["en"])]
        names += [_clean(a) for a in data.get("alias", [])]
        if data.get("de"):
            names.append(_clean(data["de"]))
        out.append((char, list(dict.fromkeys(names))))
    return out


@lru_cache(maxsize=1)
def _by_name() -> dict[str, str]:
    table = {}
    for char, names in index():
        for n in names:
            table.setdefault(n.replace(" ", "_"), char)
            table.setdefault(n.replace(" ", ""), char)
    return table


def lookup(code: str) -> str | None:
    """Emoji for a shortcode without colons ("fire", "thumbs_up", "feuer")."""
    code = code.lower()
    return _by_name().get(code) or _by_name().get(code.replace("-", "_"))


def search(query: str, limit: int = 60) -> list[tuple[str, str]]:
    """Best matches as (emoji, name). Prefix matches first, then word starts,
    then substrings; plain emoji before skin-tone variants."""
    q = _clean(query)
    if not q:
        return [(e, _name(e)) for e in recent()]
    scored = []
    for char, names in index():
        best = None
        for n in names:
            if n == q:
                rank = 0
            elif n.startswith(q):
                rank = 1
            elif any(w.startswith(q) for w in n.split()):
                rank = 2
            elif q in n:
                rank = 3
            else:
                continue
            if best is None or rank < best[0]:
                best = (rank, n)
        if best:
            toned = "skin tone" in names[0] or "hautfarbe" in " ".join(names)
            scored.append((best[0], toned, len(best[1]), char, best[1]))
    scored.sort()
    return [(c, n) for *_, c, n in scored[:limit]]


def _name(char: str) -> str:
    data = emoji.EMOJI_DATA.get(char, {})
    return _clean(data.get("en", "")) if data else ""


def emojize(text: str) -> str:
    """Replace every known :code: in the text."""
    return CODE.sub(lambda m: lookup(m.group(1)) or m.group(0), text)


def recent() -> list[str]:
    try:
        return json.loads(RECENT_PATH.read_text())[:RECENT_MAX]
    except (OSError, ValueError):
        return ["😂", "❤️", "🔥", "👍", "😭", "🥺", "😊", "🙏", "💀", "🤣", "✨", "😍"]


def remember(char: str) -> None:
    items = [char] + [e for e in recent() if e != char]
    try:
        RECENT_PATH.parent.mkdir(parents=True, exist_ok=True)
        RECENT_PATH.write_text(json.dumps(items[:RECENT_MAX], ensure_ascii=False))
    except OSError:
        pass

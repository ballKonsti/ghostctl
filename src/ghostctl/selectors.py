"""Every DOM selector ghostctl uses, in one place.

Rules:
- Prefer accessibility roles, aria-labels and visible text. Avoid generated
  CSS class names; Snapchat rebuilds them constantly.
- Each selector carries a comment saying what it targets.
- Text and labels are English: browser.py pins the locale to en-US.
- When one breaks, run `ghostctl inspect` and fix it here.

Status: PROVISIONAL. These were written before inspecting the logged-in page
and only cover session detection. They will be rebuilt from `ghostctl inspect`
output in milestone 2.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Sel:
    """A named selector. `name` shows up in error messages."""

    name: str
    css: str  # Playwright selector string (css=, role=, text=, etc.)


# Entry point for Snapchat Web. web.snapchat.com redirects here.
WEB_URL = "https://www.snapchat.com/web"

# Logged-out visits to /web get redirected to
# https://www.snapchat.com/?original_referrer=none (verified 2026-10-05).

# Logged-out page: the "Log in to Snapchat" heading in the login sidebar.
# Only used to *detect* the logged-out state; ghostctl never types into the form.
LOGIN_HEADING = Sel("login_heading", "role=heading[name='Log in to Snapchat']")

# Logged-out page: the "Username or email address" textbox (detection only).
LOGIN_FORM = Sel("login_form", "role=textbox[name='Username or email address']")

# Page Snapchat serves to browsers it refuses, including headless Chromium:
# "Browser not supported ... Error ID: ..." (verified 2026-10-05).
BLOCKED_TEXT = Sel("blocked_text", "text=/Browser not supported/i")

# Logged-in client: the chat list, a <nav> containing list "Friends Feed"
# (verified 2026-10-05).
LOGGED_IN_MARKER = Sel("logged_in_marker", "role=list[name='Friends Feed']")

# --- Login flow on accounts.snapchat.com (verified 2026-10-05 up to the
# username step; the password and 2FA steps are matched generically). ---

# Host of the login/verification pages.
ACCOUNTS_HOST = "accounts.snapchat.com"

# Landing-page "Log in" button next to LOGIN_FORM.
LOGIN_SUBMIT = Sel("login_submit", "role=button[name='Log in']")

# Accounts page: username textbox ("Username or Email").
ACC_USERNAME = Sel("acc_username", "role=textbox[name='Username or Email']")

# Accounts page: password field.
ACC_PASSWORD = Sel("acc_password", "main input[type='password']:visible")

# Accounts page: any other visible text input (2FA / SMS / email code, etc.).
ACC_OTHER_INPUT = Sel(
    "acc_other_input",
    "main input:visible:not([type='password']):not([type='hidden'])"
    ":not([type='checkbox']):not([type='radio'])",
)

# Accounts page: page title, e.g. "Log in to Snapchat".
ACC_HEADING = Sel("acc_heading", "main h1")

# Accounts page: explanatory / error paragraphs under the title.
ACC_MESSAGE = Sel("acc_message", "main p")

# Accounts page: the primary submit button.
ACC_SUBMIT = Sel(
    "acc_submit",
    "role=button[name=/^(next|log ?in|submit|verify|continue|confirm)$/i]",
)

# Automatic bot check shown between steps ("Verifying Your Request").
ACC_VERIFYING = Sel("acc_verifying", "role=heading[name='Verifying Your Request']")

# Cookie consent banner on accounts pages: the "Essential Only" choice.
COOKIE_ESSENTIAL = Sel("cookie_essential", "role=button[name='Essential Only']")

# --- Logged-in client (verified 2026-10-05 against the live page) ---
# The page uses generated class names everywhere; these selectors only use
# roles, aria attributes, element ids with stable prefixes, and plain HTML
# structure (ul/li/header/time).

# "Enable notifications" pop-up shown on load: its "Not now" button.
NOTIFY_NOT_NOW = Sel("notify_not_now", "role=button[name='Not now']")

# Chat list container (a virtualized list; only on-screen rows exist in the DOM).
FEED = Sel("feed", "[role='list'][aria-label='Friends Feed']")

# One chat row: a role=button labelled by "title-<conversation id>" + "status-<id>".
FEED_ROW = Sel("feed_row", "[role='button'][aria-labelledby^='title-']")

# Inside a row: the chat name; its id is "title-<conversation id>".
FEED_ROW_TITLE = Sel("feed_row_title", "[id^='title-']")

# Inside a row: status text ("New Snap", "Delivered", "Opened", "Call Active"...);
# id "status-<conversation id>".
FEED_ROW_STATUS = Sel("feed_row_status", "[id^='status-']")

# Inside a row: last-activity timestamp (ISO in the datetime attribute).
FEED_ROW_TIME = Sel("feed_row_time", "time[datetime]")

# Inside a row: avatar images. More than one means a group (stacked avatars).
FEED_ROW_AVATAR = Sel("feed_row_avatar", "img[role='presentation']")

# Chat search box above the list.
FEED_SEARCH = Sel("feed_search", "role=searchbox[name='Search']")

# Open conversation: message list, id "cv-<conversation id>". It is also the
# scroll container; scrolling it to the top loads older messages.
CONV_LIST = Sel("conv_list", "ul[id^='cv-']")

# Direct children of CONV_LIST: date separators (<time> without ISO datetime),
# sender groups (contain MSG_ITEM), or notices ("YOU ARE USING SNAPCHAT FOR WEB").
CONV_ITEM = Sel("conv_item", ":scope > li")

# Inside a sender group: one message each.
MSG_ITEM = Sel("msg_item", ":scope > div > ul > li, :scope > ul > li")

# Inside a message: header with sender name and ISO time (first message of a group only).
MSG_HEADER = Sel("msg_header", ":scope > header")

# Inside a message: the reactions list (last child <ul>).
MSG_REACTIONS = Sel("msg_reactions", ":scope > ul")

# Inside a message body: a reply quote is the block holding a nested <ul>
# (quoted sender text, then the quoted message in ul > li).
MSG_QUOTE_LIST = Sel("msg_quote_list", "ul")

# Inside a message body: text runs (dir=auto spans).
MSG_TEXT = Sel("msg_text", "span[dir='auto']")

# Inside a message body: the "Reply" button shown on snap/media items.
MSG_REPLY_BUTTON = Sel("msg_reply_button", "button[title='Reply']")

# Open conversation header: close button (also used to detect an open chat).
CONV_CLOSE = Sel("conv_close", "button[title='Close Chat']")

# Composer: contenteditable textbox with placeholder "Send a chat".
COMPOSER = Sel("composer", "[role='textbox'][contenteditable='true'][placeholder='Send a chat']")

# Composer: hidden image upload input (accepts png/jpeg/gif only; no video).
UPLOAD_INPUT = Sel("upload_input", "input[type='file'][name='uploadImages']")


# --- Message context menu (right-click on a message; verified 2026-10-05) ---

# Full-viewport backdrop the menu (and other pop-ups) render into. While it has
# children it swallows every click; a real click on it closes the pop-up.
OVERLAY = Sel("overlay", "#portal-container > *")

# Reaction choices at the top of the menu: <img alt="Reaction love from <you>">.
# Names seen: love, laugh, fire, thumbs-up, thumbs-down, cry, shock, question mark.
MENU_REACTION = Sel("menu_reaction", "img[alt^='Reaction ']")

# Menu entries are plain text rows: "Save in Chat" / "Unsave in Chat", "Reply",
# "Copy Text", "Delete". Matched by exact visible text (see MENU_ITEMS).
MENU_ITEMS = ("Save in Chat", "Unsave in Chat", "Reply", "Copy Text", "Delete")

# Reactions already on a message: images inside MSG_REACTIONS, alt "Reaction <name> from <who>".
MSG_REACTION_IMG = Sel("msg_reaction_img", "img[alt^='Reaction ']")

# Unopened snap entry inside a message: the "Click to view" label (clicking it opens the snap).
SNAP_CLICK_TO_VIEW = Sel("snap_click_to_view", "text='Click to view'")

# Composer reply preview: cancel button sits before the quoted message.
# (No label; matched structurally in bridge.py.)

# Region right after CONV_LIST: name chips (group "seen by") and the typing/presence slot.
CONV_ACTIVITY = Sel("conv_activity", ":scope + div")

# Home view: "View stories" tile; its label reads "No Stories" when there are none.
STORIES = Sel("stories", "[title='View stories']")

# Home view hint confirming snaps are camera-only on the web.
CAMERA_ONLY_HINT = Sel("camera_only_hint", "text='Click the Camera to send Snaps'")

# Snapchat's notice for content the web client can't show.
NOT_SUPPORTED_ON_WEB = "Not Supported on Web"


def js_selectors() -> dict[str, str]:
    """CSS selectors handed to the injected page script (bridge.py)."""
    return {
        name.lower(): sel.css
        for name, sel in globals().items()
        if isinstance(sel, Sel) and not sel.css.startswith(("role=", "text=")) and ":visible" not in sel.css
    }

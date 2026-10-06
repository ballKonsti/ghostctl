// Every DOM selector ghostctl uses, in one place.
//
// Rules: prefer roles, aria-labels and visible text; never generated class names.
// Syntax is understood by helpers.js: "role=x[name='y']", "text='z'", CSS, ":visible",
// and "A >> B". Text and labels are English: the browser is pinned to en-US.
// When one breaks, run `ghostctl inspect` and fix it here.
#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace ghost::sel {

struct Sel {
  std::string_view name;  // shown in error messages
  std::string_view css;
};

// Entry point for Snapchat Web. Logged-out visits redirect to www.snapchat.com/.
inline constexpr std::string_view WEB_URL = "https://www.snapchat.com/web";
inline constexpr std::string_view ACCOUNTS_HOST = "accounts.snapchat.com";

// --- session detection (verified 2026-10-05) ---
// Logged-out page: the "Log in to Snapchat" heading.
inline constexpr Sel LOGIN_HEADING{"login_heading", "role=heading[name='Log in to Snapchat']"};
// Logged-out page: username textbox (detection + terminal login).
inline constexpr Sel LOGIN_FORM{"login_form", "role=textbox[name='Username or email address']"};
// Page served to browsers Snapchat refuses ("Browser not supported").
inline constexpr Sel BLOCKED_TEXT{"blocked_text", "text=/Browser not supported/i"};
// Logged-in client: the chat list.
inline constexpr Sel LOGGED_IN_MARKER{"logged_in_marker", "role=list[name='Friends Feed']"};

// --- login flow on accounts.snapchat.com ---
inline constexpr Sel LOGIN_SUBMIT{"login_submit", "role=button[name='Log in']"};
inline constexpr Sel ACC_USERNAME{"acc_username", "role=textbox[name='Username or Email']"};
inline constexpr Sel ACC_PASSWORD{"acc_password", "main input[type='password']:visible"};
inline constexpr Sel ACC_OTHER_INPUT{
    "acc_other_input",
    "main input:not([type='password']):not([type='hidden']):not([type='checkbox']):not([type='radio']):visible"};
inline constexpr Sel ACC_HEADING{"acc_heading", "main h1"};
inline constexpr Sel ACC_MESSAGE{"acc_message", "main p"};
inline constexpr Sel ACC_SUBMIT{"acc_submit", "role=button[name=/^(next|log ?in|submit|verify|continue|confirm)$/i]"};
inline constexpr Sel ACC_VERIFYING{"acc_verifying", "role=heading[name='Verifying Your Request']"};
inline constexpr Sel COOKIE_ESSENTIAL{"cookie_essential", "role=button[name='Essential Only']"};

// --- logged-in client ---
inline constexpr Sel NOTIFY_NOT_NOW{"notify_not_now", "role=button[name='Not now']"};
inline constexpr Sel FEED{"feed", "[role='list'][aria-label='Friends Feed']"};
inline constexpr Sel CONV_LIST{"conv_list", "ul[id^='cv-']"};
inline constexpr Sel CONV_CLOSE{"conv_close", "button[title='Close Chat']"};
inline constexpr Sel COMPOSER{"composer", "[role='textbox'][contenteditable='true'][placeholder='Send a chat']"};
inline constexpr Sel UPLOAD_INPUT{"upload_input", "input[type='file'][name='uploadImages']"};
// Full-viewport backdrop of menus/pop-ups; a real click on it closes the pop-up.
inline constexpr Sel OVERLAY{"overlay", "#portal-container > *"};
inline constexpr Sel MENU_REACTION{"menu_reaction", "img[alt^='Reaction ']"};
inline constexpr std::string_view MENU_ITEMS[] = {"Save in Chat", "Unsave in Chat", "Reply", "Copy Text", "Delete"};
inline constexpr Sel SNAP_CLICK_TO_VIEW{"snap_click_to_view", "text='Click to view'"};
inline constexpr Sel STORIES{"stories", "[title='View stories']"};
inline constexpr std::string_view NOT_SUPPORTED_ON_WEB = "Not Supported on Web";

// --- snap / story viewer ---
inline constexpr Sel VIEWER_MEDIA{"viewer_media",
                                  "[aria-label='media content'] img, [aria-label='media content'] video"};
inline constexpr Sel VIEWER_ADVANCE{"viewer_advance", "[aria-label='media content'] [role='button']"};
inline constexpr Sel VIEWER_CLOSE{"viewer_close", "[aria-label='media content'] [aria-label='close'] button"};

// --- camera (photo snaps only: the web shutter has just an onClick) ---
inline constexpr Sel CAMERA_BUTTON{"camera_button", "button:has(+ div [role='textbox'][contenteditable='true'])"};
inline constexpr Sel CAMERA_GOT_IT{"camera_got_it", "text='Got it!'"};
inline constexpr Sel CAMERA_VIDEO{"camera_video", "#local-video"};
inline constexpr Sel CAMERA_SHUTTER{"camera_shutter",
                                    "#portal-container button:not([title]):has(+ button[title] > img[alt])"};
inline constexpr Sel CAMERA_LENS_PREV{"camera_lens_prev", "#portal-container button[aria-keyshortcuts='left']"};
inline constexpr Sel CAMERA_LENS_NEXT{"camera_lens_next", "#portal-container button[aria-keyshortcuts='right']"};
inline constexpr Sel CAMERA_LENS{"camera_lens", "#portal-container button[title] > img[alt]"};
inline constexpr Sel CAMERA_OFF{"camera_off", "button[title='Turn off camera']"};
inline constexpr Sel PREVIEW_MEDIA{"preview_media", "#snap-preview-container img, #snap-preview-container video"};
inline constexpr Sel PREVIEW_CAPTION_BUTTON{"preview_caption_button", "button[title='Add a caption']"};
inline constexpr Sel PREVIEW_CAPTION{"preview_caption", "textarea[aria-label='Caption Input']"};
inline constexpr Sel PREVIEW_DISCARD{"preview_discard", "button[title='Close snap preview and return to camera.']"};
inline constexpr Sel PREVIEW_SEND_TO{"preview_send_to", "role=button[name='Send To']"};
inline constexpr Sel SENDTO_FORM{"sendto_form", "#portal-container form"};
inline constexpr std::string_view SENDTO_SELECTED_MARK = "Unselect chosen user";
inline constexpr Sel SENDTO_SUBMIT{"sendto_submit", "#portal-container form button[type='submit']"};

// Selectors handed to page.js (its SEL argument).
inline nlohmann::json page_selectors() {
  return {
      {"feed", "[role='list'][aria-label='Friends Feed']"},
      {"feed_row", "[role='button'][aria-labelledby^='title-']"},
      {"feed_row_title", "[id^='title-']"},
      {"feed_row_status", "[id^='status-']"},
      {"feed_row_time", "time[datetime]"},
      {"feed_row_avatar", "img[role='presentation']"},
      {"conv_list", "ul[id^='cv-']"},
      {"conv_item", ":scope > li"},
      {"msg_item", ":scope > div > ul > li, :scope > ul > li"},
      {"msg_header", ":scope > header"},
      {"msg_reactions", ":scope > ul"},
      {"msg_quote_list", "ul"},
      {"msg_text", "span[dir='auto']"},
      {"msg_reaction_img", "img[alt^='Reaction ']"},
      {"viewer_media", std::string(VIEWER_MEDIA.css)},
      {"sendto_form", std::string(SENDTO_FORM.css)},
      {"__not_supported", std::string(NOT_SUPPORTED_ON_WEB)},
  };
}

}  // namespace ghost::sel

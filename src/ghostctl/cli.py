"""Command-line entry point: `ghostctl [login|logout|forget|check|config|inspect]`."""

from __future__ import annotations

import argparse
import asyncio
import sys

from .browser import Browser, ProfileLocked, Session, from_config


async def _login_terminal(remember: bool) -> int:
    from . import creds as C
    from .login import TerminalUI, run_login

    b = from_config()
    try:
        st = await b.start()
        if st.session is Session.LOGGED_IN:
            print("Already logged in; the session is saved in the profile.")
            if remember:
                return _remember_now()
            print("To save your login for automatic re-login: `ghostctl login --remember`.")
            print("To switch accounts: `ghostctl logout` first.")
            return 0
        ui = TerminalUI()
        print("Logging in to Snapchat Web (no browser window; your password is typed")
        print("into Snapchat's own form).")
        got = None
        saved = C.load()
        if saved:
            print(f"Using the saved login for {saved.username}...")
            got = await run_login(b, ui, saved)
            if got is None:
                print("The saved login didn't work; enter your details.")
                await b.goto_web()
        if got is None:
            got = await run_login(b, ui)
        if got is None:
            print("Login not completed.")
            return 1
        print("Logged in. Saving session...")
        await asyncio.sleep(3)  # let cookies/IndexedDB flush to the profile
        if got.password and C.available() and C.load() != got:
            if await ui.confirm("Remember this login in your system keyring, so ghostctl can log "
                                "back in by itself if Snapchat ever logs you out?"):
                print("Saved to the keyring." if C.save(got) else "Couldn't write to the keyring.")
        print("Done. Run `ghostctl`.")
        return 0
    finally:
        await b.close()


def _remember_now() -> int:
    """Save a login to the keyring without logging in (it's checked the next time
    Snapchat asks ghostctl to log in; if it's wrong, you get the login form)."""
    import getpass

    from . import creds as C

    if not C.available():
        print("No system keyring available (Secret Service / Keychain); nothing saved.")
        return 1
    user = input("Username or email: ").strip()
    pw = getpass.getpass("Password (stored in your keyring): ")
    if not user or not pw:
        return 1
    print("Saved to the keyring." if C.save(C.Creds(user, pw)) else "Couldn't write to the keyring.")
    return 0


def _forget() -> int:
    from . import creds as C

    print("Deleted the saved login from the keyring." if C.forget() else "No saved login.")
    return 0


def _logout() -> int:
    import shutil

    from . import config, creds as C

    profile = config.load().profile_dir
    a = input(f"Log out: delete the saved login and the session in {profile}? [y/N] ")
    if not a.lower().startswith("y"):
        return 1
    C.forget()
    shutil.rmtree(profile, ignore_errors=True)
    print("Logged out. Run `ghostctl login` (or just `ghostctl`) to log in again.")
    return 0


async def _login_window() -> int:
    b = from_config()
    await b.open_headed()
    print(
        "A browser window is open on Snapchat Web. Log in there yourself\n"
        "(QR code, 2FA, captcha, whatever it asks). ghostctl never sees your password.\n"
        "Waiting for login... (close the window to abort)"
    )
    try:
        while not b.closed and b.context and b.context.pages:
            if await b.session_state(timeout=5) is Session.LOGGED_IN:
                print("Logged in. Saving session...")
                await asyncio.sleep(3)  # let cookies/IndexedDB flush to the profile
                print("Done. Run `ghostctl check`, then `ghostctl`.")
                return 0
        print("Window closed before login was detected.")
        print(
            "If you did log in, the login detector may be stale: run `ghostctl check`;"
            " if it says unknown, run `ghostctl inspect`."
        )
        return 1
    finally:
        await b.close()


async def _check() -> int:
    b = from_config()
    try:
        status = await b.start()
    finally:
        await b.close()
    print(f"mode:    {status.mode.value}")
    print(f"session: {status.session.value}")
    if status.note:
        print(f"note:    {status.note}")
    return 0 if status.session is Session.LOGGED_IN else 1


def _config(edit: bool) -> int:
    import os
    import subprocess

    from . import config

    created = config.write_default()
    print(("Created " if created else "") + str(config.CONFIG_PATH))
    if edit:
        editor = os.environ.get("VISUAL") or os.environ.get("EDITOR") or "nano"
        return subprocess.call([editor, str(config.CONFIG_PATH)])
    return 0


async def _inspect() -> int:
    from . import inspect

    await inspect.run()
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(prog="ghostctl", description="Terminal client for Snapchat Web.")
    sub = parser.add_subparsers(dest="cmd")
    login = sub.add_parser("login", help="log in from the terminal")
    login.add_argument(
        "--window", action="store_true", help="log in in a visible browser window instead"
    )
    login.add_argument(
        "--remember", action="store_true",
        help="save your login in the system keyring so ghostctl can log back in by itself",
    )
    sub.add_parser("logout", help="delete the session and any saved login")
    sub.add_parser("forget", help="delete the saved login from the keyring (stay logged in)")
    sub.add_parser("check", help="verify the saved session")
    cfg = sub.add_parser("config", help="create the config file if missing and print its path")
    cfg.add_argument("--edit", action="store_true", help="open it in $EDITOR")
    sub.add_parser("inspect", help="open the page headed and dump DOM/accessibility tree to debug/")
    args = parser.parse_args()

    try:
        if args.cmd == "login":
            rc = asyncio.run(_login_window() if args.window else _login_terminal(args.remember))
        elif args.cmd == "logout":
            rc = _logout()
        elif args.cmd == "forget":
            rc = _forget()
        elif args.cmd == "check":
            rc = asyncio.run(_check())
        elif args.cmd == "config":
            rc = _config(args.edit)
        elif args.cmd == "inspect":
            rc = asyncio.run(_inspect())
        else:
            from .app import run_app

            rc = run_app()
    except ProfileLocked as e:
        print(f"error: {e}", file=sys.stderr)
        rc = 2
    except KeyboardInterrupt:
        rc = 130
    sys.exit(rc)

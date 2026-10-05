"""Command-line entry point: `ghostctl [login|inspect|check]`."""

from __future__ import annotations

import argparse
import asyncio
import sys

from .browser import Browser, ProfileLocked, Session, from_config


async def _login_terminal() -> int:
    from .login import terminal_login

    b = from_config()
    try:
        ok = await terminal_login(b)
        if ok:
            print("Logged in. Saving session...")
            await asyncio.sleep(3)  # let cookies/IndexedDB flush to the profile
            print("Done. Run `ghostctl`.")
            return 0
        print("Login not completed.")
        return 1
    finally:
        await b.close()


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
    sub.add_parser("check", help="verify the saved session (headless, then off-screen fallback)")
    cfg = sub.add_parser("config", help="create the config file if missing and print its path")
    cfg.add_argument("--edit", action="store_true", help="open it in $EDITOR")
    sub.add_parser("inspect", help="open the page headed and dump DOM/accessibility tree to debug/")
    args = parser.parse_args()

    try:
        if args.cmd == "login":
            rc = asyncio.run(_login_window() if args.window else _login_terminal())
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

"""Optional saved login, kept in the system keyring (Secret Service / Keychain /
Windows Credential Locker) — never in a file. Used to log back in automatically
if Snapchat ever ends the session."""

from __future__ import annotations

from dataclasses import dataclass

SERVICE = "ghostctl"
_USER_KEY = "__username__"


@dataclass
class Creds:
    username: str
    password: str


def _keyring():
    try:
        import keyring
        from keyring.backends.fail import Keyring as FailKeyring

        kr = keyring.get_keyring()
        if isinstance(kr, FailKeyring):
            return None
        return keyring
    except Exception:  # noqa: BLE001 - no usable backend
        return None


def available() -> bool:
    return _keyring() is not None


def load() -> Creds | None:
    kr = _keyring()
    if kr is None:
        return None
    try:
        user = kr.get_password(SERVICE, _USER_KEY)
        pw = kr.get_password(SERVICE, user) if user else None
    except Exception:  # noqa: BLE001 - locked or unavailable keyring
        return None
    return Creds(user, pw) if user and pw else None


def save(c: Creds) -> bool:
    kr = _keyring()
    if kr is None:
        return False
    try:
        kr.set_password(SERVICE, _USER_KEY, c.username)
        kr.set_password(SERVICE, c.username, c.password)
        return True
    except Exception:  # noqa: BLE001
        return False


def forget() -> bool:
    """Delete the saved login. Returns True if something was deleted."""
    kr = _keyring()
    if kr is None:
        return False
    try:
        user = kr.get_password(SERVICE, _USER_KEY)
        if user:
            try:
                kr.delete_password(SERVICE, user)
            except Exception:  # noqa: BLE001
                pass
            kr.delete_password(SERVICE, _USER_KEY)
            return True
    except Exception:  # noqa: BLE001
        pass
    return False

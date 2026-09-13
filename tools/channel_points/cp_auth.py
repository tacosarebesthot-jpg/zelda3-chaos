#!/usr/bin/env python3
"""cp_auth.py - the one-command Channel Points unlock (owner runbook step 2).

Reads tools/channel_points/cp_config.ini, PRINTS the Twitch authorization
URL (and copies it to the clipboard - no browser is opened automatically),
runs a loopback-only server at http://localhost:3000/ that catches the
?code=... redirect when you get around to clicking the link and clicking
Authorize, exchanges the code for a user access token, validates it, then
writes token/expires_at/refresh_token AND login (the Twitch account that
authorized - whoever signed in, no hardcoding) into cp_config.ini
ATOMICALLY. The token and secret are never printed; the safe fields
(login / scopes / expiry) are.

Prerequisite: cp_config.ini has client_id and client_secret filled in.
(Or pass --client-id/--client-secret and they are written into the file
for you. The dev app's registered redirect MUST be exactly:
    http://localhost:3000/        <- trailing slash included
)

Usage:
    python tools/channel_points/cp_auth.py
    python tools/channel_points/cp_auth.py --client-id XYZ --client-secret ABC

Stdlib only. Windows-friendly. Never print the token or the secret.
"""
import argparse
import configparser
import html
import http.server
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# Git Bash / cp1252 consoles: force utf-8 so unicode-safe messages never
# crash the script (em dashes etc.).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = os.path.join(BASE_DIR, "cp_config.ini")

PORT = 3000
DEFAULT_REDIRECT_URI = "http://localhost:3000/"
DEFAULT_SCOPE = "channel:read:redemptions"

AUTHORIZE_URL = "https://id.twitch.tv/oauth2/authorize"
TOKEN_URL = "https://id.twitch.tv/oauth2/token"
VALIDATE_URL = "https://id.twitch.tv/oauth2/validate"

MISSING_INSTRUCTIONS = """\
MISSING CREDENTIALS - stopped before touching the network. Nothing changed.

You need to paste TWO values into this file:

    {config}

Open it in Notepad and fill in these lines (values come from the
dev.twitch.tv console, app "test 36" -> Manage -> https://dev.twitch.tv/console):

    [eventsub]
    client_id = <paste the Client ID from the manage page here>
    client_secret = <click "New Secret", paste the value here>

Then run again:

    python tools/channel_points/cp_auth.py

One-time app check (only if auth fails with "redirect mismatch"): the app's
OAuth Redirect URLs entry must be EXACTLY  http://localhost:3000/
(trailing slash included).

Alternative without editing the file - pass the values on the command line
(they get written into cp_config.ini for you):

    python tools/channel_points/cp_auth.py --client-id <ID> --client-secret <SECRET>
"""


def load_config(path):
    """Return a ConfigParser for path, or None if the file does not exist."""
    ini = configparser.ConfigParser(interpolation=None)
    with open(path, "r", encoding="utf-8") as f:
        ini.read_file(f)
    return ini


def ini_find(ini, key):
    """Value of `key` searched across ALL sections (layout-agnostic)."""
    for section in ini.sections():
        if ini.has_option(section, key):
            return ini.get(section, key, fallback="").strip()
    return ""


def update_config_keys(path, updates):
    """Set key=value for every entry in `updates`, preserving all comments.

    Atomic: writes path + '.tmp' then os.replace()s it over the target, so a
    crash can never leave a half-written config. Keys are matched at line
    starts only; if a key does not exist yet it is inserted right after the
    [auth] section header (so cp_listen.py, which reads [auth] token, finds
    it), or appended as a new [auth] section at the end.
    """
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    for key, value in updates.items():
        pattern = re.compile(r"(?m)^([ \t]*%s[ \t]*=[ \t]*)[^\r\n]*"
                             % re.escape(key))
        if pattern.search(text):
            # lambda so backslashes/ampersands in `value` are never
            # interpreted as regex replacement escapes.
            text = pattern.sub(lambda m, v=value: m.group(1) + v, text, count=1)
            continue
        header = re.compile(r"(?m)^\[auth\][ \t]*\r?\n")
        if header.search(text):
            text = header.sub(lambda m: m.group(0) + "%s = %s\n" % (key, value),
                              text, count=1)
        else:
            text = text.rstrip("\r\n") + "\n\n[auth]\n%s = %s\n" % (key, value)

    tmp_path = path + ".tmp"
    with open(tmp_path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    os.replace(tmp_path, path)


def copy_to_clipboard(text):
    """Best-effort clipboard copy. Tries PowerShell Set-Clipboard (text fed
    via stdin, so no quoting issues), then the raw Win32 clipboard via
    ctypes. Returns True on success; failure is harmless - the URL is
    printed right above and can be copied by hand."""
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "$input | Set-Clipboard"],
            input=text.encode("utf-8"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        if proc.returncode == 0:
            return True
    except Exception:
        pass
    try:
        import ctypes
        CF_UNICODETEXT = 13
        GMEM_MOVEABLE = 0x0002
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        if not user32.OpenClipboard(0):
            return False
        try:
            user32.EmptyClipboard()
            blob = text.encode("utf-16-le") + b"\x00\x00"
            handle = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(blob))
            if not handle:
                return False
            ptr = kernel32.GlobalLock(handle)
            if not ptr:
                kernel32.GlobalFree(handle)
                return False
            try:
                ctypes.memmove(ptr, blob, len(blob))
            finally:
                kernel32.GlobalUnlock(handle)
            if not user32.SetClipboardData(CF_UNICODETEXT, handle):
                kernel32.GlobalFree(handle)  # system owns it on success
                return False
            return True
        finally:
            user32.CloseClipboard()
    except Exception:
        return False


class LoopbackCatcher(http.server.BaseHTTPRequestHandler):
    """Catches GET http://localhost:3000/?code=... . Loopback-only: any peer
    address other than 127.0.0.1 / ::1 is refused outright."""

    code = None
    error = None

    def _respond(self, status, body):
        payload = ("<!doctype html><html><head><meta charset=\"utf-8\">"
                   "<title>zelda3 channel points</title></head><body>"
                   + body + "</body></html>").encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        peer = self.client_address[0]
        if peer not in ("127.0.0.1", "::1"):
            LoopbackCatcher.error = ("refused non-loopback connection from %s"
                                     % peer)
            self._respond(403, "<h2>refused: non-loopback peer</h2>")
            return
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if "code" in query:
            LoopbackCatcher.code = query["code"][0]
            self._respond(200, "<h2>received &mdash; you can close this tab"
                               "</h2>")
        elif "error" in query:
            err = query["error"][0]
            desc = query.get("error_description", [""])[0]
            LoopbackCatcher.error = "Twitch returned: %s (%s)" % (err, desc)
            self._respond(200, "<h2>authorization failed: %s</h2>"
                               "<p>Close this tab and run cp_auth.py again."
                               "</p>" % html.escape(err))
        else:
            # favicon.ico and friends: keep waiting for the real redirect.
            self._respond(404, "<h2>waiting for the OAuth redirect...</h2>")

    def log_message(self, fmt, *args):  # silence request logging
        pass


def build_authorize_url(client_id, redirect_uri, scope):
    params = urllib.parse.urlencode({
        "response_type": "code",
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "scope": scope,
    })
    return AUTHORIZE_URL + "?" + params


def exchange_code(client_id, client_secret, code, redirect_uri):
    """POST the auth code to id.twitch.tv/oauth2/token. Returns the parsed
    JSON (access_token / expires_in / refresh_token). Never prints any of it.
    """
    data = urllib.parse.urlencode({
        "client_id": client_id,
        "client_secret": client_secret,
        "code": code,
        "grant_type": "authorization_code",
        "redirect_uri": redirect_uri,
    }).encode("ascii")
    req = urllib.request.Request(TOKEN_URL, data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(body)
            detail = "HTTP %d %s: %s" % (exc.code, parsed.get("error", "?"),
                                         parsed.get("error_description", ""))
        except ValueError:
            detail = "HTTP %d: %s" % (exc.code, body.strip()[:120])
        raise RuntimeError("token exchange failed (%s)" % detail.strip())
    except urllib.error.URLError as exc:
        raise RuntimeError("cannot reach %s (%s)" % (TOKEN_URL, exc.reason))
    if "access_token" not in payload:
        raise RuntimeError("token exchange returned no access_token")
    return payload


def validate_token(token):
    """GET id.twitch.tv/oauth2/validate with the token. Returns
    (dict_or_None, http_status). Prints nothing itself."""
    req = urllib.request.Request(VALIDATE_URL,
                                 headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8")), resp.status
    except urllib.error.HTTPError as exc:
        return None, exc.code
    except urllib.error.URLError as exc:
        raise RuntimeError("cannot reach %s (%s)" % (VALIDATE_URL, exc.reason))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Mint the Channel Points user token (see README.md).")
    parser.add_argument("--client-id", default="",
                        help="write this Client ID into cp_config.ini first")
    parser.add_argument("--client-secret", default="",
                        help="write this Client Secret into cp_config.ini first")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help="config file (default: cp_config.ini next to "
                             "this script)")
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the browser redirect "
                             "(default 600)")
    args = parser.parse_args(argv)

    print("zelda3 Channel Points auth - config: %s" % args.config)

    # ---- 1. credentials -------------------------------------------------
    if not os.path.isfile(args.config):
        print(MISSING_INSTRUCTIONS.format(config=args.config))
        return 1
    ini = load_config(args.config)

    client_id = args.client_id or ini_find(ini, "client_id")
    client_secret = args.client_secret or ini_find(ini, "client_secret")
    if not client_id or not client_secret:
        print(MISSING_INSTRUCTIONS.format(config=args.config))
        return 1

    redirect_uri = ini_find(ini, "redirect_uri") or DEFAULT_REDIRECT_URI
    scope = ini_find(ini, "scope") or DEFAULT_SCOPE
    if redirect_uri != DEFAULT_REDIRECT_URI:
        print("WARNING: cp_config.ini redirect_uri is %r - the dev app "
              "\"test 36\" is registered for exactly %r. If Twitch answers "
              "\"redirect mismatch\", fix the ini AND the app console to that "
              "exact string (trailing slash)." % (redirect_uri,
                                                  DEFAULT_REDIRECT_URI))

    # ---- 2. optionally persist --client-id / --client-secret ------------
    if args.client_id or args.client_secret:
        updates = {}
        if args.client_id:
            updates["client_id"] = args.client_id
        if args.client_secret:
            updates["client_secret"] = args.client_secret
        update_config_keys(args.config, updates)
        print("cp_config.ini updated: client_id%s written (values not shown)."
              % ("/client_secret" if args.client_secret else ""))

    # ---- 3. loopback server FIRST (must be listening before the redirect)
    try:
        server = http.server.HTTPServer(("127.0.0.1", PORT), LoopbackCatcher)
    except OSError as exc:
        print("FAILED: cannot listen on port %d (%s)." % (PORT, exc))
        print("Something else is using port %d - close it and re-run." % PORT)
        return 1
    server.timeout = 1  # so handle_request() wakes up every second
    print("Loopback catcher listening on http://localhost:%d/ "
          "(accepts 127.0.0.1/::1 only)." % PORT)

    authorize_url = build_authorize_url(client_id, redirect_uri, scope)
    print("\nSTEP 1 - open this link in your browser, then click Authorize "
          "(sign in as the broadcaster):\n")
    print(authorize_url + "\n")
    if copy_to_clipboard(authorize_url):
        print("The link is also on your clipboard - just paste it into a "
              "browser tab.")
    else:
        print("(Clipboard copy failed - select and copy the link above by "
              "hand.)")
    print("This window keeps waiting; continue whenever you are ready.")

    # ---- 4. wait for the redirect ---------------------------------------
    print("\nSTEP 2 - waiting for the redirect (up to %d seconds)..."
          % args.timeout)
    deadline = time.time() + max(args.timeout, 5)
    while LoopbackCatcher.code is None and LoopbackCatcher.error is None:
        server.handle_request()
        if time.time() > deadline:
            LoopbackCatcher.error = "timed out waiting for the redirect"
    server.server_close()

    if LoopbackCatcher.error and LoopbackCatcher.code is None:
        print("FAILED: %s" % LoopbackCatcher.error)
        return 1
    print("Authorization code received (not shown).")

    # ---- 5. exchange the code -------------------------------------------
    print("\nSTEP 3 - exchanging the code for a user access token...")
    try:
        payload = exchange_code(client_id, client_secret, LoopbackCatcher.code,
                                redirect_uri)
    except RuntimeError as exc:
        print("FAILED: %s" % exc)
        return 1
    access_token = payload["access_token"]
    expires_in = payload.get("expires_in")
    refresh_token = payload.get("refresh_token", "")
    print("Token received (%d chars, not shown)." % len(access_token))

    # ---- 6. validate FIRST, so the account name can be stored with it ----
    print("\nSTEP 4 - validating the token against %s ..." % VALIDATE_URL)
    login = ""
    try:
        info, status = validate_token(access_token)
    except RuntimeError as exc:
        info, status = None, None
        print("WARNING: validation could not run (%s) - the token is saved "
              "anyway; check with cp_probe.py." % exc)
    if info is None and status is not None:
        print("WARNING: validation answered HTTP %d right after minting - "
              "the token is saved anyway; run cp_probe.py afterwards."
              % status)
    if info is not None:
        login = info.get("login", "") or ""
        print("VALIDATED. Token belongs to account: %s" % (login or "?"))
        print("  scopes   : %s" % ", ".join(info.get("scopes", [])))
        expires_in_s = info.get("expires_in")
        if expires_in_s is not None:
            print("  expires  : in %s seconds (~%.1f hours)"
                  % (expires_in_s, int(expires_in_s) / 3600.0))
        if DEFAULT_SCOPE not in info.get("scopes", []):
            print("WARNING: token is MISSING scope %s - re-authorize."
                  % DEFAULT_SCOPE)

    # ---- 7. atomic write: token + expiry + refresh + WHO authorized ------
    # login= records the authenticated account as a first-class config fact;
    # cp_listen.py derives the channel from it and warns on mismatches.
    updates = {"token": access_token}
    if expires_in is not None:
        updates["expires_at"] = str(int(time.time() + int(expires_in)))
    if refresh_token:
        updates["refresh_token"] = refresh_token
    if login:
        updates["login"] = login
    update_config_keys(args.config, updates)
    written = "token"
    for extra in ("expires_at", "refresh_token", "login"):
        if extra in updates:
            written += "/" + extra
    print("cp_config.ini updated atomically: %s written. The token and the "
          "secret are never printed or logged - they live only in that file."
          % written)

    if info is None:
        print("\nDONE (token saved but UNVALIDATED). Confirm with:")
        print("    python tools/channel_points/cp_probe.py")
        return 1
    if DEFAULT_SCOPE not in info.get("scopes", []):
        print("\nDONE WITH WARNINGS - the token lacks the needed scope; "
              "re-run cp_auth.py.")
        return 1

    print("\nDONE. Whoever signed in owns this token - the channel follows "
          "the token automatically (or set [auth] channel to override).")
    print("Verify anytime with:")
    print("    python tools/channel_points/cp_probe.py")
    print("Then start the listener with:")
    print("    python tools/channel_points/cp_listen.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

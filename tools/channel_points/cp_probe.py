#!/usr/bin/env python3
"""cp_probe.py - quick health check for the Channel Points token.

Reads tools/channel_points/cp_config.ini, validates the stored token against
https://id.twitch.tv/oauth2/validate and prints the status (login / scopes /
expiry - all safe to show). Exit code 0 = token valid with the needed scope,
1 = anything else.

Usage:
    python tools/channel_points/cp_probe.py
    python tools/channel_points/cp_probe.py --config path/to/cp_config.ini

Stdlib only. The token itself is never printed.
"""
import argparse
import configparser
import json
import os
import sys
import time
import urllib.error
import urllib.request

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = os.path.join(BASE_DIR, "cp_config.ini")
VALIDATE_URL = "https://id.twitch.tv/oauth2/validate"
NEEDED_SCOPE = "channel:read:redemptions"

NO_TOKEN_MESSAGE = "no token — run cp_auth.py"


def ini_find(ini, key):
    """Value of `key` searched across ALL sections (layout-agnostic)."""
    for section in ini.sections():
        if ini.has_option(section, key):
            return ini.get(section, key, fallback="").strip()
    return ""


def validate(token):
    """Returns (dict_or_None, http_status_or_None)."""
    req = urllib.request.Request(
        VALIDATE_URL, headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            return json.loads(resp.read().decode("utf-8")), resp.status
    except urllib.error.HTTPError as exc:
        return None, exc.code
    except urllib.error.URLError as exc:
        print("NETWORK ERROR: cannot reach %s (%s)" % (VALIDATE_URL, exc.reason))
        return None, None


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Check the stored Channel Points token (exit 0 = good).")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help="config file (default: cp_config.ini next to "
                             "this script)")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.config):
        print("FAIL: %s not found - %s" % (args.config, NO_TOKEN_MESSAGE))
        return 1

    ini = configparser.ConfigParser(interpolation=None)
    with open(args.config, "r", encoding="utf-8") as f:
        ini.read_file(f)

    token = ini_find(ini, "token")
    if not token:
        print("FAIL: %s" % NO_TOKEN_MESSAGE)
        return 1

    # cp_config.ini advisory: expires_at (unix seconds) written by cp_auth.py.
    expires_at = ini_find(ini, "expires_at")
    if expires_at.isdigit() and int(expires_at) < time.time():
        print("NOTE: cp_config.ini says the token expired at %s (clock time). "
              "Asking Twitch to confirm..." % time.strftime(
                  "%Y-%m-%d %H:%M:%S", time.localtime(int(expires_at))))

    print("token present (%d chars, not shown) - validating against %s ..."
          % (len(token), VALIDATE_URL))
    info, status = validate(token)

    if info is None:
        if status == 401:
            print("FAIL: token rejected (HTTP 401) - expired or revoked. "
                  "Twitch user tokens live ~4 hours: re-run cp_auth.py to "
                  "mint a fresh one (or renew via the stored refresh_token).")
        elif status is not None:
            print("FAIL: validation answered HTTP %d - see "
                  "dev.twitch.tv docs; if this persists, re-run cp_auth.py."
                  % status)
        return 1

    login = info.get("login", "?")
    scopes = info.get("scopes", [])
    expires_in = info.get("expires_in")
    print("VALID (HTTP 200):")
    print("  login    : %s" % login)
    print("  scopes   : %s" % (", ".join(scopes) if scopes else "(none)"))
    if expires_in is not None:
        print("  expires  : in %s seconds (~%.1f hours)"
              % (expires_in, int(expires_in) / 3600.0))

    # consistency: [auth] login= (written by cp_auth.py) vs the live token
    stored_login = ini_find(ini, "login")
    if stored_login and login != "?" \
            and stored_login.casefold() != str(login).casefold():
        print("NOTE: [auth] login=%r in cp_config.ini is stale - the token "
              "now belongs to %r. Harmless for the token itself; re-run "
              "cp_auth.py to refresh the record." % (stored_login, login))

    if NEEDED_SCOPE not in scopes:
        print("FAIL: token is MISSING the required scope %s - re-run "
              "cp_auth.py." % NEEDED_SCOPE)
        return 1

    print("OK: Channel Points token is live and has %s." % NEEDED_SCOPE)
    return 0


if __name__ == "__main__":
    sys.exit(main())

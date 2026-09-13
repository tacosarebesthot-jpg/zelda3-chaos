#!/usr/bin/env python3
"""cp_refresh.py - renew the Channel Points token WITHOUT the browser ritual.

cp_auth.py mints a ~4-hour user token and stores its refresh_token in
cp_config.ini [auth].  This tool trades that refresh_token for a fresh
access token - no browser, no port 3000, no re-authorize:

    POST https://id.twitch.tv/oauth2/token
        grant_type=refresh_token & refresh_token=... & client_id=... & client_secret=...

then writes back into cp_config.ini ATOMICALLY (same .tmp + os.replace()
path as cp_auth.py):
  token          the new access token
  expires_at     now + expires_in from the token response
  refresh_token  only when Twitch rotated it (rewriting the same value is a
                 no-op; written BEFORE anything else so a rotation is never
                 lost)
  login          the validated account (after validation succeeds)

Only safe fields are ever printed (login / scopes / expiry / whether the
refresh_token rotated).  The access token, the refresh token, and the client
secret are NEVER printed or logged - they live only in cp_config.ini.

Exit codes:  0 = refreshed and written;  1 = failed.  When Twitch rejects
the refresh (revoked / expired / already-used refresh token) the failure
message says plainly: RE-AUTH REQUIRED - run cp_auth.py.

Usage:
    python tools/channel_points/cp_refresh.py
    python tools/channel_points/cp_refresh.py --config path/to/cp_config.ini

cp_listen.py imports refresh_config() / is_stale() / has_refresh_token()
from this module for its automatic refresh (once at startup when the token
is stale, plus one retry when Helix answers 401 mid-run).

Stdlib only.  Windows-friendly (utf-8 console reconfigure, like cp_auth.py).
"""

import argparse
import configparser
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONFIG = os.path.join(BASE_DIR, "cp_config.ini")

# Reuse cp_auth's atomic config writer + token validation so both tools share
# one code path (cp_auth is import-safe: all its work happens in main()).
from cp_auth import TOKEN_URL, ini_find, load_config, update_config_keys, \
    validate_token

REFRESH_MARGIN = 10 * 60  # "expired or dies within 10 minutes" counts as stale

REAUTH_MESSAGE = ("RE-AUTH REQUIRED - run cp_auth.py:\n"
                  "    python tools/channel_points/cp_auth.py")

# reasons a refresh ends without ok=True (returned in result["reason"]):
#   no_config        config file missing
#   no_refresh_token nothing stored to refresh with
#   no_credentials   client_id / client_secret missing
#   rejected         Twitch refused the refresh (revoked/expired/used token)
#   network          could not reach id.twitch.tv (nothing was changed)
#   write_failed     token minted but the config write failed (rare, bad)
#   bad_response     token endpoint answered, but without an access_token


def has_refresh_token(path):
    """True when the config holds a non-empty refresh_token (any section)."""
    try:
        ini = load_config(path)
    except (OSError, configparser.Error):
        return False
    return bool(ini_find(ini, "refresh_token"))


def is_stale(expires_at, now=None, margin_secs=REFRESH_MARGIN):
    """True when expires_at is missing/zero, or the token dies within
    margin_secs (10 minutes by default)."""
    try:
        expires_at = int(expires_at or 0)
    except (TypeError, ValueError):
        return True
    if expires_at <= 0:
        return True
    now = time.time() if now is None else now
    return (expires_at - now) < margin_secs


def refresh_config(config_path):
    """One refresh round against Twitch.  Returns a dict and NEVER prints
    the token / secret / refresh_token (callers may log the safe fields).

    Keys: ok (bool), reason (see above), error (human text, safe to print),
    token (the NEW access token - for the caller's in-memory cfg ONLY, never
    log it), expires_at, expires_in, login, scopes, validated, rotated.
    """
    result = {"ok": False, "reason": "", "error": "", "token": "",
              "expires_at": 0, "expires_in": 0, "login": "", "scopes": [],
              "validated": False, "rotated": False}

    if not os.path.isfile(config_path):
        result["reason"] = "no_config"
        result["error"] = "%s not found" % config_path
        return result
    try:
        ini = load_config(config_path)
    except (OSError, configparser.Error) as exc:
        result["reason"] = "no_config"
        result["error"] = "cannot read %s (%s)" % (config_path, exc)
        return result

    refresh_token = ini_find(ini, "refresh_token")
    client_id = ini_find(ini, "client_id")
    client_secret = ini_find(ini, "client_secret")
    if not refresh_token:
        result["reason"] = "no_refresh_token"
        result["error"] = ("cp_config.ini has no refresh_token - cp_auth.py "
                           "stores one when it mints a token")
        return result
    if not client_id or not client_secret:
        missing = "client_id" if not client_id else "client_secret"
        result["reason"] = "no_credentials"
        result["error"] = ("cp_config.ini is missing %s - fill it in as in "
                           "cp_auth.py Step 1" % missing)
        return result

    data = urllib.parse.urlencode({
        "client_id": client_id,
        "client_secret": client_secret,
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
    }).encode("ascii")
    req = urllib.request.Request(TOKEN_URL, data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(body)
            detail = "HTTP %d %s: %s" % (
                exc.code, parsed.get("error", "?"),
                parsed.get("message") or parsed.get("error_description", ""))
        except ValueError:
            detail = "HTTP %d: %s" % (exc.code, body.strip()[:120])
        result["reason"] = "rejected"
        result["error"] = ("Twitch refused the refresh (%s). The stored "
                           "refresh_token is revoked/expired/already-used and "
                           "cannot be recovered." % detail.strip())
        return result
    except urllib.error.URLError as exc:
        result["reason"] = "network"
        result["error"] = ("cannot reach %s (%s) - nothing was changed, the "
                           "old token/refresh_token still stand, try again"
                           % (TOKEN_URL, exc.reason))
        return result

    access_token = payload.get("access_token") or ""
    if not access_token:
        result["reason"] = "bad_response"
        result["error"] = "token endpoint answered without an access_token"
        return result
    try:
        expires_in = int(payload.get("expires_in") or 0)
    except (TypeError, ValueError):
        expires_in = 0
    new_refresh = payload.get("refresh_token") or ""

    # Atomic write FIRST: if Twitch rotated the refresh_token, the new one
    # MUST land on disk before anything else can fail.  Same keys cp_auth.py
    # owns, same .tmp + os.replace() atomicity.
    updates = {"token": access_token}
    if expires_in:
        updates["expires_at"] = str(int(time.time()) + expires_in)
    if new_refresh:
        updates["refresh_token"] = new_refresh
        result["rotated"] = (new_refresh != refresh_token)
    try:
        update_config_keys(config_path, updates)
    except OSError as exc:
        result["reason"] = "write_failed"
        result["error"] = ("token minted but cp_config.ini could not be "
                           "written (%s) - if the refresh_token rotated, "
                           "re-run cp_auth.py" % exc)
        return result

    result["token"] = access_token
    result["expires_in"] = expires_in
    try:
        result["expires_at"] = int(updates.get("expires_at") or 0)
    except ValueError:
        result["expires_at"] = 0

    # Validation is for reporting login/scopes (and proving the token); the
    # token endpoint already told us the token is good.
    try:
        info, status = validate_token(access_token)
    except RuntimeError as exc:
        result["ok"] = True
        result["reason"] = "ok"
        result["error"] = ("stored, but validation could not run (%s) - "
                           "confirm with cp_probe.py" % exc)
        return result
    if info is None:
        result["ok"] = True
        result["reason"] = "ok"
        result["error"] = ("stored, but validation answered HTTP %s right "
                           "after minting - confirm with cp_probe.py"
                           % status)
        return result

    result["validated"] = True
    result["login"] = (info.get("login") or "").strip()
    result["scopes"] = info.get("scopes", []) or []
    if info.get("expires_in") is not None:
        try:  # freshest number; normally identical to expires_in above
            result["expires_at"] = int(time.time()) + int(info["expires_in"])
            update_config_keys(config_path,
                               {"expires_at": str(result["expires_at"])})
        except (TypeError, ValueError, OSError):
            pass
    if result["login"]:
        try:  # same first-class config fact cp_auth.py records
            update_config_keys(config_path, {"login": result["login"]})
        except OSError:
            pass
    result["ok"] = True
    result["reason"] = "ok"
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Renew the Channel Points token from the stored "
                    "refresh_token (no browser, no re-authorize).")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help="config file (default: cp_config.ini next to "
                             "this script)")
    args = parser.parse_args(argv)

    print("zelda3 Channel Points refresh - config: %s" % args.config)
    result = refresh_config(args.config)

    if result["ok"]:
        print("REFRESHED - cp_config.ini updated atomically "
              "(token%s written; values not shown)."
              % ("/refresh_token (rotated)" if result["rotated"] else ""))
        print("  login    : %s" % (result["login"] or "?"))
        print("  scopes   : %s" % (", ".join(result["scopes"])
                                   if result["scopes"]
                                   else "(unknown - validation skipped)"))
        if result["expires_at"]:
            remain = result["expires_at"] - time.time()
            print("  expires  : in %d seconds (~%.1f hours), at %s local"
                  % (max(0, int(remain)), max(0.0, remain / 3600.0),
                     time.strftime("%Y-%m-%d %H:%M:%S",
                                   time.localtime(result["expires_at"]))))
        if result["error"]:
            print("WARNING: %s" % result["error"])
        print("Verify anytime with:  python tools/channel_points/cp_probe.py")
        return 0

    print("FAILED: %s" % (result["error"] or result["reason"]))
    if result["reason"] in ("rejected", "no_refresh_token", "no_config",
                            "no_credentials"):
        print(REAUTH_MESSAGE)
    return 1


if __name__ == "__main__":
    sys.exit(main())

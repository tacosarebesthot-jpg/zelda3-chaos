#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cp_listen.py - Twitch Channel Points -> zelda3 drop-file bridge (prototype).

Bridges real Channel Point redemptions on the broadcaster's Twitch channel
into the game's existing command path.  The broadcaster is whoever's account
authorized the token (cp_config.ini [auth] login, written by cp_auth.py) -
nothing here is tied to a specific account.  The game (src/twitch.c,
drop=1) polls twitch_drop/
for "verb|arg|who|dur" files; this script produces exactly those files from
EventSub websocket notifications.  It never touches the game, its configs,
or the save - the only thing it writes is new drop files (plus its log).

Pipeline:
  Twitch EventSub (wss) --> this script --> twitch_drop/cp_*.txt --> game

Modes:
  python cp_listen.py            real mode: live EventSub + Helix
  python cp_listen.py --mock     offline test against tools/channel_points/test_server_mock.py
                                 (writes to mock_out/ instead of the real twitch_drop/)
  python cp_listen.py --check    validate config + mapping table, no network
  python cp_listen.py --once     do a single session, then exit (implied by --mock)

Token upkeep (real mode): the ~4-hour user token is refreshed AUTOMATICALLY -
once at startup when it is expired or dies within 10 minutes, and once more
if Helix answers 401 mid-run - via cp_refresh.py trading the stored
refresh_token (no browser).  cp_auth.py is only needed when the refresh
token itself is dead (Twitch revoked/expired it): that failure exits loudly
with "RE-AUTH REQUIRED", never a silent death.  --mock/--check stay offline.

Stdlib only: the RFC6455 websocket client below is hand-rolled over ssl
sockets because neither `websocket-client` nor `websockets` is installed in
this environment's Python (checked 2026-09-09).  Note Twitch forbids the
client from sending anything except pong replies on an EventSub socket
(close code 4001), so this client never pings - which keeps it simple.
"""

import argparse
import base64
import configparser
import hashlib
import json
import os
import re
import socket
import ssl
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# cp_refresh.py (and its cp_auth.py helper) live next to this script; make
# the plain imports work no matter which directory python was launched from.
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import cp_refresh  # noqa: E402  (refresh_token grant; see maybe_startup_refresh)

VERSION = "0.1"

DEFAULT_WSS_URL = "wss://eventsub.wss.twitch.tv/ws"
DEFAULT_HELIX_BASE = "https://api.twitch.tv/helix"

SUB_AUTO = "channel.channel_points_automatic_reward_redemption.add"
SUB_CUSTOM = "channel.channel_points_custom_reward_redemption.add"
SUB_VERSION = "1"  # v1 of both; automatic also has a v2 (payload differs - see README)

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Mirror of kVerbs[] in src/twitch.c - used only to warn at load time and to
# explain rejections; the game re-validates every verb anyway.
KNOWN_VERBS = {
    "heal", "hurt", "mp", "drain", "refill", "rupees", "tax", "bombs", "arrows",
    "smite", "clearscreen", "spawn", "swarm", "confuse", "flip", "party",
    "freeze", "stun", "curse", "root", "steal", "rob", "thief", "pickpocket",
    "speed", "fast", "slow", "ice", "icefloor", "illusion", "bunny", "fairy",
    "deny", "denyboots", "arise", "chicken", "cucco",
}
# world verbs are dropped by the game unless Link is in actual gameplay
WORLD_VERBS = {"smite", "clearscreen", "spawn", "swarm", "fairy", "arise", "chicken", "cucco"}
# vs_mode=1 in twitch_config.txt blocks these "helpful" verbs
VS_BLOCKED = {"heal", "mp", "refill", "rupees", "bombs", "arrows"}

MOCK_TOKEN = "mock-token-12345"
MOCK_CLIENT_ID = "mock-client-id-67890"
MOCK_CHANNEL = "mockcaster"  # offline fixture identity for --mock, not a real account
MOCK_WS_ENV = "CP_MOCK_WS_PORT"    # default 8932, settable to match test_server_mock.py
MOCK_HTTP_ENV = "CP_MOCK_HTTP_PORT"  # default 8931


def log(msg):
    print(time.strftime("[%H:%M:%S]") + " [cp] " + msg, flush=True)


# ------------------------------------------------------------------ RFC6455 --

class ConnClosed(Exception):
    def __init__(self, code=None, reason=""):
        super().__init__("websocket closed code=%s %s" % (code, reason))
        self.code = code


class WsClient:
    """Minimal RFC6455 websocket client over (optionally TLS) TCP.

    Client->server frames are always masked (RFC requirement).  Server pings
    are answered with pongs inside recv_message(); nothing else is ever sent,
    which is exactly what EventSub demands (4001 otherwise).
    """

    def __init__(self, url, timeout=15.0):
        m = re.match(r"^(wss?)://([^/:]+)(?::(\d+))?(/.*)?$", url.strip())
        if not m:
            raise ValueError("bad websocket url: %r" % url)
        self.use_tls = m.group(1) == "wss"
        self.host = m.group(2)
        self.port = int(m.group(3) or (443 if self.use_tls else 80))
        self.path = m.group(4) or "/"
        self.timeout = timeout
        self.sock = None
        self._buf = b""

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.use_tls:
            ctx = ssl.create_default_context()
            self.sock = ctx.wrap_socket(self.sock, server_hostname=self.host)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: zelda3-cp-listen/%s\r\n\r\n"
            % (self.path, self.host, self.port, key, VERSION)
        )
        self.sock.sendall(req.encode("ascii"))
        # read the 101 response headers
        while b"\r\n\r\n" not in self._buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnClosed(reason="handshake: connection dropped")
            self._buf += chunk
        head, self._buf = self._buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        if " 101 " not in lines[0]:
            raise ConnClosed(reason="handshake rejected: %s" % lines[0])
        accept = None
        for ln in lines[1:]:
            if ln.lower().startswith("sec-websocket-accept:"):
                accept = ln.split(":", 1)[1].strip()
        want = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        if accept != want:
            raise ConnClosed(reason="handshake: bad Sec-WebSocket-Accept")
        return self

    def _send_frame(self, opcode, payload=b""):
        assert self.sock is not None
        mask = os.urandom(4)
        n = len(payload)
        b0 = 0x80 | opcode  # FIN + opcode
        if n < 126:
            header = struct.pack("!BB", b0, 0x80 | n)
        elif n < 65536:
            header = struct.pack("!BBH", b0, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", b0, 0x80 | 127, n)
        data = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        self.sock.sendall(header + mask + data)

    def _recv_exact(self, n):
        while len(self._buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnClosed(reason="connection dropped mid-frame")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _recv_frame(self):
        b0, b1 = self._recv_exact(2)
        fin = bool(b0 & 0x80)
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        ln = b1 & 0x7F
        if ln == 126:
            (ln,) = struct.unpack("!H", self._recv_exact(2))
        elif ln == 127:
            (ln,) = struct.unpack("!Q", self._recv_exact(8))
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(ln) if ln else b""
        if mask:
            payload = bytes(c ^ mask[i % 4] for i, c in enumerate(payload))
        return fin, opcode, payload

    def recv_message(self):
        """Returns (opcode, bytes).  Auto-pongs pings, ignores pongs,
        assembles fragmented messages, raises ConnClosed on close frames."""
        fragments = []
        frag_op = None
        while True:
            fin, opcode, payload = self._recv_frame()
            if opcode == 0x9:  # ping -> pong (the ONLY thing we may send)
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:  # unsolicited pong: ignore
                continue
            if opcode == 0x8:  # close
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else None
                try:
                    self._send_frame(0x8, payload[:2])
                except OSError:
                    pass
                raise ConnClosed(code)
            if opcode in (0x1, 0x2):
                if fin:
                    return opcode, payload
                frag_op, fragments = opcode, [payload]
            elif opcode == 0x0:
                fragments.append(payload)
                if fin:
                    return frag_op, b"".join(fragments)

    def send_text(self, s):
        self._send_frame(0x1, s.encode("utf-8"))

    def close(self):
        try:
            if self.sock is not None:
                self.sock.close()
        except OSError:
            pass
        finally:
            self.sock = None
            self._buf = b""


# ------------------------------------------------------------------ config --

def parse_kv_file(path):
    """Parse twitch_config.txt the same way src/twitch.c does: key=value,
    #/;/ comments, 'oauth:' prefix stripped from token, user/channel lowered."""
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if not s or s[0] in "#/;":
                    continue
                eq = s.find("=")
                if eq < 0:
                    continue
                key = s[:eq].strip().lower()
                val = s[eq + 1:].strip()
                if key == "token" and val.lower().startswith("oauth:"):
                    val = val[6:]
                if key in ("user", "nick", "channel"):
                    val = val.lower().lstrip("#")
                    if key == "nick":
                        key = "user"
                out[key] = val
    except OSError:
        pass
    return out


def find_project_root():
    """tools/channel_points/cp_listen.py -> zelda3 project root (two up)."""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    if os.path.isfile(os.path.join(root, "twitch_config.txt")) or \
       os.path.isdir(os.path.join(root, "twitch_drop")):
        return root
    return os.getcwd()


def first(*vals):
    for v in vals:
        if v:
            return v
    return ""


def validate_login(token):
    """Which Twitch account does this token belong to?  Asks
    id.twitch.tv/oauth2/validate (safe endpoint: returns login/scopes/
    expiry).  Returns "" when it cannot be determined - never raises.
    The token itself is never logged."""
    if not token:
        return ""
    req = urllib.request.Request(
        "https://id.twitch.tv/oauth2/validate",
        headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
            return (data.get("login") or "").strip()
    except Exception:
        return ""


class Cfg(object):
    pass


def load_cfg(args):
    root = find_project_root()
    cfg = Cfg()
    cfg.root = root
    cfg.mock = args.mock

    # cp_config.ini: client_id / token / login / optional overrides
    # (see cp_config.ini.example for the full annotated set)
    ini = configparser.ConfigParser()
    ini_path = os.path.join(root, "tools", "channel_points", "cp_config.ini")
    have_ini = bool(ini.read(ini_path, encoding="utf-8"))

    def ini_get(section, key, default=""):
        if have_ini and ini.has_option(section, key):
            return ini.get(section, key, fallback=default).strip()
        return default

    game = parse_kv_file(os.path.join(root, "twitch_config.txt"))

    cfg.channel = first(args.channel,
                        ini_get("auth", "channel"),
                        game.get("channel"))
    cfg.token = first(args.token,
                      ini_get("auth", "token"),
                      game.get("token"))
    cfg.client_id = first(args.client_id,
                          ini_get("eventsub", "client_id"))
    # refresh support: where cp_refresh writes the renewed token back to, and
    # when the current one dies ([auth] expires_at, unix seconds; 0 = unknown)
    cfg.ini_path = ini_path
    _ea = ini_get("auth", "expires_at")
    cfg.expires_at = int(_ea) if _ea.isdigit() else 0

    # Account-agnostic channel resolution.  An empty channel NEVER falls back
    # to a hardcoded name: it means "the account that authorized the token".
    # [auth] login= (written by cp_auth.py after validation) is the cheap
    # source; otherwise derive it from token validation - but only when
    # network use is allowed (--mock / --check must stay fully offline).
    cfg.token_login = ini_get("auth", "login")
    offline = bool(getattr(args, "mock", False) or getattr(args, "check", False))
    if not cfg.token_login and cfg.token and not offline:
        cfg.token_login = validate_login(cfg.token)
    if not cfg.channel and cfg.token_login:
        cfg.channel = cfg.token_login
        log("channel empty -> using the token's own account %r "
            "(from cp_config.ini [auth] login / token validation)"
            % cfg.token_login)
    if (cfg.channel and cfg.token_login
            and cfg.channel.strip().casefold() != cfg.token_login.casefold()):
        log("=" * 74)
        log("WARNING: channel %r does not match the token's account %r."
            % (cfg.channel, cfg.token_login))
        log("  Redemptions would be watched on %r using a token that belongs"
            " to %r." % (cfg.channel, cfg.token_login))
        log("  EventSub answers 403 (missing authorization) unless the"
            " token's account owns the channel.")
        log("  Fix: re-run cp_auth.py signed in AS the channel owner, or"
            " clear [auth] channel so it follows the token.")
        log("=" * 74)

    cfg.types_raw = first(ini_get("eventsub", "types"), "auto,custom")
    cfg.wss_url = first(args.wss, ini_get("eventsub", "wss_url"), DEFAULT_WSS_URL)
    cfg.helix_base = first(args.helix, ini_get("eventsub", "helix_base"), DEFAULT_HELIX_BASE)

    map_default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cp_map.ini")
    cfg.map_path = first(args.map, ini_get("run", "map"), map_default)
    drop_default = os.path.join(root, "twitch_drop")
    cfg.drop_dir = first(args.drop_dir, ini_get("drop", "dir"), drop_default)
    cfg.log_unknown = ini_get("run", "log_unknown", "true").lower() not in ("0", "false", "no")

    if cfg.mock:
        # fully offline: fixed creds (mock server enforces them), plain-HTTP
        # Helix on localhost, plain-WS EventSub on localhost, scratch drop dir
        cfg.token = args.token or MOCK_TOKEN
        cfg.client_id = args.client_id or MOCK_CLIENT_ID
        cfg.channel = args.channel or MOCK_CHANNEL
        ws_port = int(os.environ.get(MOCK_WS_ENV, "8932"))
        http_port = int(os.environ.get(MOCK_HTTP_ENV, "8931"))
        cfg.wss_url = args.wss or "ws://127.0.0.1:%d/ws" % ws_port
        cfg.helix_base = args.helix or "http://127.0.0.1:%d/helix" % http_port
        if not args.drop_dir:
            cfg.drop_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_out")

    # which subscription types to create
    cfg.sub_types = []
    for t in cfg.types_raw.replace(";", ",").split(","):
        t = t.strip().lower()
        if t == "auto":
            cfg.sub_types.append(SUB_AUTO)
        elif t == "custom":
            cfg.sub_types.append(SUB_CUSTOM)
        elif t in (SUB_AUTO, SUB_CUSTOM):
            cfg.sub_types.append(t)
    if not cfg.sub_types:
        cfg.sub_types = [SUB_AUTO, SUB_CUSTOM]
    return cfg


# --------------------------------------------------------------------- map --

class Mapping(object):
    """reward name -> (verb, arg_template, dur_frames)."""

    def __init__(self, path):
        self.path = path
        self.table = {}
        cp = configparser.ConfigParser()
        read = cp.read(path, encoding="utf-8")
        if not read:
            raise FileNotFoundError("map file not found: %s" % path)
        for key, val in cp.items("map") if cp.has_section("map") else []:
            parsed = self.parse_value(key, val)
            if parsed:
                self.table[self.norm(key)] = parsed

    @staticmethod
    def norm(name):
        return " ".join((name or "").split()).casefold()

    def parse_value(self, key, val):
        # grammar:  verb [arg words...] [@frames]
        #   "arise"            -> verb only, default duration
        #   "spawn raven 2"    -> verb + arg
        #   "confuse @600"     -> explicit duration in FRAMES (game unit)
        #   "hurt {input}"     -> {input} = the redeemer's message text
        #   "-"                -> explicitly disabled
        val = val.strip()
        if not val or val == "-":
            return None
        dur = 0
        m = re.search(r"@(\d+)\s*$", val)
        if m:
            dur = int(m.group(1))
            val = val[:m.start()].strip()
        parts = val.split()
        verb = parts[0].lower()
        arg = " ".join(parts[1:])
        if verb not in KNOWN_VERBS:
            log("WARNING map: '%s' -> unknown verb '%s' (game will ignore it)"
                " - valid verbs: %s" % (key, verb, " ".join(sorted(KNOWN_VERBS))))
        if verb in VS_BLOCKED:
            log("WARNING map: '%s' -> verb '%s' is blocked when vs_mode=1" % (key, verb))
        if verb in WORLD_VERBS:
            log("NOTE map: '%s' -> verb '%s' only fires during actual gameplay"
                " (world verb)" % (key, verb))
        return (verb, arg, dur)

    def lookup(self, reward_name):
        return self.table.get(self.norm(reward_name))


# ------------------------------------------------------------------- helix --

class HelixError(Exception):
    def __init__(self, status, message):
        super().__init__("Helix %s: %s" % (status, message))
        self.status = status


def helix(method, path, cfg, query=None, body=None):
    url = cfg.helix_base.rstrip("/") + path
    if query:
        url += "?" + urllib.parse.urlencode(query)
    data = None
    headers = {
        "Authorization": "Bearer " + cfg.token,
        "Client-Id": cfg.client_id,
    }
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read().decode("utf-8", "replace")
            return resp.status, (json.loads(raw) if raw.strip() else {})
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            msg = json.loads(raw)
            msg = msg.get("message") or raw
        except ValueError:
            msg = raw
        raise HelixError(e.code, msg)


def resolve_broadcaster(cfg):
    status, data = helix("GET", "/users", cfg, query={"login": cfg.channel})
    users = data.get("data") or []
    if not users:
        raise HelixError(status, "no such channel: %r" % cfg.channel)
    return users[0]["id"], users[0].get("display_name", cfg.channel)


def create_subscription(cfg, session_id, sub_type, broadcaster_id):
    body = {
        "type": sub_type,
        "version": SUB_VERSION,
        "condition": {"broadcaster_user_id": broadcaster_id},
        "transport": {"method": "websocket", "session_id": session_id},
    }
    status, data = helix("POST", "/eventsub/subscriptions", cfg, body=body)
    return data


# ------------------------------------------------------------------ events --

def sanitize_field(s, limit):
    """Drop-file fields are '|' separated; a stray '|' would shift every
    field in the game's positional parser (src/twitch.c TwitchPollDrop)."""
    s = re.sub(r"[\r\n\x00-\x1f|]+", " ", s or "")
    return s.strip()[:limit]


def extract_redemption(sub_type, event):
    """Returns (reward_name, user_input, who, display, cost).

    Payload shapes differ (verified against dev.twitch.tv 2026-09-09):
      custom    v1: reward{title, cost, prompt}, user_input, status
      automatic v1: reward{type, cost, unlocked_emote}  <- NO human name!
                    message.text carries the highlighted message
    We accept every plausible alias so a Twitch payload tweak degrades to
    'unknown reward, ignored' instead of a crash.
    """
    reward = event.get("reward") or {}
    if "automatic" in sub_type:
        name = reward.get("name") or reward.get("title") or reward.get("type") or ""
        msg = event.get("message") or {}
        user_input = msg.get("text") if isinstance(msg, dict) else None
        user_input = user_input or event.get("user_input") or ""
    else:
        name = reward.get("title") or reward.get("name") or ""
        user_input = event.get("user_input")
        if user_input is None:
            msg = event.get("message") or {}
            user_input = msg.get("text") if isinstance(msg, dict) else ""
    who = event.get("user_login") or event.get("user_name") or "anon"
    display = event.get("user_name") or who
    return name, user_input or "", who, display, reward.get("cost")


def render_arg(template, user_input):
    if "{input}" not in template:
        return template
    return template.replace("{input}", sanitize_field(user_input, 40))


class Stats(object):
    def __init__(self):
        self.received = 0
        self.mapped = 0
        self.ignored = 0
        self.files = []


def write_drop(drop_dir, verb, arg, who, dur, reward_name):
    os.makedirs(drop_dir, exist_ok=True)
    slug = re.sub(r"[^a-z0-9]+", "_", reward_name.lower()).strip("_")[:32] or "reward"
    fname = "cp_%d_%s.txt" % (int(time.time() * 1000), slug)
    full = os.path.join(drop_dir, fname)
    tmp = full + ".tmp"  # game scans *.txt only -> half-writes are invisible
    line = "%s|%s|%s|%d\n" % (verb[:23], sanitize_field(arg, 60), sanitize_field(who, 63), dur)
    with open(tmp, "w", encoding="ascii", errors="replace") as f:
        f.write(line)
    os.replace(tmp, full)  # atomic: the game never sees a partial file
    return full, line.strip()


def handle_notification(sub_type, event, mapping, cfg, stats):
    stats.received += 1
    name, user_input, who, display, cost = extract_redemption(sub_type, event)
    log("redemption: reward=%r user=%s (display %s) cost=%s input=%r type=%s"
        % (name, who, display, cost, user_input, "auto" if "automatic" in sub_type else "custom"))
    hit = mapping.lookup(name) if name else None
    if not hit:
        stats.ignored += 1
        if cfg.log_unknown:
            log("IGNORED: no mapping for reward %r (add it to %s to give it an effect)"
                % (name, os.path.basename(cfg.map_path)))
        return
    verb, template, dur = hit
    arg = render_arg(template, user_input)
    full, line = write_drop(cfg.drop_dir, verb, arg, who, dur, name)
    stats.mapped += 1
    stats.files.append(full)
    log("DROP: %s  (%s redeemed %r)" % (line, who, name))


# ---------------------------------------------------------------- refresh --

def apply_refresh(cfg, result):
    """Copy a successful cp_refresh.refresh_config() result into the live cfg
    (token bytes never touch the log; safe fields do)."""
    cfg.token = result.get("token") or cfg.token
    try:
        cfg.expires_at = int(result.get("expires_at") or 0)
    except (TypeError, ValueError):
        pass
    if result.get("login"):
        if not cfg.token_login:
            cfg.token_login = result["login"]
        if not cfg.channel:
            cfg.channel = cfg.token_login
            log("channel empty -> using the refreshed token's account %r"
                % cfg.channel)


def maybe_startup_refresh(cfg):
    """Real mode, once before connecting: when the stored token is expired or
    dies within cp_refresh.REFRESH_MARGIN (10 min) and cp_config.ini holds a
    refresh_token, mint a fresh token - no browser - so a ~4-hour-old token
    never blocks a stream.  A missing token is also rescued when only a
    refresh_token exists.  Failure here is NOT fatal (the token might still
    validate, or the network hiccup pass); it logs loudly and lets the run
    proceed - the mid-run Helix-401 handler in main() gets one more try.
    Returns True when cfg.token changed."""
    if cfg.mock:
        return False
    stale = bool(cfg.token and cfg.expires_at
                 and cp_refresh.is_stale(cfg.expires_at))
    rescue = not cfg.token  # no token at all: a refresh_token alone can mint one
    if not stale and not rescue:
        if cfg.expires_at:
            log("token: still fresh for ~%.1f h (no refresh needed)"
                % max(0.0, (cfg.expires_at - time.time()) / 3600.0))
        return False
    if not cp_refresh.has_refresh_token(cfg.ini_path):
        if stale:
            log("WARNING: token is stale (expired_at %s local) and "
                "cp_config.ini holds no refresh_token - cannot auto-renew."
                % time.strftime("%Y-%m-%d %H:%M:%S",
                                time.localtime(cfg.expires_at)))
            log(cp_refresh.REAUTH_MESSAGE)
        return False
    log("token stale (expired or < %d min left%s) - refreshing via the stored "
        "refresh_token (no browser needed)..."
        % (cp_refresh.REFRESH_MARGIN // 60,
           "" if cfg.token else ", or missing entirely"))
    result = cp_refresh.refresh_config(cfg.ini_path)
    if not result.get("ok"):
        log("WARNING: automatic refresh failed (%s): %s"
            % (result.get("reason"), result.get("error", "")))
        if result.get("reason") != "network":
            log(cp_refresh.REAUTH_MESSAGE)
        return False
    apply_refresh(cfg, result)
    log("refresh OK: login=%s, expires in ~%.1f h%s"
        % (result.get("login") or "?",
           max(0.0, (cfg.expires_at - time.time()) / 3600.0),
           "; refresh_token rotated" if result.get("rotated") else ""))
    return True


# ----------------------------------------------------------------- session --

def read_event(ws, deadline_secs, on_keepalive=None):
    """Reads one JSON EventSub message (skipping pings/keeps the watchdog
    fed).  Raises TimeoutError if nothing arrives within deadline_secs."""
    ws.sock.settimeout(deadline_secs)
    opcode, payload = ws.recv_message()
    if opcode != 0x1:
        return {}
    return json.loads(payload.decode("utf-8", "replace"))


def run_session(cfg, mapping, stats):
    """One websocket session.  Returns:
      ("reconnect", url)  graceful server-directed reconnect (subs carry over)
      ("closed", None)    dropped without notice (fresh reconnect must re-subscribe)
      ("revoked", status) subscription revoked - stop until re-authorized
    """
    ws = WsClient(cfg.wss_url, timeout=20.0)
    ws.connect()
    log("websocket connected: %s" % cfg.wss_url)

    # session_welcome must be the first message; we have ~10s after it to
    # create subscriptions (close code 4003 otherwise).
    welcome = None
    try:
        while welcome is None:
            msg = read_event(ws, 20.0)
            mtype = (msg.get("metadata") or {}).get("message_type")
            if mtype == "session_welcome":
                welcome = msg
            elif mtype in ("session_keepalive",):
                continue
            else:
                log("unexpected pre-welcome message type=%r" % mtype)
    except (TimeoutError, socket.timeout):
        ws.close()
        return ("closed", None)

    session = welcome.get("payload", {}).get("session", {})
    session_id = session.get("id", "")
    keepalive_secs = int(session.get("keepalive_timeout_seconds") or 10)
    log("session %s (keepalive %ds)" % (session_id[:16] + "...", keepalive_secs))

    broadcaster_id, broadcaster_display = resolve_broadcaster(cfg)
    log("channel: %s (id %s)" % (broadcaster_display, broadcaster_id))

    for sub_type in cfg.sub_types:
        try:
            sub = create_subscription(cfg, session_id, sub_type, broadcaster_id)
            log("subscribed: %s (cost %s, status %s)"
                % (sub_type, sub.get("cost", "?"), sub.get("status", "?")))
        except HelixError as e:
            if e.status == 409:
                log("already subscribed: %s (ok)" % sub_type)
            elif e.status == 401:
                ws.close()
                if cfg.mock:
                    raise SystemExit(
                        "FATAL 401 on %s - the mock server enforces Bearer %r "
                        "/ Client-Id %r (see test_server_mock.py)."
                        % (sub_type, MOCK_TOKEN, MOCK_CLIENT_ID))
                raise  # real mode: main() retries ONCE with a refreshed token
            elif e.status == 403:
                ws.close()
                raise SystemExit(
                    "FATAL 403 on %s - token lacks scope "
                    "channel:read:redemptions.\n"
                    "  A refresh cannot ADD a missing scope: re-run cp_auth.py\n"
                    "  and click Authorize (see README.md)." % sub_type)
            else:
                log("WARNING: subscribe failed for %s: %s" % (sub_type, e))
    log("listening for channel point redemptions... (ctrl+c to stop)")

    last_msg = time.time()
    try:
        while True:
            try:
                msg = read_event(ws, keepalive_secs + 5.0)
            except (TimeoutError, socket.timeout):
                log("keepalive timeout (%ds silent) - reconnecting" % (keepalive_secs + 5))
                return ("closed", None)
            last_msg = time.time()
            meta = msg.get("metadata") or {}
            mtype = meta.get("message_type")
            payload = msg.get("payload") or {}
            if mtype == "session_keepalive":
                continue
            if mtype == "session_reconnect":
                url = (payload.get("session") or {}).get("reconnect_url") or cfg.wss_url
                log("server-directed reconnect")
                ws.close()
                return ("reconnect", url)
            if mtype == "revocation":
                status = (payload.get("subscription") or {}).get("status", "?")
                log("SUBSCRIPTION REVOKED: %s - re-authorize and restart" % status)
                ws.close()
                return ("revoked", status)
            if mtype == "notification":
                sub = payload.get("subscription") or {}
                event = payload.get("event") or {}
                handle_notification(sub.get("type", ""), event, mapping, cfg, stats)
                continue
            log("unhandled message_type=%r" % mtype)
    except ConnClosed as e:
        log("websocket closed: %s" % e)
        return ("closed", None)
    except json.JSONDecodeError as e:
        log("bad json from server (%s) - ignoring message" % e)
        return ("closed", None)
    finally:
        ws.close()


# -------------------------------------------------------------------- main --

def do_check(cfg, mapping):
    ok = True
    log("project root : %s" % cfg.root)
    log("map file     : %s (%d mappings)" % (cfg.map_path, len(mapping.table)))
    for k in sorted(mapping.table):
        log("  %-34r -> %s" % (k, mapping.table[k]))
    log("drop dir     : %s %s" % (cfg.drop_dir,
                                   "(exists)" if os.path.isdir(cfg.drop_dir) else "(created on first drop)"))
    if cfg.mock:
        log("mode         : MOCK (ws %s / helix %s)" % (cfg.wss_url, cfg.helix_base))
        log("credentials  : mock - no real token needed")
        return
    log("channel      : %s%s" % (cfg.channel or "MISSING",
                                 " (= the token's own account)" if cfg.channel == cfg.token_login else ""))
    log("token account: %s" % (cfg.token_login
                                or "unknown (--check is offline; a real run derives it from the token)"))
    cid = (cfg.client_id[:8] + "...") if cfg.client_id else \
        "MISSING (register an app at dev.twitch.tv/console - see README.md)"
    log("client_id    : %s" % cid)
    log("token        : %s" % ("present (%d chars)" % len(cfg.token) if cfg.token
                                 else "MISSING (twitch_config.txt token is empty)"))
    log("subscriptions: %s" % ", ".join(cfg.sub_types))
    if not cfg.token or not cfg.client_id:
        ok = False
        log("CHECK FAILED: real mode needs client_id + a user token with")
        log("  channel:read:redemptions in cp_config.ini - see README.md.")
    else:
        log("CHECK OK - run:  python cp_listen.py")
    if not ok:
        raise SystemExit(2)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Twitch Channel Points -> zelda3 drop-file bridge")
    ap.add_argument("--mock", action="store_true",
                    help="offline test against test_server_mock.py (no real token, writes mock_out/)")
    ap.add_argument("--once", action="store_true",
                    help="one session then exit (implied by --mock)")
    ap.add_argument("--check", action="store_true", help="validate config + map, no network")
    ap.add_argument("--channel")
    ap.add_argument("--token")
    ap.add_argument("--client-id")
    ap.add_argument("--map", help="override cp_map.ini path")
    ap.add_argument("--drop-dir", help="override twitch_drop/ output dir")
    ap.add_argument("--wss", help="override EventSub websocket url")
    ap.add_argument("--helix", help="override Helix API base url")
    args = ap.parse_args(argv)

    print("zelda3 cp_listen v%s - channel points bridge (stdlib-only prototype)" % VERSION,
          flush=True)
    cfg = load_cfg(args)
    try:
        mapping = Mapping(cfg.map_path)
    except FileNotFoundError as e:
        raise SystemExit("FATAL %s" % e)
    log("loaded %d reward mappings from %s" % (len(mapping.table), os.path.basename(cfg.map_path)))

    if args.check:
        do_check(cfg, mapping)
        return

    refresh_guard_mono = 0.0  # monotonic ts of the last token refresh
    if cfg.mock:
        args.once = True
        log("MOCK MODE: drop files go to %s (NOT the game's twitch_drop/)" % cfg.drop_dir)
    else:
        # Refresh the token BEFORE connecting if it is stale (real mode only;
        # --mock/--check stay fully offline).  A success here also fills
        # token/channel for the missing-credentials check below.
        if maybe_startup_refresh(cfg):
            refresh_guard_mono = time.monotonic()

    if not cfg.mock and not (cfg.token and cfg.client_id and cfg.channel):
        raise SystemExit(
            "FATAL: missing credentials for real mode.\n"
            "  channel=%r token=%s client_id=%s\n"
            "  Fix: cp_config.ini must hold client_id + a user token with\n"
            "  channel:read:redemptions - run cp_auth.py, it fills everything\n"
            "  in (token, login, channel).  Exact steps: README.md runbook.\n"
            "  Note: channel may be empty in the config - a real run derives\n"
            "  it from the token automatically (--check stays offline).\n"
            "  (Or run the offline test:  python cp_listen.py --mock)"
            % (cfg.channel, "yes" if cfg.token else "NO", "yes" if cfg.client_id else "NO"))

    stats = Stats()
    backoff = 0.0
    reconnect_url = None
    while True:
        if reconnect_url:
            cfg.wss_url = reconnect_url  # subscriptions carry over on this path
        try:
            reason, url = run_session(cfg, mapping, stats)
        except KeyboardInterrupt:
            log("bye (%d events: %d mapped -> drop files, %d ignored)"
                % (stats.received, stats.mapped, stats.ignored))
            return
        except SystemExit:
            raise
        except HelixError as e:
            if e.status != 401 or cfg.mock:
                log("session error: %r" % e)
                reason, url = ("closed", None)
            elif time.monotonic() - refresh_guard_mono < 120:
                # fresh token yet still 401: the remaining suspect is the app
                raise SystemExit(
                    "FATAL: Helix 401 AGAIN right after a successful token "
                    "refresh.\n"
                    "  The token is fresh, so the remaining suspect is "
                    "[eventsub] client_id:\n"
                    "  it must belong to the SAME dev app that minted the "
                    "token.\n"
                    "  Fix cp_config.ini, then re-run cp_auth.py.")
            else:
                log("Helix 401 mid-run - attempting ONE automatic refresh...")
                result = cp_refresh.refresh_config(cfg.ini_path)
                if not result.get("ok"):
                    log("REFRESH FAILED: %s"
                        % (result.get("error") or result.get("reason")))
                    raise SystemExit(
                        "FATAL: Helix 401 and the automatic refresh failed "
                        "(%s).\n"
                        "  %s" % (result.get("reason"),
                                  cp_refresh.REAUTH_MESSAGE))
                apply_refresh(cfg, result)
                refresh_guard_mono = time.monotonic()
                reconnect_url = None  # fresh session with the new token
                backoff = 0.0
                log("refresh OK: login=%s, expires in ~%.1f h%s - "
                    "reconnecting with the new token"
                    % (result.get("login") or "?",
                       max(0.0, (cfg.expires_at - time.time()) / 3600.0),
                       "; refresh_token rotated" if result.get("rotated") else ""))
                continue
        except Exception as e:
            log("session error: %r" % e)
            reason, url = ("closed", None)

        if args.once:
            log("session over - once mode, exiting. stats: %d received, %d mapped, %d ignored"
                % (stats.received, stats.mapped, stats.ignored))
            for p in stats.files:
                log("wrote: %s" % p)
            return

        if reason == "revoked":
            raise SystemExit(3)
        if reason == "reconnect" and url:
            reconnect_url = url
            backoff = 0.0  # grace: server expects us back within ~30s
            continue
        reconnect_url = None  # fresh session -> subscriptions must be re-created
        backoff = min(60.0, backoff * 2 or 1.0)
        log("reconnecting in %.0fs (fresh session: subscriptions will be re-created)" % backoff)
        try:
            time.sleep(backoff)
        except KeyboardInterrupt:
            log("bye")
            return


if __name__ == "__main__":
    main()

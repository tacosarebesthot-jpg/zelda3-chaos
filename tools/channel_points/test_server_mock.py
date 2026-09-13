#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""test_server_mock.py - offline double for Twitch, for testing cp_listen.py.

Serves, on localhost only:
  HTTP :8931  fake Helix API
               GET  /helix/users?login=...                     (id lookup)
               POST /helix/eventsub/subscriptions               (websocket transport)
               - enforces Authorization: Bearer mock-token-12345
               - enforces Client-Id: mock-client-id-67890
               - rejects missing session_id with 400 (like real Helix)
  WS   :8932  fake EventSub websocket
               session_welcome -> 5 scripted redemptions -> close(1000)

Run (terminal 1):
  python tools/channel_points/test_server_mock.py
Run (terminal 2):
  python tools/channel_points/cp_listen.py --mock

Expected result: 3 drop files in tools/channel_points/mock_out/
  arise||chickenchaser|0            (from "ARISE Chicken!! (ATHF)")
  spawn|raven 2|duckman42|0         (from "Duck Vanishes")
  tax|50|coin_gremlin|0             (from "So Disappointing", user_input)
and 2 IGNORED log lines (unmapped custom reward + automatic reward type).
No real token, no Twitch traffic, nothing written to the game's twitch_drop/.

Ports follow env CP_MOCK_HTTP_PORT / CP_MOCK_WS_PORT (cp_listen --mock reads
the same variables). Python stdlib only.
"""

import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from socketserver import BaseRequestHandler, ThreadingTCPServer

HOST = "127.0.0.1"
HTTP_PORT = int(os.environ.get("CP_MOCK_HTTP_PORT", "8931"))
WS_PORT = int(os.environ.get("CP_MOCK_WS_PORT", "8932"))

MOCK_TOKEN = "mock-token-12345"
MOCK_CLIENT_ID = "mock-client-id-67890"
MOCK_CHANNEL_ID = "987654321"
MOCK_CHANNEL_LOGIN = "mockcaster"  # offline fixture identity, not a real account
SESSION_ID = "MOCK-SESSION-0001-abcdef0123456789"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

VERBOSE = True


def vlog(*a):
    if VERBOSE:
        print("[mock]", *a, flush=True)


# ------------------------------------------------------------- RFC6455 out --

def ws_accept(key):
    return base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")


def ws_send_frame(sock, opcode, payload=b""):
    # server->client frames are never masked
    n = len(payload)
    b0 = 0x80 | opcode
    if n < 126:
        header = struct.pack("!BB", b0, n)
    elif n < 65536:
        header = struct.pack("!BBH", b0, 126, n)
    else:
        header = struct.pack("!BBQ", b0, 127, n)
    sock.sendall(header + payload)


def ws_send_json(sock, obj):
    ws_send_frame(sock, 0x1, json.dumps(obj).encode("utf-8"))


def ws_recv_frame(sock):
    """Returns (opcode, payload); unmasks client frames."""
    hdr = b""
    while len(hdr) < 2:
        c = sock.recv(2 - len(hdr))
        if not c:
            raise ConnectionError("client dropped")
        hdr += c
    b0, b1 = hdr
    masked = bool(b1 & 0x80)
    ln = b1 & 0x7F
    if ln == 126:
        ext = b""
        while len(ext) < 2:
            c = sock.recv(2 - len(ext))
            if not c:
                raise ConnectionError("client dropped")
            ext += c
        (ln,) = struct.unpack("!H", ext)
    elif ln == 127:
        ext = b""
        while len(ext) < 8:
            c = sock.recv(8 - len(ext))
            if not c:
                raise ConnectionError("client dropped")
            ext += c
        (ln,) = struct.unpack("!Q", ext)
    mask = b""
    if masked:
        while len(mask) < 4:
            c = sock.recv(4 - len(mask))
            if not c:
                raise ConnectionError("client dropped")
            mask += c
    payload = b""
    while len(payload) < ln:
        c = sock.recv(min(4096, ln - len(payload)))
        if not c:
            raise ConnectionError("client dropped")
        payload += c
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return b0 & 0x0F, payload


# ------------------------------------------------------------- fake Helix --

class FakeHelix(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # silence default noisy logging
        pass

    def _check_auth(self):
        auth = self.headers.get("Authorization", "")
        cid = self.headers.get("Client-Id", "")
        if auth != "Bearer " + MOCK_TOKEN:
            self._json(401, {"error": "Unauthorized", "status": 401,
                             "message": "invalid access token passed as bearer"})
            return False
        if cid != MOCK_CLIENT_ID:
            self._json(401, {"error": "Unauthorized", "status": 401,
                             "message": "client_id did not match the token's application"})
            return False
        return True

    def _json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self._check_auth():
            return
        if self.path.startswith("/helix/channel_points/custom_rewards"):
            # the channel's rewards, as the in-game listener lists them at boot
            vlog("HTTP GET %s -> 4 rewards" % self.path)
            self._json(200, {"data": [
                {"id": "r-1", "title": "ARISE  Chicken!! (ATHF)", "cost": 100},
                {"id": "r-2", "title": "Duck Vanishes", "cost": 200},
                {"id": "r-3", "title": "So Disappointing", "cost": 40},
                {"id": "r-4", "title": "Totally Unmapped Reward", "cost": 2},
            ]})
            return
        if self.path.startswith("/helix/users"):
            login = MOCK_CHANNEL_LOGIN
            if "login=" in self.path:
                login = self.path.split("login=", 1)[1].split("&", 1)[0]
            vlog("HTTP GET %s -> user id %s" % (self.path, MOCK_CHANNEL_ID))
            self._json(200, {"data": [{
                "id": MOCK_CHANNEL_ID,
                "login": MOCK_CHANNEL_LOGIN,
                "display_name": MOCK_CHANNEL_LOGIN.capitalize(),
                "type": "", "broadcaster_type": "partner",
                "description": "mock"}]})
            return
        self._json(404, {"error": "Not Found", "status": 404, "message": "mock has no " + self.path})

    def do_POST(self):
        if not self._check_auth():
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode("utf-8"))
        except ValueError:
            self._json(400, {"error": "Bad Request", "status": 400, "message": "bad json"})
            return
        if not self.path.startswith("/helix/eventsub/subscriptions"):
            self._json(404, {"error": "Not Found", "status": 404, "message": "mock has no " + self.path})
            return
        transport = body.get("transport") or {}
        if transport.get("method") != "websocket" or not transport.get("session_id"):
            vlog("HTTP POST subscriptions: REJECTED (bad transport: %s)" % transport)
            self._json(400, {"error": "Bad Request", "status": 400,
                             "message": "websocket subscriptions need transport.method=websocket"
                                        " + transport.session_id from session_welcome"})
            return
        sub = {
            "id": "mock-sub-" + body.get("type", "").split(".")[-1][:12],
            "type": body.get("type", "?"),
            "version": body.get("version", "1"),
            "status": "enabled",
            "cost": 0,  # broadcaster-authorized user-token subs cost 0
            "condition": body.get("condition", {}),
            "transport": {"method": "websocket", "session_id": transport["session_id"]},
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        vlog("HTTP POST subscriptions: 202 type=%s session=%s broadcaster=%s"
             % (sub["type"], transport["session_id"][:18] + "...",
                body.get("condition", {}).get("broadcaster_user_id")))
        self._json(202, sub)


# --------------------------------------------------------- fake EventSub --

def make_notification(seq, sub_type, event):
    return {
        "metadata": {
            "message_id": "mock-msg-%d" % seq,
            "message_type": "notification",
            "message_timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        },
        "payload": {
            "subscription": {
                "id": "mock-sub", "type": sub_type, "version": "1",
                "status": "enabled", "cost": 0,
                "condition": {"broadcaster_user_id": MOCK_CHANNEL_ID},
                "transport": {"method": "websocket", "session_id": SESSION_ID},
            },
            "event": event,
        },
    }


CUSTOM = "channel.channel_points_custom_reward_redemption.add"
AUTO = "channel.channel_points_automatic_reward_redemption.add"


def scripted_events():
    """5 redemptions: 3 should map, 2 must be ignored (unmapped name, and an
    automatic reward which carries only reward.type - v1 payload shape)."""
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    base = {"broadcaster_user_id": MOCK_CHANNEL_ID,
            "broadcaster_user_login": MOCK_CHANNEL_LOGIN,
            "broadcaster_user_name": MOCK_CHANNEL_LOGIN.capitalize(),
            "redeemed_at": now}
    return [
        # 1) custom, name with caps + punctuation + bonus spaces -> arise
        (CUSTOM, dict(base, id="evt-1", user_id="111", user_login="chickenchaser",
                      user_name="ChickenChaser", status="unfulfilled",
                      reward={"id": "r-1", "title": "ARISE  Chicken!! (ATHF)",
                              "cost": 100, "prompt": "become the chicken"},
                      user_input="")),
        # 2) custom, two-word arg -> spawn raven 2
        (CUSTOM, dict(base, id="evt-2", user_id="222", user_login="duckman42",
                      user_name="duckman42", status="unfulfilled",
                      reward={"id": "r-2", "title": "Duck Vanishes",
                              "cost": 50, "prompt": ""},
                      user_input="")),
        # 3) custom WITH user_input (proves input survives into arg field)
        (CUSTOM, dict(base, id="evt-3", user_id="333", user_login="coin_gremlin",
                      user_name="CoinGremlin", status="unfulfilled",
                      reward={"id": "r-3", "title": "So Disappointing",
                              "cost": 40, "prompt": "tax time"},
                      user_input="take my 50")),
        # 4) custom with NO mapping in cp_map.ini -> must be logged + ignored
        (CUSTOM, dict(base, id="evt-4", user_id="444", user_login="someone9",
                      user_name="someone9", status="unfulfilled",
                      reward={"id": "r-4", "title": "Totally Unmapped Reward",
                              "cost": 2, "prompt": ""},
                      user_input="")),
        # 5) AUTOMATIC redemption, real v1 payload shape: reward has type/cost,
        #    no title/name -> cp_map has it disabled -> logged + ignored
        (AUTO, dict(base, id="evt-5", user_id="555", user_login="lurker_lee",
                    user_name="LurkerLee",
                    reward={"type": "send_highlighted_message", "cost": 200},
                    message={"text": "poggers", "offset": 0})),
    ]


def ws_handler(conn, addr):
    try:
        conn.settimeout(30)
        # read the HTTP upgrade request
        buf = b""
        while b"\r\n\r\n" not in buf:
            c = conn.recv(4096)
            if not c:
                return
            buf += c
        head, _ = buf.split(b"\r\n\r\n", 1)
        headers = {}
        for ln in head.decode("latin-1").split("\r\n")[1:]:
            if ":" in ln:
                k, val = ln.split(":", 1)
                headers[k.strip().lower()] = val.strip()
        if headers.get("upgrade", "").lower() != "websocket":
            conn.close()
            return
        resp = ("HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n\r\n" % ws_accept(headers.get("sec-websocket-key", "")))
        conn.sendall(resp.encode("ascii"))
        vlog("WS client connected from %s" % (addr,))

        # 1) session_welcome (10s subscribe window is the real rule; the mock
        #    client subscribes right away so no timer needed)
        ws_send_json(conn, {
            "metadata": {"message_id": "mock-welcome", "message_type": "session_welcome",
                         "message_timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())},
            "payload": {"session": {
                "id": SESSION_ID, "status": "connected",
                "keepalive_timeout_seconds": 10,
                "reconnect_url": None,
                "connected_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            }},
        })
        vlog("WS sent session_welcome session=%s" % SESSION_ID)

        # reader thread: answer pings like the real server needs (and log
        # anything else - EventSub forbids client traffic beyond pongs)
        def reader():
            try:
                while True:
                    op, payload = ws_recv_frame(conn)
                    if op == 0x9:  # ping -> pong
                        ws_send_frame(conn, 0xA, payload)
                    elif op == 0x8:
                        return
                    else:
                        vlog("WS got unexpected client frame opcode=%d (real Twitch: 4001 close)" % op)
            except (ConnectionError, OSError):
                return
        rt = threading.Thread(target=reader, daemon=True)
        rt.start()

        # 2) scripted notifications
        for i, (sub_type, event) in enumerate(scripted_events(), 1):
            time.sleep(0.35)
            ws_send_json(conn, make_notification(i, sub_type, event))
            vlog("WS sent notification %d/5: %r (%s)"
                 % (i, (event.get("reward") or {}).get("title")
                    or (event.get("reward") or {}).get("type"), sub_type.split(".")[-2]))

        # 3) hold briefly, then close cleanly -> cp_listen --once exits
        time.sleep(1.2)
        ws_send_frame(conn, 0x8, struct.pack("!H", 1000))
        vlog("WS closed cleanly (1000)")
    except (ConnectionError, OSError) as e:
        vlog("WS handler ended: %s" % e)
    finally:
        try:
            conn.close()
        except OSError:
            pass


class WsMockHandler(BaseRequestHandler):
    """socketserver wants a BaseRequestHandler class, not a bare function."""

    def handle(self):
        ws_handler(self.request, self.client_address)


def main():
    httpd = ThreadingHTTPServer((HOST, HTTP_PORT), FakeHelix)
    class WS(ThreadingTCPServer):
        daemon_threads = True
        allow_reuse_address = True
    wsd = WS((HOST, WS_PORT), WsMockHandler)
    print("[mock] fake Helix      : http://%s:%d/helix  (requires Client-Id %s)"
          % (HOST, HTTP_PORT, MOCK_CLIENT_ID))
    print("[mock] fake EventSub WS: ws://%s:%d/ws   (token %s)" % (HOST, WS_PORT, MOCK_TOKEN))
    print("[mock] READY - now run:  python cp_listen.py --mock   (ctrl+c to stop)", flush=True)
    try:
        t1 = threading.Thread(target=httpd.serve_forever, daemon=True)
        t2 = threading.Thread(target=wsd.serve_forever, daemon=True)
        t1.start(); t2.start()
        while t1.is_alive() and t2.is_alive():
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[mock] bye", flush=True)
    finally:
        httpd.shutdown()
        wsd.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())

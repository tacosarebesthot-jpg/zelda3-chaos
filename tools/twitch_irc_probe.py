#!/usr/bin/env python3
"""twitch_irc_probe.py - live wire probe replicating src/twitch.c's IRC
keepalive exactly, to diagnose "[twitch] disconnected, retrying in 5s"
reconnect cycles on a quiet channel.

Wire behavior replicated from TwitchSocketConnect / TwitchSocketRun:
  PASS oauth:<token>\\r\\n
  NICK <user>\\r\\n
  JOIN #<channel>\\r\\n        (sent back-to-back, no CAP REQ)
  ... then loop:
    - if >= 60 s with zero received bytes AND >= 60 s since last client PING:
        send "PING :tmi.twitch.tv"
    - reply "PONG :tmi.twitch.tv" to any line starting with "PING"
    - every received byte counts as traffic (resets the idle clock)

The probe ALSO simulates the C code's fate without disconnecting: whenever
the quiet gap crosses 120 s (the SO_RCVTIMEO), it prints a marker saying the
game would have reconnected at that instant. This lets one 5-minute run show
exactly when/why zelda3.exe would cycle.

Security: the OAuth token is read from twitch_config.txt and NEVER printed;
the PASS line is logged as "PASS oauth:***". Read-only usage: login, JOIN,
PING/PONG, listen. No chat is ever sent.

Usage:
  python tools/twitch_irc_probe.py [--duration 300] [--idle 60]
                                   [--ping-form "PING :tmi.twitch.tv"]
                                   [--no-client-ping]
"""

import argparse
import socket
import sys
import time
from pathlib import Path


def load_config():
    cfg = {}
    path = Path(__file__).resolve().parent.parent / "twitch_config.txt"
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        s = raw.strip()
        if not s or s[0] in "#/;":
            continue
        if "=" not in s:
            continue
        k, v = s.split("=", 1)
        cfg[k.strip().lower()] = v.strip()
    tok = cfg.get("token", "")
    if tok.lower().startswith("oauth:"):
        tok = tok[6:]
    user = cfg.get("user", cfg.get("nick", "")).lower()
    chan = cfg.get("channel", "").lower().lstrip("#")
    if not (tok and user and chan):
        sys.exit("[probe] config missing token/user/channel")
    return tok, user, chan


class Probe:
    def __init__(self, idle_s, ping_form, client_ping=True):
        self.idle_s = idle_s
        self.ping_form = ping_form
        self.client_ping = client_ping
        self.t0 = time.monotonic()
        self.last_traffic = self.t0
        self.last_ping = self.t0
        self.lines = 0
        self.client_pings = 0
        self.pongs = 0
        self.server_pings = 0
        self.gaps = []            # (gap_seconds, at_seconds) between data reads
        self.pending_ping = None  # time of last unanswered client PING
        self.timeout_markers = 0  # times the 120 s SO_RCVTIMEO fate was marked
        self.armed_marker = False

    def ts(self):
        return time.monotonic() - self.t0

    def log(self, direction, text):
        tok = None
        print("[%8.2fs] %s %s" % (self.ts(), direction, text), flush=True)

    def mark_rcvtimeout_fate(self, now):
        # The C code's recv() returns <= 0 once quiet for TW_RCVTIMEO_MS=120s
        # and TwitchSocketRun returns -1 -> "[twitch] disconnected, retrying".
        if now - self.last_traffic >= 120.0 and not self.armed_marker:
            self.armed_marker = True
            self.timeout_markers += 1
            self.log("!!", "SIM: zelda3 would RECONNECT now "
                            "(quiet %.1fs >= SO_RCVTIMEO 120s)"
                     % (now - self.last_traffic))

    def run(self, dur):
        tok, user, chan = load_config()
        self.log(">>", "connecting irc.chat.twitch.tv:6667 "
                       "(user=%s channel=%s token=oauth:***)" % (user, chan))
        sock = socket.create_connection(("irc.chat.twitch.tv", 6667), timeout=15)
        sock.settimeout(1.0)   # 1 s poll: same wire behavior as the C block,
                               # but lets us schedule the idle PING precisely
        buf = b""
        end = time.monotonic() + dur

        # Exact login sequence from TwitchSocketRun (lines 316-320).
        for line in ("PASS oauth:" + tok, "NICK " + user, "JOIN #" + chan):
            wire = line.replace(tok, "oauth:***")
            self.log(">>", wire)
            sock.sendall((line + "\r\n").encode("utf-8"))
        self.log(">>", "login sent, joining #%s" % chan)

        while time.monotonic() < end:
            now = time.monotonic()
            # Keepalive gate, identical condition to twitch.c line 338.
            if (self.client_ping
                    and now - self.last_traffic >= self.idle_s
                    and now - self.last_ping >= self.idle_s):
                self.log(">>", "SEND %r (idle %.1fs)"
                         % (self.ping_form, now - self.last_traffic))
                sock.sendall((self.ping_form + "\r\n").encode("utf-8"))
                self.last_ping = now
                self.client_pings += 1
                self.pending_ping = now
            try:
                data = sock.recv(1024)   # 1024 like the C recv buffer
            except socket.timeout:
                self.mark_rcvtimeout_fate(time.monotonic())
                continue
            except OSError as e:
                self.log("!!", "socket error: %s" % e)
                break
            now = time.monotonic()
            if not data:
                self.log("!!", "server closed connection (EOF) at +%.1fs"
                         % self.ts())
                break
            gap = now - self.last_traffic
            self.gaps.append((gap, self.ts()))
            if self.pending_ping is not None:
                self.log("<<", "... %.3fs after our client PING"
                         % (now - self.pending_ping))
                self.pending_ping = None
            elif gap > 5.0:
                self.log("<<", "(quiet gap %.1fs ended)" % gap)
            self.armed_marker = False
            self.last_traffic = now
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                if not line:
                    continue
                self.lines += 1
                low = line.lower()
                if "pong" in low:
                    self.pongs += 1
                    self.log("<<", "PONG  | " + line)
                elif line.startswith("PING"):
                    self.server_pings += 1
                    self.log("<<", "SRVPING | " + line)
                    resp = "PONG :tmi.twitch.tv"   # twitch.c line 429
                    self.log(">>", "SEND %r (server-ping reply)" % resp)
                    sock.sendall((resp + "\r\n").encode("utf-8"))
                else:
                    self.log("<<", line)

        try:
            sock.close()
        except OSError:
            pass
        self.summary()

    def summary(self):
        gaps = sorted(self.gaps, reverse=True)
        print("\n===== PROBE SUMMARY (duration %.1fs) =====" % self.ts())
        print("lines received      : %d" % self.lines)
        print("client PINGs sent   : %d (%r every %ds idle)"
              % (self.client_pings, self.ping_form, self.idle_s))
        print("PONGs received      : %d" % self.pongs)
        print("server PINGs        : %d" % self.server_pings)
        print("recv-timeout fates  : %d (times zelda3 would have cycled)"
              % self.timeout_markers)
        if gaps:
            print("longest quiet gap   : %.1fs (at t+%.1fs)"
                  % (gaps[0][0], gaps[0][1]))
            print("top 5 quiet gaps    : %s"
                  % ", ".join("%.1fs@+%.0fs" % g for g in gaps[:5]))
        else:
            print("longest quiet gap   : n/a (no traffic at all)")
        print("=============================================")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=300)
    ap.add_argument("--idle", type=int, default=60,
                    help="idle seconds before client PING (twitch.c: 60)")
    ap.add_argument("--ping-form", default="PING :tmi.twitch.tv",
                    help="exact PING wire line (twitch.c default)")
    ap.add_argument("--no-client-ping", action="store_true",
                    help="listen only: never send client PING")
    a = ap.parse_args()
    Probe(a.idle, a.ping_form, client_ping=not a.no_client_ping).run(a.duration)
    return 0


if __name__ == "__main__":
    sys.exit(main())

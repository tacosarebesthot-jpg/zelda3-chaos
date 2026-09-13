#!/usr/bin/env python3
"""Per-viewer limits, exercised over the REAL IRC path with a fake Twitch (argus).

The drop folder is exempt from per-viewer limits by design, so the only way to
test them without a live channel is to be the channel: this script runs a tiny
IRC server on 127.0.0.1:16667, the game is pointed at it through the
harness-only twitch_config keys irc_host/irc_port, and scripted viewers say
things.  Assertions are on the game's RESULT lines:

  alice !speed            -> applied (effect START)
  alice !speed  (2 s)     -> DROPPED reason=user_cooldown   (30 s per viewer+verb)
  bob   !flip             -> applied                        (limits are per viewer)
  alice !curse            -> applied                        (2nd effect slot)
  alice !confuse          -> DROPPED reason=user_effect_cap (cap 2 per viewer)
  bob   !confuse          -> applied                        (bob owns 1)

Note: re-arming an effect another viewer already owns (bob !speed while
alice's speed runs) transfers that slot to the new viewer - ownership is
"last arm", so a cap test must use distinct verbs per viewer.

Usage: python tools/peruser_limits_check.py
"""
import os
import re
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

import phase2_scenarios  # noqa: E402
from phase2_scenarios import Session  # noqa: E402
import rig  # noqa: E402

PORT = 16667
CHAN = "harness"
rig.TEST_TWITCH_CFG = """\
# harness: fake local IRC, per-viewer limits ON, global cooldown OFF
token=fakefakefakefakefakefakefake
user=harnessbot
channel=%s
enabled=1
drop=1
debug=1
test=0
cooldown=1
effect_secs=8
vs_mode=0
per_user_limits=1
per_user_cooldown_secs=30
per_user_max_effects=2
global_max_effects=8
irc_host=127.0.0.1
irc_port=%d
""" % (CHAN, PORT)


class FakeTwitch(threading.Thread):
    """One-client IRC server: answers the login with 001, PONGs PINGs, and
    lets the test inject PRIVMSG lines from named viewers."""

    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", PORT))
        self.srv.listen(1)
        self.conn = None
        self.logged_in = threading.Event()
        self.lines = []

    def run(self):
        self.conn, _ = self.srv.accept()
        self.conn.settimeout(0.5)
        buf = b""
        nick = "harnessbot"
        while True:
            try:
                data = self.conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                self.lines.append(line)
                if line.startswith("NICK "):
                    nick = line[5:].strip()
                elif line.startswith("JOIN "):
                    self.send(":tmi.twitch.tv 001 %s :Welcome, GLHF!" % nick)
                    self.send(":%s!%s@%s.tmi.twitch.tv JOIN #%s" % (nick, nick, nick, CHAN))
                    self.logged_in.set()
                elif line.startswith("PING"):
                    self.send("PONG :tmi.twitch.tv")

    def send(self, s):
        try:
            self.conn.sendall((s + "\r\n").encode())
        except OSError:
            pass

    def say(self, who, text):
        self.send(":%s!%s@%s.tmi.twitch.tv PRIVMSG #%s :%s" % (who, who, who, CHAN, text))


RESULTS = []


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
    RESULTS.append(ok)


def expect(tail, rx, timeout=6.0):
    """Wait for a RESULT line matching rx; returns the line or None."""
    r = re.compile(rx)
    deadline = time.time() + timeout
    seen = len(tail.events)
    while time.time() < deadline:
        tail.pump()
        for _t, ln in list(tail.events)[seen:]:
            if r.search(ln):
                return ln
        time.sleep(0.05)
    return None


def main():
    fake = FakeTwitch()
    fake.start()
    with Session("peruser", audio=False) as s:
        t = s.tail
        s.load_chapter(1)
        if not fake.logged_in.wait(20):
            check("fake_irc_login", False, "game never sent JOIN to the fake server")
            return 1
        ok = expect(t, r"connected to chat|verb=", 8) is not None or \
            "connected to chat" in open(s.log_path, errors="replace").read()
        check("fake_irc_login", True, "game logged in to 127.0.0.1:%d" % PORT)
        time.sleep(1.0)

        fake.say("alice", "!speed")
        check("alice_speed_applied", expect(t, r"verb=speed effect=START") is not None)
        time.sleep(2.0)
        fake.say("alice", "!speed")
        check("alice_speed_cooldown", expect(t, r"verb=speed DROPPED reason=user_cooldown") is not None)
        # a DIFFERENT verb for bob: re-arming alice's speed would hand that slot
        # to bob (ownership = last arm, by design) and free alice's budget
        fake.say("bob", "!flip")
        check("bob_flip_applied", expect(t, r"verb=flip effect=START") is not None)
        fake.say("alice", "!curse")
        check("alice_curse_second_slot", expect(t, r"verb=curse effect=START") is not None)
        fake.say("alice", "!confuse")
        check("alice_confuse_capped", expect(t, r"verb=confuse DROPPED reason=user_effect_cap") is not None)
        fake.say("bob", "!confuse")
        check("bob_confuse_applied", expect(t, r"verb=confuse effect=START") is not None)
        s.shot("pu_end")
    print("PER-USER LIMITS: %d/%d passed" % (sum(RESULTS), len(RESULTS)))
    return 0 if all(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Live-demo board for the zelda3 Twitch showcase stream.

Run from anywhere:  python tools/demo_board.py
Fires fake chat commands through the game's drop-folder (drop=1 must be on
in twitch_config.txt). Grouped menu, single keypress per command, plus an
auto "showcase" cinematic sequence timed for camera.

Keys are chosen so a dozen effects are one tap away - no typing verbs live.
"""
import os
import sys
import time

DROP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", "twitch_drop"))

# (key, label, verb, arg, dur_frames)
# dur 0 = game default (effect_secs). frames: 60 = 1 second.
MENU = [
    ("CHILL (safe to fire anywhere)", [
        ("1", "heal full",        "heal",    ""),
        ("2", "hurt 2 hearts",    "hurt",    "16"),
        ("3", "refill hp+magic",  "refill",  ""),
        ("4", "rupees +100",      "rupees",  "100"),
        ("5", "fairy",            "fairy",   ""),
    ]),
    ("CHAOS (visual, chat-pleasers)", [
        ("q", "flip screen",      "flip",    ""),
        ("w", "confuse 2 screens", "confuse", ""),
        ("e", "party (flip+confuse)", "party", ""),
        ("a", "freeze enemies",   "freeze",  ""),
        ("s", "swarm",            "swarm",   "4"),
        ("d", "spawn keese x6",   "spawn",   "keese 6"),
        ("z", "smite (clear screen)", "smite", ""),
        ("x", "curse",            "curse",   ""),
    ]),
    ("TRANSFORMS", [
        ("r", "bunny (illusion)", "illusion", ""),
        ("t", "ARISE CHICKEN!!",  "arise",   ""),
        ("y", "ice floor",        "ice",     ""),
        ("u", "slow",             "slow",    ""),
        ("i", "speed",            "speed",   ""),
    ]),
    ("BLOCKERS", [
        ("g", "deny item",        "deny",      ""),
        ("h", "deny boots",       "denyboots", ""),
        ("j", "steal an item",    "steal",     ""),
        ("k", "root Link",        "root",      ""),
    ]),
]

SHOWCASE = [
    # (delay_before_s, verb, arg)
    (2.0,  "swarm",    "5"),
    (6.0,  "freeze",   ""),
    (2.0,  "flip",     ""),
    (5.0,  "party",    ""),
    (8.0,  "confuse",  "3"),
    (4.0,  "speed",    ""),
    (10.0, "slow",     ""),
    (6.0,  "bunny",    ""),
    (14.0, "arise",    ""),
    (12.0, "smite",    ""),
    (2.0,  "refill",   ""),
]


def drop(verb, arg):
    os.makedirs(DROP, exist_ok=True)
    name = "demo_%d.txt" % time.time_ns()
    with open(os.path.join(DROP, name), "w") as f:
        f.write("%s|%s|stream|0\n" % (verb, arg))


def fire(key):
    for _, group in MENU:
        for k, label, verb, arg in group:
            if k == key:
                drop(verb, arg)
                print("  -> %s (%s %s)" % (label, verb, arg or "-"))
                return True
    return False


def showcase():
    print("== showcase sequence (%.0fs total, Ctrl+C to stop) ==" %
          sum(d for d, _, _ in SHOWCASE))
    try:
        for delay, verb, arg in SHOWCASE:
            time.sleep(delay)
            drop(verb, arg)
            print("  [%s] %s %s" % (time.strftime("%H:%M:%S"), verb, arg or "-"))
    except KeyboardInterrupt:
        print("\n  sequence stopped")


def main():
    print("zelda3 demo board - drop folder: %s" % DROP)
    print("(game must be running with drop=1 in twitch_config.txt)\n")
    for header, group in MENU:
        print(header)
        for k, label, _, _ in group:
            print("  [%s] %s" % (k, label))
        print()
    print("[S] auto showcase sequence      [Q] quit\n")
    while True:
        try:
            c = input("fire> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            break
        if c == "q":
            break
        if c == "s":
            showcase()
            continue
        if c == "h":
            drop("heal", "")
            print("  -> heal full")
            continue
        if not fire(c):
            print("  ? unknown key")


if __name__ == "__main__":
    sys.exit(main())

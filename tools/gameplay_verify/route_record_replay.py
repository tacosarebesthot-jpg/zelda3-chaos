#!/usr/bin/env python3
"""Record-and-replay for the randomizer chest gold test (owner demo).

--record : boots zelda3.exe with the randomizer enabled, loads the chosen
  chapter reference save, announces "GO - owner has controls" and then only
  PASSIVELY records keyboard input (GetAsyncKeyState poll at ~30 Hz; no key
  is ever injected).  Recording stops when tracker_items.txt changes (the
  engine's Link_ReceiveItem hook = a chest item landed) or the owner presses
  F12.  Writes:
    route_recording.txt      raw transition log (t, key, down/up)
    owner_chest_route.txt    replayable (key, hold_ms) runs for the rig

--replay : boots again, reloads the reference save for a clean start,
  replays the recorded route with the rig's SendInput pattern, screenshots
  before/during/after and prints the tracker receipt line for comparison
  against rando_placement.json.

Usage (repo root):
  python tools/gameplay_verify/route_record_replay.py --record [--seed 5] [--chapter 2]
  python tools/gameplay_verify/route_record_replay.py --replay
"""
import argparse
import ctypes
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))                 # repo root
SHOTS = os.path.join(HERE, "shots_rando")
INI = os.path.join(HERE, "zelda3_verify.ini")
RUN_LOG = os.path.join(HERE, "run_route_rr.log")
RANDO_INI = os.path.join(BASE, "randomizer.ini")
TRACKER = os.path.join(BASE, "tracker_items.txt")
RAW_LOG = os.path.join(HERE, "route_recording.txt")
ROUTE = os.path.join(HERE, "owner_chest_route.txt")

user32 = ctypes.windll.user32

# virtual keys we watch -> rig key names (rig.SC scancodes for replay)
VK_TABLE = {
    0x25: "left", 0x26: "up", 0x27: "right", 0x28: "down",
    0x41: "a", 0x43: "c", 0x53: "s", 0x56: "v", 0x58: "x", 0x5A: "z",
    0x30: "0", 0x31: "1", 0x32: "2", 0x33: "3", 0x34: "4", 0x35: "5",
    0x36: "6", 0x37: "7", 0x38: "8", 0x39: "9",
    0xBB: "=", 0xBD: "-", 0x08: "backspace", 0x0D: "return", 0xA1: "rshift",
}
VK_F12 = 0x7B


def tracker_state():
    try:
        return (os.path.getmtime(TRACKER), open(TRACKER, "r", errors="replace").read())
    except OSError:
        return (0, "")


def shoot(g, name):
    path = g.shot(SHOTS, name)
    print("    shot: %s" % os.path.relpath(path, BASE))


def load_chapter(g, chapter, settle=4.0):
    marker = "*** Loading slot %d" % (256 + chapter - 1)
    before = os.path.getsize(RUN_LOG) if os.path.exists(RUN_LOG) else 0
    rig.tap(rig.loadref_key(chapter))
    deadline = time.time() + 10.0
    while time.time() < deadline:
        with open(RUN_LOG, "r", errors="replace") as fh:
            fh.seek(max(0, before - 64))
            if marker in fh.read():
                print("    load marker seen")
                break
        time.sleep(0.2)
    else:
        print("    [warn] no load marker seen")
    time.sleep(settle)


def start_game(args):
    with open(RANDO_INI, "r", encoding="utf-8") as fh:
        backup = fh.read()
    with open(RANDO_INI, "w", encoding="utf-8") as fh:
        fh.write("enabled=1\nseed=%d\nlog=1\n" % args.seed)
    print("randomizer.ini: enabled=1 seed=%d" % args.seed)
    g = rig.Game(BASE, RUN_LOG, os.path.relpath(INI, BASE))
    g.start()
    print("boot: waiting %.1fs" % args.bootwait)
    time.sleep(args.bootwait)
    load_chapter(g, args.chapter)
    return g, backup


def restore(g, backup):
    try:
        g.stop()
    except Exception:
        pass
    with open(RANDO_INI, "w", encoding="utf-8") as fh:
        fh.write(backup)
    print("randomizer.ini restored")


def record(args):
    g, backup = start_game(args)
    transitions = []          # (t_rel_ms, name, down_bool)
    state = {vk: False for vk in VK_TABLE}
    base_tracker = tracker_state()
    t0 = time.time()
    print("GO - owner has controls", flush=True)
    shoot(g, "owner_00_go")
    try:
        while True:
            now = time.time()
            for vk, name in VK_TABLE.items():
                down = bool(user32.GetAsyncKeyState(vk) & 0x8000)
                if down != state[vk]:
                    state[vk] = down
                    transitions.append((int((now - t0) * 1000), name, down))
            if bool(user32.GetAsyncKeyState(VK_F12) & 0x8000):
                print("    F12 pressed - stopping recording")
                break
            st = tracker_state()
            if st[0] != base_tracker[0] or st[1] != base_tracker[1]:
                print("    tracker_items.txt changed - item received, stopping")
                break
            if now - t0 > args.max_s:
                print("    %ss elapsed - stopping recording" % args.max_s)
                break
            time.sleep(1.0 / 30.0)
    except KeyboardInterrupt:
        print("    interrupted - stopping recording")
    time.sleep(0.5)
    shoot(g, "owner_01_end")
    receipt = tracker_state()[1].strip().splitlines()
    print("    tracker receipt now: %r" % (receipt[:2],))

    # raw log
    with open(RAW_LOG, "w") as fh:
        fh.write("# route recording %s seed=%d chapter=%d\n"
                 % (time.strftime("%Y-%m-%d %H:%M:%S"), args.seed, args.chapter))
        for t, name, down in transitions:
            fh.write("t=%6dms %-6s %s\n" % (t, name, "down" if down else "up"))
    # replayable route: pair each down with the next up of the same key
    opens = {}
    steps = []
    for t, name, down in transitions:
        if down:
            opens[name] = t
        elif name in opens:
            steps.append((name, max(30, t - opens[name])))
            del opens[name]
    for name, t in sorted(opens.items()):     # still-held keys: release at end
        steps.append((name, 200))
    with open(ROUTE, "w") as fh:
        fh.write("# replayable route (%d steps) - rig SendInput pattern\n"
                 % len(steps))
        for name, ms in steps:
            fh.write("hold %s %d\n" % (name, ms))
    print("    raw log: %s (%d transitions), route: %s (%d steps)"
          % (RAW_LOG, len(transitions), ROUTE, len(steps)))
    restore(g, backup)


def replay(args):
    steps = []
    for line in open(ROUTE):
        line = line.strip()
        if line and not line.startswith("#"):
            _, name, ms = line.split()
            steps.append((name, int(ms)))
    print("route: %d steps from %s" % (len(steps), ROUTE))
    g, backup = start_game(args)
    try:
        shoot(g, "replay_00_start")
        for i, (name, ms) in enumerate(steps):
            rig.key_down(name)
            time.sleep(ms / 1000.0)
            rig.key_up(name)
            time.sleep(0.03)
            if i in (len(steps) // 2,):
                shoot(g, "replay_01_midway")
        time.sleep(2.0)
        shoot(g, "replay_02_after")
        print("    tracker receipt: %r" % (tracker_state()[1].strip().splitlines()[:2],))
    finally:
        restore(g, backup)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--replay", action="store_true")
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--chapter", type=int, default=2)
    ap.add_argument("--bootwait", type=float, default=12.0)
    ap.add_argument("--max-s", type=float, default=480.0)
    args = ap.parse_args()
    if args.record == args.replay:
        ap.error("pick exactly one of --record / --replay")
    if args.record:
        record(args)
    else:
        replay(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())

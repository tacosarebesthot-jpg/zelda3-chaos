#!/usr/bin/env python3
"""Phase-B gold test for the native randomizer: boot zelda3.exe with
randomizer.ini enabled, load a chapter reference save, walk to a chest whose
placement is known from rando_placement.json / randomizer_log.txt, open it
and screenshot the received item.

Scenario (defaults): seed 5 puts the HOOKSHOT in the single chest of room
0x012 (ALTTPR location 'Sanctuary') - the room the chapter-1 reference save
("Zelda's Rescue") spawns in.  Opening that chest must hand Link the
Hookshot, matching the placement the engine logged at boot.

Usage (from anywhere; game launched with cwd = repo root):
  python tools/gameplay_verify/run_rando_gold.py [--seed 5] [--chapter 1]
         [--moves "hold:up:0.5;tap:x;shot:name;open"] [--bootwait 6]

Move language:
  hold:<key>:<seconds>   hold a direction/button (up/down/left/right/x/z/s/a/c/v)
  tap:<key>              tap a key
  shot:<name>            screenshot to shots_rando/<name>.png
  open                   face-and-press-A sequence used for a chest
  wait:<seconds>         sit still
The script restores randomizer.ini on exit (its original bytes are kept).
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))  # repo root
SHOTS = os.path.join(HERE, "shots_rando")
INI = os.path.join(HERE, "zelda3_verify.ini")
RUN_LOG = os.path.join(HERE, "run_rando_gold.log")
RANDO_INI = os.path.join(BASE, "randomizer.ini")


def shoot(g, name):
    path = g.shot(SHOTS, name)
    print("    shot: %s" % os.path.relpath(path, BASE))
    return path


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


def run_moves(g, moves):
    for step in moves:
        if not step:
            continue
        parts = step.split(":")
        op = parts[0]
        if op == "hold":
            rig.key_down(parts[1])
            time.sleep(float(parts[2]))
            rig.key_up(parts[1])
        elif op == "tap":
            rig.tap(parts[1])
        elif op == "shot":
            shoot(g, parts[1])
        elif op == "wait":
            time.sleep(float(parts[1]))
        elif op == "open":
            # face the chest and press A twice (open + dismiss nothing);
            # caller positions Link first
            rig.tap("x")   # A button
            time.sleep(1.2)
            rig.tap("x")
            time.sleep(1.2)
        else:
            print("    [warn] unknown move %r" % step)
        time.sleep(0.15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--chapter", type=int, default=1)
    ap.add_argument("--bootwait", type=float, default=6.0)
    ap.add_argument("--moves", default=(
        "shot:10_spawn_room;"
        "wait:1.0;"
        "shot:11_spawn_room_settled"))
    args = ap.parse_args()

    # enable the randomizer for this run (original bytes restored at exit)
    with open(RANDO_INI, "r", encoding="utf-8") as fh:
        rando_backup = fh.read()
    with open(RANDO_INI, "w", encoding="utf-8") as fh:
        fh.write("enabled=1\nseed=%d\nlog=1\n" % args.seed)
    print("randomizer.ini: enabled=1 seed=%d" % args.seed)

    g = rig.Game(BASE, RUN_LOG, os.path.relpath(INI, BASE))
    ok = False
    try:
        g.start()
        print("boot: waiting %.1fs" % args.bootwait)
        time.sleep(args.bootwait)
        print("load: pressing '%s' -> saves/ref/Chapter %d"
              % (rig.loadref_key(args.chapter), args.chapter))
        load_chapter(g, args.chapter)
        run_moves(g, args.moves.split(";"))
        ok = True
    finally:
        try:
            g.stop()
        except Exception:
            pass
        with open(RANDO_INI, "w", encoding="utf-8") as fh:
            fh.write(rando_backup)
        print("randomizer.ini restored")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

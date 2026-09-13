#!/usr/bin/env python3
"""Phase-B2 pin gold test: boot zelda3.exe with the randomizer enabled AND a
demo-chest pin set, load a chapter reference save, open the pinned chest and
screenshot the received item.

Default scenario: pin_location=Sanctuary (the chapter-1 spawn chest, room
0x012) pin_item=Hookshot at seed 1234 - a seed whose UNPINNED fill puts a
Piece of Heart there, so a Hookshot from that chest can only come from the
pin.  Screenshots go to shots_pin/.

Usage:
  python tools/gameplay_verify/pin_gold_run.py [--pin_location Sanctuary]
         [--pin_item Hookshot] [--seed 1234] [--chapter 1] [--bootwait 8]
         [--moves "hold:down:0.1;...;shot:name"]
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))  # repo root
SHOTS = os.path.join(HERE, "shots_pin")
INI = os.path.join(HERE, "zelda3_verify.ini")
RUN_LOG = os.path.join(HERE, "run_pin_gold.log")
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
        else:
            print("    [warn] unknown move %r" % step)
        time.sleep(0.15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pin_location", default="Sanctuary")
    ap.add_argument("--pin_item", default="Hookshot")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--chapter", type=int, default=1)
    ap.add_argument("--bootwait", type=float, default=8.0)
    ap.add_argument("--moves", default=(
        "shot:10_spawn;"
        "hold:down:0.1;hold:left:0.7;hold:down:0.28;"
        "shot:50_on_chest;tap:x;wait:1.5;shot:51_item;wait:2.0;shot:52_text"))
    args = ap.parse_args()

    with open(RANDO_INI, "r", encoding="utf-8") as fh:
        rando_backup = fh.read()
    with open(RANDO_INI, "w", encoding="utf-8") as fh:
        fh.write("enabled=1\nseed=%d\nlog=1\npin_location=%s\npin_item=%s\n"
                 % (args.seed, args.pin_location, args.pin_item))
    print("randomizer.ini: enabled=1 seed=%d pin %s=%s"
          % (args.seed, args.pin_location, args.pin_item))

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

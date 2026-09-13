#!/usr/bin/env python3
"""confuse expiry: does the effect END after N room transitions? (argus)

The run_verify 'confuse_expiry' scenario walks a fixed chapter-2 route that
never leaves its room (Link is on a ledge), so it could not decide. This one
is route-agnostic: arm confuse|1, then hold each direction in turn until the
probe's room key changes, and assert the RESULT END line follows.

Usage: python tools/confuse_expiry_check.py [--chapter 6] [--screens 1]
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from phase2_scenarios import Session  # noqa: E402
import rig  # noqa: E402


def room_of(tail):
    p, _ = tail.latest()
    # a "screen" is any room/module/overworld-screen change: leaving the
    # sanctuary keeps room=18 but flips mod 7->9 and scr 0->19
    return None if p is None else (p.get("room", p.get("r")), p.get("module", p.get("mod")), p.get("scr"))


def walk_until_room_change(session, start_room, per_dir_s=7.0):
    """Hold DOWN, RIGHT, LEFT, UP (in that order) until the room key changes.
    Returns (new_room, direction) or (None, None)."""
    for d in ("down", "right", "left", "up"):
        session.game.ensure_focus()
        rig.key_down(d)
        t_end = time.time() + per_dir_s
        try:
            while time.time() < t_end:
                r = room_of(session.tail)
                if r is not None and r != start_room:
                    time.sleep(1.0)   # let the transition finish
                    return r, d
                time.sleep(0.05)
        finally:
            rig.key_up(d)
        time.sleep(0.3)
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chapter", type=int, default=6)
    ap.add_argument("--screens", type=int, default=1)
    args = ap.parse_args()
    fails = []
    with Session("confuse_expiry", audio=False) as s:
        s.load_chapter(args.chapter)
        time.sleep(1.0)
        r0 = room_of(s.tail)
        ack = s.drop_and_ack("confuse|%d|rig|0" % args.screens, r"verb=confuse effect=START")
        print("  START ack:", (ack or "NONE").strip(), flush=True)
        if not ack:
            fails.append("confuse START never acked")
        crossings = 0
        cur = r0
        for i in range(args.screens):
            new_r, d = walk_until_room_change(s, cur)
            print("  crossing %d: room %s -> %s via %s" % (i + 1, cur, new_r, d), flush=True)
            s.shot("ce_cross_%d" % (i + 1))
            if new_r is None:
                fails.append("could not leave room %s in any direction" % cur)
                break
            crossings += 1
            cur = new_r
        time.sleep(1.5)
        s.tail.pump()
        ended = [ln for _t, ln in s.tail.events if "verb=confuse effect=END" in ln]
        print("  END lines after %d crossing(s): %d" % (crossings, len(ended)), flush=True)
        if crossings == args.screens and not ended:
            fails.append("confuse END never fired after %d room transition(s)" % crossings)
        elif crossings == args.screens:
            print("  PASS confuse expires after %d screen(s)" % args.screens, flush=True)
    print("CONFUSE EXPIRY: %s" % ("PASS" if not fails else "FAIL " + "; ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

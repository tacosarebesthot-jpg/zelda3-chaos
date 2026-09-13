#!/usr/bin/env python3
"""One-off probe: use confuse|1 as a room-transition detector (the engine
prints RESULT verb=confuse effect=END at the FIRST room-key change) to map
which walks from the chapter-2 spawn actually cross rooms."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))
SHOTS = os.path.join(HERE, "shots_probe")


def main():
    os.makedirs(SHOTS, exist_ok=True)
    ini = rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"),
                              os.path.join(HERE, "zelda3_verify.ini"))
    g = rig.Game(BASE, os.path.join(HERE, "probe.log"),
                 os.path.relpath(ini, BASE))

    def hold(key, s):
        rig.key_down(key)
        time.sleep(s)
        rig.key_up(key)

    def shot(name):
        p = os.path.join(SHOTS, name + ".png")
        rig.capture_window(g.hwnd, p)
        print("shot", name)

    def check(label, timeout=4.0):
        line, _ = g.wait_for(r"verb=confuse effect=END", timeout)
        print("%-28s -> %s" % (label, "END (transition!)" if line
                               else "no END (same room)"))
        return line is not None

    def rearm():
        g.drop("confuse|1|probe|0")
        line, _ = g.wait_for(r"verb=confuse effect=START screens=1", 6)
        print("rearmed:", (line or "(none)").strip())

    def reload():
        rig.tap(rig.loadref_key(2))
        time.sleep(4.5)
        g.drop("heal|0|probe|0")
        g.wait_for(r"verb=heal hp=", 4)

    try:
        g.start()
        time.sleep(6)
        reload()
        print("in gameplay:", g.in_gameplay())

        print("== A: walk DOWN 3.0 s from spawn")
        rearm()
        hold("down", 3.0)
        time.sleep(1.2)
        shot("E1_after_down30")
        check("down 3.0")

        print("== B: continue DOWN 4.0 s (onto entrance stairwell)")
        rearm()
        hold("down", 4.0)
        time.sleep(1.2)
        shot("E2_after_down40_more")
        check("down 4.0 more")

        print("== C: from fresh spawn walk UP 2.0 s (top door)")
        reload()
        rearm()
        hold("up", 2.0)
        time.sleep(1.2)
        shot("E3_after_up20")
        check("up 2.0")

        print("== D: continue UP 2.0 s more")
        rearm()
        hold("up", 2.0)
        time.sleep(1.2)
        shot("E4_after_up20_more")
        check("up 2.0 more")
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

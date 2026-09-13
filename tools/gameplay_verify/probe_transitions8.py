#!/usr/bin/env python3
"""ch6 confuse-expiry FIX follow-up probe: the 4-leg SOUTH route never
crosses an overworld area key (Link wedges at the courtyard fence), so it
can never expire confuse|2 even with the fixed tick. This probe walks WEST
from the ref spawn (open grass corridor) with confuse|1 armed as a
room-key-change detector: one effect=END per area transition crossed."""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))
SHOTS = os.path.join(HERE, "shots_probe")
KEY = "left"


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

    def check(label, timeout=3.0):
        line, _ = g.wait_for(r"verb=confuse effect=END", timeout)
        print("%-30s -> %s" % (label, "END (area key changed!)" if line
                               else "no END yet"))
        return line is not None

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(6))
        time.sleep(4.5)
        print("in gameplay:", g.in_gameplay()[0])
        g.drop("confuse|1|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=1", 6)
        for i in (1, 2, 3, 4, 5, 6):
            print("== leg %d: %s 6.0 s" % (i, KEY.upper()))
            hold(KEY, 6.0)
            time.sleep(1.0)
            shot("W%d_leg%d" % (i, i))
            g.drop("heal|999|probe|0")
            g.wait_for(r"verb=heal hp=", 5)
            if check("leg %d" % i):
                shot("W_end_reached")
                break
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

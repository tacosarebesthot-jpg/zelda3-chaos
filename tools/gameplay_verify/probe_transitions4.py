#!/usr/bin/env python3
"""One-off probe on chapter 5 (Pyramid of Power, overworld): does walking
SOUTH across overworld screen boundaries decrement confuse? confuse|1 fires
END at the first room-key change. Shots every leg show the terrain."""
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

    def check(label, timeout=3.0):
        line, _ = g.wait_for(r"verb=confuse effect=END", timeout)
        print("%-30s -> %s" % (label, "END (crossed!)" if line
                               else "no END yet"))
        return line is not None

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(5))
        time.sleep(4.5)
        print("in gameplay:", g.in_gameplay())
        g.drop("confuse|1|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=1", 6)
        for i in (1, 2, 3):
            print("== leg %d: DOWN 3.0 s" % i)
            hold("down", 3.0)
            time.sleep(1.2)
            shot("G%d_leg%d" % (i, i))
            g.drop("heal|999|probe|0")
            g.wait_for(r"verb=heal hp=", 5)
            if check("leg %d" % i):
                shot("G_end_reached")
                break
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

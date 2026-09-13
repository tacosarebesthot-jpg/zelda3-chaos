#!/usr/bin/env python3
"""One-off probe on chapter 5: does entering the Pyramid door (a real
dungeon transition with a fade) decrement confuse? confuse|1 fires END at
the first room-key change."""
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

    def check(label, timeout=5.0):
        line, _ = g.wait_for(r"verb=confuse effect=END", timeout)
        print("%-34s -> %s" % (label, "END (counts!)" if line
                               else "no END"))
        return line is not None

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(5))
        time.sleep(4.5)
        print("in gameplay:", g.in_gameplay())
        g.drop("confuse|1|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=1", 6)
        print("== walk UP into the pyramid door (2.5 s)")
        hold("up", 2.5)
        time.sleep(1.5)
        shot("K1_after_up_into_pyramid")
        ended = check("enter pyramid door")
        print("== walk back OUT (2.5 s down), re-arm confuse|1")
        g.drop("confuse|1|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=1", 6)
        hold("down", 2.5)
        time.sleep(1.5)
        shot("K2_after_exit")
        ended2 = check("exit pyramid door")
        print("summary: enter=%s exit=%s" % (ended, ended2))
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

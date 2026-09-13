#!/usr/bin/env python3
"""One-off probe: after the stairwell entrance (which does NOT decrement -
the resync eats submodule transitions), do OVERWORLD screen scrolls south of
the courtyard decrement confuse? confuse|1 fires END at the first real
room-key change; confuse|2 at the second."""
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

    def heal():
        g.drop("heal|999|probe|0")
        g.wait_for(r"verb=heal hp=", 5)

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(2))
        time.sleep(4.5)
        print("in gameplay:", g.in_gameplay())
        g.drop("confuse|2|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=2", 6)
        print("== push to courtyard (travel, expect no END)")
        hold("down", 7.5)
        time.sleep(1.5)
        g.drop("smite||probe|0")
        g.wait_for(r"verb=smite killed=\d+", 6)
        heal()
        shot("F1_courtyard")
        if check("at courtyard"):
            print("(already ended - courtyard crossing counts!)")
            return
        for i in (1, 2, 3):
            print("== continue DOWN 4.0 s (leg %d)" % i)
            hold("down", 4.0)
            time.sleep(1.0)
            heal()
            shot("F%d_leg%d" % (i + 1, i))
            if check("leg %d" % i):
                shot("F_end_reached")
                break
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

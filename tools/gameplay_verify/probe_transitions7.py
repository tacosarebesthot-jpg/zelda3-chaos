#!/usr/bin/env python3
"""One-off probe on chapter 6 (Dark Palace entrance, normal-form Link):
does walking SOUTH across overworld screens decrement confuse? confuse|1
fires END at the first room-key change (overworld scrolling counts; fades
never do - they resync g_tw_last_room)."""
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
        rig.tap(rig.loadref_key(6))
        time.sleep(4.5)
        print("in gameplay:", g.in_gameplay())
        g.drop("confuse|1|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=1", 6)
        for i in (1, 2, 3, 4):
            print("== leg %d: DOWN 6.0 s" % i)
            hold("down", 6.0)
            time.sleep(1.0)
            shot("L%d_leg%d" % (i, i))
            g.drop("heal|999|probe|0")
            g.wait_for(r"verb=heal hp=", 5)
            if check("leg %d" % i):
                shot("L_end_reached")
                break
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

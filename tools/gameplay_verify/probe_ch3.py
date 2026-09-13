#!/usr/bin/env python3
"""One-off probe: map a clear dash runway on the chapter-3 ref save
(Desert Palace entrance, overworld). Walks each direction from a fresh
reload and screenshots where Link ends up."""
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

    def shot(name):
        p = os.path.join(SHOTS, name + ".png")
        rig.capture_window(g.hwnd, p)
        print("shot", name)

    def leg(key, s, name):
        rig.key_down(key)
        time.sleep(s)
        rig.key_up(key)
        time.sleep(0.8)
        shot(name)

    def reload():
        rig.tap(rig.loadref_key(3))
        time.sleep(4.5)

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(3))
        time.sleep(4.5)
        print("in gameplay:", g.in_gameplay())
        leg("down", 1.2, "C1_down")
        reload()
        leg("left", 1.2, "C2_left")
        reload()
        leg("right", 1.2, "C3_right")
        reload()
        leg("down", 0.5, "C4_down_short")
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

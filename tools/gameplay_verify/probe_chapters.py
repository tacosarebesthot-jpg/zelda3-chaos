#!/usr/bin/env python3
"""One-off probe: screenshot the spawn room of each boots-owning ref chapter
(4..13) to find one with a long straight runway for the denyboots dash A/B."""
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

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(4))
        time.sleep(5)
        for ch in (4, 5, 6, 7, 8, 9, 10, 11):
            rig.tap(rig.loadref_key(ch))
            time.sleep(4.0)
            g.drop("heal|0|probe|0")
            g.wait_for(r"verb=heal hp=", 4)
            time.sleep(1.0)
            shot("D%d_chapter%d" % (ch, ch))
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

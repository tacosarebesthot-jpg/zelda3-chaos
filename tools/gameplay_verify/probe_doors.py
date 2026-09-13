#!/usr/bin/env python3
"""One-off probe: which room transitions can the rig actually drive from the
chapter-2 spawn hall?

Sequence (all from LoadRef reloads):
  A) walk UP through the hall's top door into the wide room   -> shot A1
  B) hold UP into the door-like structure at the room's top   -> shot A2
  C) hold DOWN 4.0s (the bottom stairwell, long attempt)      -> shot A3
  D) hold DOWN 2.5s more (whatever A3 left in reach)          -> shot A4
Each phase also drops a heal|0 probe: DROPPED means a transition/submodule
was in flight at that moment. twitch_config.txt is restored on exit.
"""
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

    def probe():
        g.drop("heal|0|probe|0")
        line, _ = g.wait_for(r"verb=heal (hp=\d+->\d+|DROPPED)", 2.5)
        print("  probe:", (line or "(none)").strip())

    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(2))
        time.sleep(4)
        print("in gameplay:", g.in_gameplay())

        hold("up", 2.0)
        time.sleep(1.0)
        shot("A1_wide_room")
        probe()

        hold("up", 2.6)
        time.sleep(1.2)
        shot("A2_after_up_hold")
        probe()

        hold("down", 4.0)
        time.sleep(1.2)
        shot("A3_after_long_down")
        probe()

        hold("down", 2.5)
        time.sleep(1.2)
        shot("A4_after_more_down")
        probe()
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

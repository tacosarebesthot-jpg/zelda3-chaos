#!/usr/bin/env python3
"""One-off probe: what does confuse do to KEYBOARD input in this port?

Reaches gameplay, walks Link onto the open floor, then:
  A) holds RIGHT (no confuse)  -> baseline displacement
  B) fires confuse|1, holds RIGHT -> where does Link end up?
  C) under confuse, holds LEFT -> and now?
Screenshots land in shots_probe/. twitch_config.txt is restored on exit.
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
    try:
        g.start()
        time.sleep(6)
        rig.tap(rig.loadref_key(2))
        time.sleep(4)
        print("in gameplay:", g.in_gameplay())

        def shot(name):
            p = os.path.join(SHOTS, name + ".png")
            rig.capture_window(g.hwnd, p)
            print("shot", name)

        # walk to the open floor below the knights
        rig.key_down("down"); time.sleep(1.4); rig.key_up("down")
        time.sleep(0.5)
        shot("A_floor")

        # baseline: hold RIGHT 1.0s (no confuse)
        shot("B_before_right")
        rig.key_down("right"); time.sleep(1.0); rig.key_up("right")
        time.sleep(0.4)
        shot("C_after_right")

        # back to center-ish, fire confuse, hold RIGHT again
        rig.key_down("left"); time.sleep(1.6); rig.key_up("left")
        time.sleep(0.4)
        g.drop("confuse|1|probe|0")
        line, _ = g.wait_for(r"verb=confuse effect=START", 6)
        print(line)
        shot("D_before_confused_right")
        rig.key_down("right"); time.sleep(1.0); rig.key_up("right")
        time.sleep(0.4)
        shot("E_after_confused_right")

        # and LEFT under confuse
        rig.key_down("left"); time.sleep(1.0); rig.key_up("left")
        time.sleep(0.4)
        shot("F_after_confused_left")

        # what does Select (bit7) do under confuse? (theory: kbd-right ->
        # bit7). Tap rshift and take a shot.
        rig.tap("rshift")
        time.sleep(1.0)
        shot("G_after_select_tap")
        rig.tap("rshift")
        time.sleep(1.0)
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

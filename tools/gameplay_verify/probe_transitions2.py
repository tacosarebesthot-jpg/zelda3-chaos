#!/usr/bin/env python3
"""One-off probe: does the Sanctuary->courtyard entrance crossing decrement
confuse screens? confuse|1 must print effect=END at the FIRST transition."""
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

    def check(label, timeout=6.0):
        line, _ = g.wait_for(r"verb=confuse effect=END", timeout)
        print("%-34s -> %s" % (label, "END (counts!)" if line
                               else "no END (does NOT count)"))
        return line is not None

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

        print("== A: confuse|1 then 7.5 s down-push to the courtyard")
        g.drop("confuse|1|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=1", 6)
        hold("down", 7.5)
        time.sleep(1.5)
        g.drop("smite||probe|0")
        g.wait_for(r"verb=smite killed=\d+", 6)
        ok_a = check("down 7.5 (sanctuary->courtyard)")

        print("== B: confuse|2, down 7.5, then up 4.5 (suite sequence)")
        reload()
        g.drop("confuse|2|probe|0")
        g.wait_for(r"verb=confuse effect=START screens=2", 6)
        hold("down", 7.5)
        time.sleep(1.5)
        g.drop("smite||probe|0")
        g.wait_for(r"verb=smite killed=\d+", 6)
        g.drop("heal|999|probe|0")
        g.wait_for(r"verb=heal hp=", 6)
        hold("up", 4.5)
        time.sleep(1.5)
        ok_b = check("down 7.5 + up 4.5 (full cycle)")
        g.drop("refill||probe|0")
        g.wait_for(r"verb=refill", 6)
        print("summary: A=%s B=%s" % (ok_a, ok_b))
    finally:
        rig.release_all()
        g.stop()


if __name__ == "__main__":
    main()

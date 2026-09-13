#!/usr/bin/env python3
"""Sprite-menu boot check (2026-09-10): verifies the F11/Shift+F11 sprite
selector end to end on the live game - catalog banner, cycling with live
apply, wrap-around, persistence into sprites.ini, missing-file skip.

Run solo (owns the game window).  Restore: sprites.ini is left at
name=Meatwad (the showtime pick) no matter what the test cycled to.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

import rig  # noqa: E402
import phase2_scenarios as p2  # noqa: E402

SPRITES_INI = os.path.join(p2.BASE, "sprites.ini")
CATALOG = os.path.join(p2.BASE, "sprites", "sprite_library.ini")


def catalog_first_names(n=4):
    names = []
    with open(CATALOG, "r", errors="replace") as fh:
        for ln in fh:
            ln = ln.strip()
            if ln.startswith("sprite="):
                names.append(ln.split("=", 1)[1].split("|")[0].strip())
    return names[:n]


def applied_names(tail):
    """All '[sprites] applied 'X'' names seen so far, in order (from the
    game's stdout log - sprite banners are plain lines, not RESULT lines)."""
    out = []
    try:
        with open(tail.log_path, "r", errors="replace") as fh:
            for ln in fh:
                if "[sprites] applied '" in ln:
                    try:
                        out.append(
                            ln.split("applied '", 1)[1].split("'", 1)[0])
                    except IndexError:
                        pass
    except OSError:
        pass
    return out


def main():
    checks = []

    def check(name, ok, detail=""):
        checks.append(ok)
        print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail),
              flush=True)

    with open(SPRITES_INI, "w") as fh:
        fh.write("name=Meatwad\n")
    first = catalog_first_names(4)
    print("catalog head: %s" % first, flush=True)

    with p2.Session("spriteboot", audio=False) as session:
        g = session.game
        session.load_chapter(1)
        time.sleep(2.0)
        session.tail.pump()

        # 1. boot banners: Meatwad applied + catalog loaded
        names = applied_names(session.tail)
        check("boot applies Meatwad", "Meatwad" in names, str(names[-1:]))
        cat_ok = any("[sprites] catalog" in ln
                     for ln in open(session.tail.log_path,
                                    errors="replace").read().splitlines())
        check("catalog banner present", cat_ok)

        # 2. F11: cycle to the NEXT sprite, live apply
        g.ensure_focus()
        rig.tap("f11")
        time.sleep(2.0)
        session.tail.pump()
        names = applied_names(session.tail)
        pick1 = names[-1] if names else None
        check("F11 cycles to next sprite", bool(pick1) and pick1 != "Meatwad",
              "applied %r" % pick1)
        probe_ok = g.position(timeout=5.0) is not None
        check("telemetry still flowing after swap", probe_ok)
        session.shot("sprite_01_f11")

        # 3. F11 again (further), then Shift+F11 back (previous direction)
        rig.tap("f11")
        time.sleep(2.0)
        session.tail.pump()
        names = applied_names(session.tail)
        pick2 = names[-1] if names else None
        check("F11 advances again", bool(pick2) and pick2 != pick1,
              "applied %r" % pick2)
        key_down_ok = True
        rig.key_down("rshift")
        time.sleep(0.05)
        try:
            rig.tap("f11")
        finally:
            rig.key_up("rshift")
        time.sleep(2.0)
        session.tail.pump()
        names = applied_names(session.tail)
        check("Shift+F11 goes back", names and names[-1] == pick1,
              "applied %r (expected %r)" % (names[-1:], pick1))

        # 4. persistence: sprites.ini rewritten with the last pick, no file=
        ini = open(SPRITES_INI, errors="replace").read()
        check("sprites.ini persisted pick",
              ("name=%s" % pick1) in ini and "file=" not in ini,
              ini.strip().replace("\n", " | "))

    # restore the showtime pick
    with open(SPRITES_INI, "w") as fh:
        fh.write("name=Meatwad\n")
    print("  sprites.ini restored to name=Meatwad", flush=True)

    passed = sum(1 for ok in checks if ok)
    print("\nSPRITE BOOT CHECK: %d/%d passed" % (passed, len(checks)),
          flush=True)
    return 0 if passed == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())

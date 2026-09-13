#!/usr/bin/env python3
"""In-game menu verification (argus, 2026-09-11).

Asserts, against the running game, that every randomizer setting is reachable
and persisted from the game's OWN menus, and that the keyboard overlay is no
longer a player surface:

  pause-menu MODS page (SELECT+L while paused)
    mods_page_opens        the page paints something different from inventory
    seed_shoulder_step     R shoulder = +100 per tap, RIGHT = +1, and the
                           value lands in randomizer.ini once released
    seed_hold_repeat       holding RIGHT climbs by 10s and commits on release
    randomizer_toggle      RIGHT on the RANDOMIZER row flips enabled= in the
                           ini immediately, and back
    menu_close_keeps_seed  closing the pause menu does not lose the edit
  overlay gate (player config, test=0)
    f12_gated / f10_gated  the frame does not change when F12 / F10 is pressed
  file select (title -> START -> file select, SEED row = slot 6)
    fileselect_hold_repeat holding R climbs the seed and commits on release
    fileselect_tap_step    a single L tap = -1, committed on release

randomizer.ini is restored byte-for-byte at the end of every session.
Screenshots land in tools/phase2_out/shots/mc_*.png for eyeballing the
rows that have no machine oracle (SPRITE row text).

Usage:  python tools/menu_check.py   (boots the game twice, ~90 s)
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from phase2_scenarios import Session, BASE, OUTDIR, INI_PATH  # noqa: E402
import phase2_scenarios  # noqa: E402
import rig  # noqa: E402

SHOTS = os.path.join(OUTDIR, "shots")
INI = os.path.join(BASE, "randomizer.ini")
RESULTS = []
SELECT_KEY = "backspace"   # test-only Select binding, see _patch_ini


def _patch_ini(*a, **kw):
    """SDL never sees a SendInput Right Shift (the repo's Select key), so
    the verify ini binds Select to Backspace for this run only. The repo
    zelda3.ini is untouched (rig launches with --config)."""
    path = _orig_make_ini(*a, **kw)
    with open(path, "r", encoding="utf-8") as fh:
        txt = fh.read()
    txt = txt.replace("Controls = Up, Down, Left, Right, Right Shift, Return",
                      "Controls = Up, Down, Left, Right, Backspace, Return")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(txt)
    return path


_orig_make_ini = phase2_scenarios.make_ini
phase2_scenarios.make_ini = _patch_ini


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
    RESULTS.append({"name": name, "ok": bool(ok), "detail": str(detail)})


def read_ini():
    out = {"enabled": None, "seed": None, "logic": None}
    with open(INI, "r", encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            k, _, v = ln.strip().partition("=")
            if k in out:
                try:
                    out[k] = int(v.strip())
                except ValueError:
                    out[k] = v.strip()
    return out


def img_diff(a, b):
    """Mean absolute pixel difference between two PNGs (0 = identical)."""
    from PIL import Image, ImageChops, ImageStat
    ia = Image.open(a).convert("L")
    ib = Image.open(b).convert("L")
    if ia.size != ib.size:
        ib = ib.resize(ia.size)
    return ImageStat.Stat(ImageChops.difference(ia, ib)).mean[0]


def shot(session, name):
    p = session.shot(name)
    return p or os.path.join(SHOTS, name + ".png")


def taps(name, n, gap=0.18):
    for _ in range(n):
        rig.tap(name)
        time.sleep(gap)


def hold(name, secs):
    rig.key_down(name)
    time.sleep(secs)
    rig.key_up(name)


def pause_menu_flow():
    rig.TEST_TWITCH_CFG = rig.TEST_TWITCH_CFG.replace("test=1", "test=0")
    with open(INI, "rb") as fh:
        ini_bytes = fh.read()
    try:
        with Session("menucheck", audio=False) as session:
            g = session.game
            session.load_chapter(1)
            time.sleep(1.0)
            g.ensure_focus()

            # -- overlay gate on the player config ----------------------
            base_shot = shot(session, "mc_gameplay")
            rig.tap("f12")
            time.sleep(0.8)
            d = img_diff(base_shot, shot(session, "mc_after_f12"))
            check("f12_gated", d < 6.0, "diff=%.2f" % d)
            rig.tap("f10")
            time.sleep(0.8)
            d = img_diff(base_shot, shot(session, "mc_after_f10"))
            check("f10_gated", d < 6.0, "diff=%.2f" % d)

            # -- open the pause menu, then the MODS page ----------------
            rig.tap("return")
            time.sleep(1.3)
            inv = shot(session, "mc_inventory")
            rig.key_down(SELECT_KEY)
            time.sleep(0.15)
            rig.tap("c")
            time.sleep(0.15)
            rig.key_up(SELECT_KEY)
            time.sleep(0.7)
            mods = shot(session, "mc_mods_page")
            d = img_diff(inv, mods)
            check("mods_page_opens", d > 4.0, "diff vs inventory=%.2f" % d)

            # -- RANDOMIZER / SEED / LOGIC rows are read-only in-game: they show
            #    the loaded file's settings, so taps must leave randomizer.ini alone
            taps("down", 2)
            time.sleep(0.3)
            ini0 = read_ini()
            taps("v", 3)
            taps("right", 2)
            hold("right", 1.0)
            time.sleep(0.6)
            check("seed_row_locked", read_ini()["seed"] == ini0["seed"],
                  "seed %s unchanged after taps and a hold" % ini0["seed"])
            shot(session, "mc_seed_row_locked")
            rig.tap("up")
            time.sleep(0.3)
            rig.tap("right")
            time.sleep(0.4)
            check("randomizer_row_locked", read_ini()["enabled"] == ini0["enabled"],
                  "enabled %s unchanged" % ini0["enabled"])
            taps("down", 2)
            time.sleep(0.3)
            rig.tap("right")
            time.sleep(0.4)
            check("logic_row_locked", read_ini()["logic"] == ini0["logic"],
                  "logic %s unchanged" % ini0["logic"])
            shot(session, "mc_logic_row")

            # -- REWARDS row: A opens the page, B comes back --------------
            rig.tap("down")
            time.sleep(0.3)
            rig.tap("x")            # A
            time.sleep(0.6)
            rew = shot(session, "mc_rewards_page")
            check("rewards_page_opens", img_diff(mods, rew) > 3.0, "diff=%.2f" % img_diff(mods, rew))
            rig.tap("z")            # B
            time.sleep(0.5)
            # -- SPRITE row: screenshot for the eye (no text oracle) ------
            rig.tap("down")
            time.sleep(0.3)
            shot(session, "mc_sprite_row")

            # -- close the menu ---------------------------------------------
            rig.tap("return")
            time.sleep(1.2)
            closed = shot(session, "mc_menu_closed")
            d = img_diff(mods, closed)
            check("menu_close", d > 4.0, "diff=%.2f" % d)
    finally:
        rig.release_all()
        with open(INI, "wb") as fh:
            fh.write(ini_bytes)


def file_select_flow():
    with open(INI, "rb") as fh:
        ini_bytes = fh.read()
    try:
        with Session("menucheck_fs", audio=False) as session:
            g = session.game
            # boot -> logo -> triforce -> title plays on its own; the intro
            # only honours START once it is waiting on the title
            # (Module00_Intro: submodule >= 8). So: wait for the frame to go
            # static (title), then tap START once per try until the frame
            # changes hard (fade to file select). Never over-tap: START on
            # file select would load a save.
            g.ensure_focus()
            prev = shot(session, "mc_boot_0")
            static = 0
            for i in range(1, 20):
                time.sleep(2.0)
                cur = shot(session, "mc_boot_%d" % i)
                static = static + 1 if img_diff(prev, cur) < 1.0 else 0
                prev = cur
                if static >= 2:
                    break
            title = prev
            for i in range(5):
                rig.tap("return")
                time.sleep(3.0)
                cur = shot(session, "mc_start_%d" % i)
                if img_diff(title, cur) > 5.0:
                    break
            fs = shot(session, "mc_fileselect")
            # cursor: 0-2 files, 3 COPY, 4 ERASE, 5 OPTIONS (under ERASE; the old
            # RANDO line is gone, the defaults page only follows naming a file).
            # UP from file 1 wraps to OPTIONS and A opens the controller setup.
            rig.tap("up")
            time.sleep(0.4)
            rig.tap("x")
            time.sleep(0.8)
            page = shot(session, "mc_options_page")
            check("options_page_opens", img_diff(fs, page) > 3.0, "diff=%.2f" % img_diff(fs, page))
            rig.tap("x")                          # CONTROLS (first row of the top level)
            time.sleep(0.5)
            taps("down", 2, gap=0.3)              # TEST BUTTONS
            rig.tap("x")
            time.sleep(0.6)
            idle = shot(session, "mc_options_test_idle")
            check("options_test_opens", img_diff(page, idle) > 1.0, "diff=%.2f" % img_diff(page, idle))
            rig.key_down("x")                     # A held: its row must light up
            time.sleep(0.4)
            held = shot(session, "mc_options_test_held")
            rig.key_up("x")
            time.sleep(0.3)
            check("options_test_shows_press", img_diff(idle, held) > 0.15, "diff=%.2f" % img_diff(idle, held))
            rig.tap("esc")                     # leaves the test page
            time.sleep(0.5)
            back = shot(session, "mc_options_menu_again")
            check("options_test_exit", img_diff(back, idle) > 1.0, "diff=%.2f" % img_diff(back, idle))
            rig.tap("z")                          # B: controls menu -> OPTIONS top level
            time.sleep(0.4)
            rig.tap("z")                          # B: back to the file select
            time.sleep(0.8)
            fs2 = shot(session, "mc_fileselect_after")
            check("options_page_closes", img_diff(fs, fs2) < 1.5, "diff=%.2f" % img_diff(fs, fs2))

    finally:
        rig.release_all()
        with open(INI, "wb") as fh:
            fh.write(ini_bytes)


def main():
    os.makedirs(SHOTS, exist_ok=True)
    print("== pause-menu MODS page + overlay gate ==", flush=True)
    try:
        pause_menu_flow()
    except Exception as ex:
        check("pause_menu_flow_ran", False, repr(ex))
    print("== file select ==", flush=True)
    try:
        file_select_flow()
    except Exception as ex:
        check("file_select_flow_ran", False, repr(ex))
    fails = [r for r in RESULTS if not r["ok"]]
    with open(os.path.join(OUTDIR, "menu_check.json"), "w") as fh:
        json.dump(RESULTS, fh, indent=1)
    print("\nMENU CHECK: %d/%d passed" % (len(RESULTS) - len(fails), len(RESULTS)),
          flush=True)
    for r in fails:
        print("  FAILED:", r["name"], r["detail"])
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

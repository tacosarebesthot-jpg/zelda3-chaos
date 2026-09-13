#!/usr/bin/env python3
"""OPTIONS page check (player select > OPTIONS): the VIDEO and GAME FEATURES
pages against the running game.

  options_opens          UP from file 1 wraps to OPTIONS, A opens the top level
  video_opens            DOWN, A opens the VIDEO list
  widescreen_off_ini     RIGHT on WIDESCREEN writes options.ini with 4:3
  widescreen_off_shot    ...and the frame changes (narrower picture)
  widescreen_on_ini      RIGHT again restores 16:9
  video_page_flips       R shows page 2 (a different frame), L comes back
  features_toggle_ini    GAME FEATURES > RIGHT on ITEM SWITCH writes ItemSwitchLR = 1
  options_cleanup        B B B returns to the player select

options.ini is deleted before and after the run so the repo config stays
what zelda3.ini says.  Same launch as menu_check (rig --config, no game
launches while the owner is on the box).
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
from phase2_scenarios import Session, BASE, OUTDIR  # noqa: E402
import phase2_scenarios  # noqa: E402
import rig  # noqa: E402
from PIL import Image, ImageChops, ImageStat  # noqa: E402

SHOTS = os.path.join(OUTDIR, "shots")
OPTIONS_INI = os.path.join(BASE, "options.ini")
RESULTS = []


def _patch_ini(*a, **kw):
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


def img_diff(a, b):
    ia = Image.open(a).convert("L")
    ib = Image.open(b).convert("L")
    if ia.size != ib.size:
        ib = ib.resize(ia.size)
    return ImageStat.Stat(ImageChops.difference(ia, ib)).mean[0]


def img_width(a):
    return Image.open(a).size[0]


def shot(session, name):
    p = session.shot(name)
    return p or os.path.join(SHOTS, name + ".png")


def read_options():
    out = {}
    if not os.path.exists(OPTIONS_INI):
        return out
    with open(OPTIONS_INI, encoding="utf-8") as fh:
        for ln in fh:
            ln = ln.strip()
            if "=" in ln and not ln.startswith("#") and not ln.startswith("["):
                k, v = ln.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def taps(name, n, gap=0.3):
    for _ in range(n):
        rig.tap(name)
        time.sleep(gap)


def flow():
    if os.path.exists(OPTIONS_INI):
        os.remove(OPTIONS_INI)
    try:
        with Session("optionscheck", audio=False) as session:
            g = session.game
            g.ensure_focus()
            prev = shot(session, "oc_boot_0")
            static = 0
            for i in range(1, 20):
                time.sleep(2.0)
                cur = shot(session, "oc_boot_%d" % i)
                static = static + 1 if img_diff(prev, cur) < 1.0 else 0
                prev = cur
                if static >= 2:
                    break
            title = prev
            for i in range(5):
                rig.tap("return")
                time.sleep(3.0)
                cur = shot(session, "oc_start_%d" % i)
                if img_diff(title, cur) > 5.0:
                    break
            fs = shot(session, "oc_fileselect")
            rig.tap("up")                       # wraps to OPTIONS
            time.sleep(0.4)
            rig.tap("x")
            time.sleep(0.8)
            top = shot(session, "oc_options_top")
            check("options_opens", img_diff(fs, top) > 3.0, "diff=%.2f" % img_diff(fs, top))

            rig.tap("down")                     # VIDEO
            time.sleep(0.3)
            rig.tap("x")
            time.sleep(0.8)
            video = shot(session, "oc_video")
            check("video_opens", img_diff(top, video) > 1.0, "diff=%.2f" % img_diff(top, video))

            w0 = img_width(video)
            rig.tap("right")                    # WIDESCREEN 16:9 -> 4:3
            time.sleep(1.0)
            opts = read_options()
            check("widescreen_off_ini", opts.get("ExtendedAspectRatio") == "4:3", "ini=%s" % opts.get("ExtendedAspectRatio"))
            narrow = shot(session, "oc_video_narrow")
            check("widescreen_off_shot", img_width(narrow) < w0 or img_diff(video, narrow) > 1.0,
                  "width %d -> %d" % (w0, img_width(narrow)))
            rig.tap("right")                    # back to 16:9
            time.sleep(1.0)
            opts = read_options()
            check("widescreen_on_ini", opts.get("ExtendedAspectRatio") == "16:9", "ini=%s" % opts.get("ExtendedAspectRatio"))
            wide = shot(session, "oc_video_wide")

            rig.tap("v")                        # R: page 2
            time.sleep(0.6)
            page2 = shot(session, "oc_video_page2")
            rig.tap("c")                        # L: page 1
            time.sleep(0.6)
            page1 = shot(session, "oc_video_page1")
            check("video_page_flips", img_diff(wide, page2) > 1.0 and img_diff(page2, page1) > 1.0,
                  "diff p2=%.2f back=%.2f" % (img_diff(wide, page2), img_diff(page2, page1)))

            rig.tap("z")                        # B: top level
            time.sleep(0.5)
            rig.tap("down")                     # GAME FEATURES
            time.sleep(0.3)
            rig.tap("x")
            time.sleep(0.8)
            rig.tap("right")                    # ITEM SWITCH on
            time.sleep(0.8)
            opts = read_options()
            check("features_toggle_ini", opts.get("ItemSwitchLR") == "1", "ini=%s" % opts.get("ItemSwitchLR"))
            rig.tap("right")                    # and off again
            time.sleep(0.5)

            taps("z", 2, gap=0.5)               # B: top level, B: player select
            time.sleep(0.8)
            back = shot(session, "oc_fileselect_after")
            check("options_cleanup", img_diff(fs, back) < 1.5, "diff=%.2f" % img_diff(fs, back))
    finally:
        rig.release_all()
        if os.path.exists(OPTIONS_INI):
            os.remove(OPTIONS_INI)


def main():
    os.makedirs(SHOTS, exist_ok=True)
    print("== options page ==", flush=True)
    flow()
    fails = [r for r in RESULTS if not r["ok"]]
    with open(os.path.join(OUTDIR, "options_check.json"), "w") as fh:
        json.dump(RESULTS, fh, indent=1)
    print("\nOPTIONS CHECK: %d/%d passed" % (len(RESULTS) - len(fails), len(RESULTS)),
          "FAIL: " + ", ".join(r["name"] for r in fails) if fails else "", flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

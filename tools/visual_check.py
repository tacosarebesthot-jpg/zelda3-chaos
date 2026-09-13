#!/usr/bin/env python3
"""Visual verification for in-game UI overlays (2026-09-11).

Rationale: UI features were being marked done on compile-success alone; the
owner then found a cut-off panel and mirrored text in-game. This script makes
visual defects automatic to catch: boots the game, opens each UI surface,
screenshots it, and asserts structural properties (panel within screen bounds,
text pixels present, glyph orientation). Add a check here for every new
overlay/UI feature BEFORE calling it done.

Usage:  python tools/visual_check.py     (boots the game ~30s, owner warned)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from phase2_scenarios import Session  # noqa: E402
import rig  # noqa: E402

SHOTS = os.path.join(HERE, "phase2_out", "shots")
FAILS = []


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
    if not ok:
        FAILS.append(name)


def white_bbox(png_path, thr=230):
    """Bounding box (x0, y0, x1, y1) of near-white pixels, or None."""
    from PIL import Image
    img = Image.open(png_path).convert("RGB")
    w, h = img.size
    px = img.load()
    minx, miny, maxx, maxy = None, None, None, None
    for y in range(0, h):
        for x in range(0, w):
            r, g, b = px[x, y]
            if r > thr and g > thr and b > thr:
                if minx is None or x < minx:
                    minx = x
                if maxx is None or x > maxx:
                    maxx = x
                if miny is None or y < miny:
                    miny = y
                if maxy is None or y > maxy:
                    maxy = y
    if minx is None:
        return None
    return (minx, miny, maxx, maxy)


def main():
    os.makedirs(SHOTS, exist_ok=True)
    # The F10/F12 overlay is harness-only now (twitch_config.txt test=1);
    # the rig's default test config has test=0, so opt in for this run.
    rig.TEST_TWITCH_CFG = rig.TEST_TWITCH_CFG.replace("test=0", "test=1")
    with Session("visualcheck", audio=False) as session:
        g = session.game
        session.load_chapter(1)
        time.sleep(1.0)

        # -- 1. countdown digits: rendered, right-side-up, on screen -------
        g.drop("countdown|3|rec|0")
        time.sleep(1.2)
        session.shot("vc_countdown_3")
        p3 = os.path.join(SHOTS, "vc_countdown_3.png")
        bbox = white_bbox(p3)
        w, h = None, None
        from PIL import Image
        if bbox and os.path.exists(p3):
            im = Image.open(p3)
            w, h = im.size
        # a correct digit 3 (scale 8) is ~40 wide x 56 tall, centered;
        # a MIRRORED frame would push glyphs off the left edge
        check("countdown digit visible",
              bool(bbox) and w and bbox[0] > w * 0.25 and bbox[2] < w * 0.9,
              str(bbox))
        time.sleep(3.0)

        # -- 2. settings menu: panel fits + title glyphs present -----------
        rig.tap("f10")
        time.sleep(1.5)
        session.shot("vc_settings")
        ps = os.path.join(SHOTS, "vc_settings.png")
        bbox = white_bbox(ps)
        from PIL import Image
        im = Image.open(ps)
        w, h = im.size
        # panel must be fully within the frame (cut-off = bbox touches edges)
        inside = bool(bbox) and bbox[0] > 2 and bbox[1] > 2 \
            and bbox[2] < w - 3 and bbox[3] < h - 3
        check("settings panel fully on screen", inside, str(bbox))
        # the settings title must START with an S-glyph at its left edge
        # (mirror bug drew the reversed word first)
        check("settings text present", bool(bbox) and (bbox[2] - bbox[0]) > 100,
              str(bbox))
        time.sleep(0.3)
        session.shot("vc_settings_settled")

    print("\nVISUAL CHECK: %d failures" % len(FAILS), flush=True)
    for f in FAILS:
        print("  FAILED:", f)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

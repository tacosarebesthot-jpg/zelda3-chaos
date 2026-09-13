#!/usr/bin/env python3
"""Automated orientation verification for the in-game countdown (2026-09-11).

Boots the game, triggers the debug countdown, captures each second, decodes
the rendered glyph from raw pixels, and asserts:
  - the decoded digit matches the expected countdown sequence
  - the glyph is NOT horizontally mirrored (normal orientation outscores the
    mirrored variant)

This is the programmatic replacement for eyeballing screenshots (owner rule:
verify your own work; screenshot evidence, not judgment).
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from PIL import Image  # noqa: E402
from phase2_scenarios import Session  # noqa: E402

# Must match src/twitch.c kTwDigits (bit0 = LEFTMOST column, 5x7)
DIGITS = {
    0: [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
    1: [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
    2: [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F],
    3: [0x1F, 0x10, 0x10, 0x1C, 0x10, 0x10, 0x1F],
    4: [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02],
    5: [0x1F, 0x01, 0x1F, 0x10, 0x10, 0x11, 0x0E],
    6: [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E],
    7: [0x1F, 0x10, 0x08, 0x04, 0x04, 0x04, 0x04],
    8: [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E],
    9: [0x0E, 0x11, 0x11, 0x0F, 0x10, 0x10, 0x0C],
}


def white_mask(png):
    img = Image.open(png).convert("RGB")
    w, h = img.size
    px = img.load()
    pts = []
    # center region only: the countdown draws center-screen; this excludes
    # the HUD (top bars, item boxes, LIFE text) whose whites poison the bbox
    xlo, xhi = int(w * 0.25), int(w * 0.75)
    ylo, yhi = int(h * 0.30), int(h * 0.75)
    for y in range(ylo, yhi):
        for x in range(xlo, xhi):
            r, g, b = px[x, y]
            # 250+: the countdown writes pure 0xFFFFFFFF white; room whites
            # (priest robes, chest silver) are dimmer and must not register
            if r > 250 and g > 250 and b > 250:
                pts.append((x, y))
    return pts, (w, h)


def glyph_grid(png):
    """Downsample the white-glyph bbox into a 5x7 cell grid (True = lit)."""
    pts, (w, h) = white_mask(png)
    if len(pts) < 200:                       # nothing meaningful drawn
        return None, None
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    cw = (x1 - x0 + 1) / 5.0
    ch = (y1 - y0 + 1) / 7.0
    grid = []
    for gy in range(7):
        row = []
        for gx in range(5):
            cx = x0 + gx * cw + cw / 2
            cy = y0 + gy * ch + ch / 2
            hit = sum(1 for (x, y) in pts
                      if abs(x - cx) <= cw / 2 and abs(y - cy) <= ch / 2)
            row.append(hit > 0)
        grid.append(row)
    return grid, (x0, y0, x1, y1)


def pattern(digit, mirrored):
    """Expected 5x7 bool grid for a digit (bit0 = leftmost unless mirrored)."""
    out = []
    for bits in DIGITS[digit]:
        row = []
        for gx in range(5):
            bit = gx if not mirrored else 4 - gx
            row.append(bool(bits & (1 << bit)))
        out.append(row)
    return out


def score(grid, pat):
    hits = same = total = 0
    for gy in range(7):
        for gx in range(5):
            if grid[gy][gx] == pat[gy][gx]:
                same += 1
            if grid[gy][gx]:
                hits += 1
            total += 1
    return same / total, hits


def decode(grid):
    """Best-matching digit + whether it looks mirrored."""
    best = (0, -1, False)
    for d in DIGITS:
        s_normal = score(grid, pattern(d, False))[0]
        s_mirror = score(grid, pattern(d, True))[0]
        if s_normal >= s_mirror and s_normal > best[1]:
            best = (d, s_normal, False)
        elif s_mirror > best[1]:
            best = (d, s_mirror, True)
    return best


def main():
    os.makedirs(os.path.join(HERE, "menu_shots", "verify_digits"),
                exist_ok=True)
    shots_dir = os.path.join(HERE, "phase2_out", "shots")
    fails = []
    with Session("verifydig", audio=False) as session:
        g = session.game
        session.load_chapter(1)
        time.sleep(0.8)
        g.drop("countdown|9|rec|0")
        for i in range(9):
            time.sleep(0.92)
            session.shot("verifydig_%02d" % (i + 1))

    expected = [9, 8, 7, 6, 5, 4, 3, 2, 1]
    for i, want in enumerate(expected):
        png = os.path.join(shots_dir, "verifydig_%02d.png" % (i + 1))
        if not os.path.exists(png):
            fails.append("shot %d missing" % (i + 1))
            continue
        grid, bbox = glyph_grid(png)
        if grid is None:
            fails.append("shot %d: no glyph captured (want %d)" % (i + 1, want))
            continue
        digit, conf, mirrored = decode(grid)
        ok = (digit == want) and not mirrored
        detail = "decoded %d conf %.2f mirrored=%s bbox=%s (want %d)" % (
            digit, conf, mirrored, bbox, want)
        print("  [%s] shot %02d: %s" % ("PASS" if ok else "FAIL", i + 1, detail),
              flush=True)
        if not ok:
            fails.append("shot %d: %s" % (i + 1, detail))

    print("\nDIGIT ORIENTATION VERIFY: %d issues" % len(fails))
    for f in fails:
        print("  FAILED:", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

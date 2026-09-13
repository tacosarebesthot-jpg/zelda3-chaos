#!/usr/bin/env python3
"""In-gameplay verification of the config-driven Link sprite selector
(src/sprites.c, sprites.ini).

Runs one boot of zelda3.exe using the same rig as run_verify.py (generated
verify ini with LoadRef enabled, twitch_config swapped for the drop/debug
test config and restored afterwards), loads reference-save chapter 2, walks
Link DOWN from the deterministic spawn point and captures screenshots into
tools/gameplay_verify/shots_sprites/<tag>_*.png.

Modes (select via --tag, this script never touches sprites.ini itself):
  --tag vanilla   no sprites.ini expected: title + gameplay + walk shots of
                  standard Link (also the revert test when run after a
                  sprite run with sprites.ini removed)
  --tag meatwad   sprites.ini with name=Meatwad expected: same shot set;
                  asserts the '[sprites] applied' log line and reports a
                  pixel diff of the Link crop vs the vanilla rest frame

Usage:
  python tools/gameplay_verify/run_sprites_verify.py --tag vanilla
  python tools/gameplay_verify/run_sprites_verify.py --tag meatwad

CAUTION: drives the real keyboard (SendInput). Don't touch the machine
mid-run; all held keys are released in a finally block.
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))            # repo root (zelda3/)
OUT_DIR = os.path.join(HERE, "shots_sprites")
INI_PATH = os.path.join(HERE, "zelda3_verify.ini")
RUN_LOG = os.path.join(HERE, "run_sprites.log")

BOOT_WAIT_S = 8.0        # task spec: boot 8 s before declaring alive
RELOAD_SETTLE_S = 4.0
WALK_S = 0.5             # DOWN hold from the fresh spawn (matches run_verify)
REST_S = 0.7             # skid-settle before the rest frame

CHAPTER = 2
LOADREF_KEY = rig.loadref_key(CHAPTER)
SLOT_MARKER = "*** Loading slot %d" % (256 + CHAPTER - 1)

# Link crop on the chapter-2 spawn screen (fractions; from run_verify.py).
LINK_BOX = (0.34, 0.15, 0.74, 0.44)


def ensure_foreground():
    if not rig.is_foreground(G.hwnd):
        if not rig.focus_window(G.hwnd, attempts=10):
            raise RuntimeError("lost window focus; aborting before key input")


def shoot(name):
    os.makedirs(OUT_DIR, exist_ok=True)
    p = G.shot(OUT_DIR, "%s_%s" % (TAG, name))
    rel = os.path.relpath(p, HERE)
    print("   shot: %s" % rel)
    return rel


def wait_gameplay(timeout=25.0):
    """Zero-side-effect gameplay probe: heal|0 only prints a non-DROPPED
    RESULT when the engine is in real gameplay."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        G.drop("heal|0|sprites-rig|0")
        line, _ = G.wait_for(r"verb=heal (hp=\d+->\d+|DROPPED)", 3.0)
        if line and "DROPPED" not in line:
            return line
    return None


def load_ref():
    before = os.path.getsize(RUN_LOG) if os.path.exists(RUN_LOG) else 0
    ensure_foreground()
    rig.tap(LOADREF_KEY)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        try:
            with open(RUN_LOG, "r", errors="replace") as fh:
                fh.seek(max(0, before - 64))
                if SLOT_MARKER in fh.read():
                    break
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    else:
        print("   [warn] no reload marker seen")
    time.sleep(RELOAD_SETTLE_S)
    return wait_gameplay()


def link_crop_diff(path_a, path_b):
    """Mean abs grayscale diff of the Link crop between two shots."""
    try:
        from PIL import Image, ImageChops, ImageStat
        a = Image.open(path_a).convert("L")
        b = Image.open(path_b).convert("L")
        box = (int(LINK_BOX[0] * a.width), int(LINK_BOX[1] * a.height),
               int(LINK_BOX[2] * a.width), int(LINK_BOX[3] * a.height))
        return ImageStat.Stat(ImageChops.difference(a.crop(box), b.crop(box))).mean[0]
    except Exception as ex:
        print("   [diff failed: %s]" % ex)
        return None


def main():
    global G, TAG
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True,
                    help="shot prefix, e.g. 'vanilla' or 'meatwad'")
    args = ap.parse_args()
    TAG = args.tag

    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI_PATH)
    ini_path = os.path.join(HERE, "zelda3_verify.ini")
    sprites_ini = os.path.join(BASE, "sprites.ini")
    want_sprite = TAG.lower() not in ("vanilla", "revert")
    print("sprites.ini present: %s (want: %s)" %
          (os.path.exists(sprites_ini), want_sprite))
    if os.path.exists(sprites_ini) != want_sprite:
        print("ERROR: sprites.ini state does not match --tag %s" % TAG)
        return 2

    G = rig.Game(BASE, RUN_LOG, os.path.relpath(ini_path, BASE))
    shots = {}
    applied = False
    t0 = time.time()
    try:
        G.start()
        print("window: 0x%X" % G.hwnd)
        time.sleep(BOOT_WAIT_S)
        shots["title"] = shoot("00_title")
        if G.proc.poll() is not None:
            print("FAIL: game exited during boot")
            return 1

        line = load_ref()
        if not line:
            print("FAIL: never reached gameplay after LoadRef")
            shots["fail"] = shoot("90_fail")
            return 1
        print("gameplay confirmed: %s" % line.strip())
        shots["gameplay"] = shoot("01_gameplay_spawn")

        # Deterministic walk: DOWN from the fresh spawn, mid + rest frames.
        time.sleep(0.3)
        ensure_foreground()
        rig.key_down("down")
        try:
            time.sleep(WALK_S * 0.5)
            shots["walk_mid"] = shoot("02_walk_mid")
            time.sleep(WALK_S * 0.5)
        finally:
            rig.key_up("down")
        time.sleep(REST_S)
        shots["walk_rest"] = shoot("03_walk_rest")

        # A second pose: sword swing on the rest position.
        ensure_foreground()
        rig.tap("x")
        time.sleep(0.35)
        shots["sword"] = shoot("04_sword_swing")

        with open(RUN_LOG, "r", errors="replace") as fh:
            log = fh.read()
        applied = "[sprites] applied" in log
        print("log: [sprites] applied line: %s" % applied)
    except Exception as ex:
        print("FAIL: %r" % ex)
        return 1
    finally:
        rig.release_all()
        G.stop()

    print("== run %s done in %.1f s; shots in %s" %
          (TAG, time.time() - t0, os.path.relpath(OUT_DIR, HERE)))

    # Cross-run comparison: sprite rest frame vs vanilla rest frame.
    # shoot() returns paths relative to HERE.
    def shot_path(key):
        return os.path.join(HERE, shots[key])

    vanilla_rest = os.path.join(OUT_DIR, "vanilla_03_walk_rest.png")
    if want_sprite and os.path.exists(vanilla_rest) and "walk_rest" in shots:
        d = link_crop_diff(vanilla_rest, shot_path("walk_rest"))
        print("LINK-CROP-DIFF vanilla vs %s rest: %s (want clearly > 1.0)"
              % (TAG, round(d, 2) if d is not None else "n/a"))
    elif not want_sprite and "walk_rest" in shots:
        prev_rest = [os.path.join(OUT_DIR, f)
                     for f in os.listdir(OUT_DIR)
                     if f.endswith("_03_walk_rest.png")
                     and os.path.abspath(os.path.join(OUT_DIR, f)) !=
                     os.path.abspath(shot_path("walk_rest"))]
        for other in prev_rest:
            d = link_crop_diff(other, shot_path("walk_rest"))
            print("LINK-CROP-DIFF %s vs %s rest: %s (want ~0.00)"
                  % (os.path.basename(other), TAG,
                     round(d, 2) if d is not None else "n/a"))

    if want_sprite and not applied:
        print("FAIL: sprite run did not apply the sprite ([sprites] applied missing)")
        return 1
    if not want_sprite and applied:
        print("FAIL: vanilla run applied a sprite (sprites.ini should be absent)")
        return 1
    print("PASS: %s run verified" % TAG)
    return 0


G = None
TAG = "run"

if __name__ == "__main__":
    sys.exit(main())

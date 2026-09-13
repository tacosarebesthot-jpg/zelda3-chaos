#!/usr/bin/env python3
"""Ice-floor coasting probe for the zelda3 Twitch 'ice' verb.

Measures Link's COASTING distance (displacement after input is released)
from exact position telemetry (RESULT probe lines emitted by twitch.c every
10 gameplay frames when debug=1), comparing:

  control  - no effect active: releasing the keys must stop Link (~0 px)
  ice      - ice active: releasing the keys must leave Link sliding
             (target >= 16 px, real-ice physics gives ~45 px)

Run twice per binary via the twitch_config.txt ice_legacy knob:
  ice_legacy=1 -> the OLD flag-only implementation (the regression baseline)
  ice_legacy=0 -> the current momentum-shadow implementation
Same binary, same room, same inputs - only the ice code path differs.

Scenario (per mode, per arm), reusing the run_verify.py patterns:
  1. launch zelda3.exe with tools/gameplay_verify/zelda3_verify.ini
  2. LoadRef key '2' -> saves/ref/"Chapter 2 ..." -> real gameplay
  3. reload ref again for a deterministic spawn
  4. hold UP 1.8 s: through the top door into the wide room above
     (same known-open floor the confuse probe uses; walked BEFORE the
     drop so both arms arrive with identical physics)
  5. settle 1.0 s; (ice arm) drop ice|600 (10 s) and wait for START
  6. screenshot, hold LEFT 0.1 s (short - statues flank the landing),
     release
  7. coast distance = last probe x after release vs final probe x (+1.8 s)

CAUTION: drives the real keyboard. Run only while the machine is idle;
all held keys are released in a finally block.

Usage: python tools/gameplay_verify/probe_ice.py [--modes legacy,new] [--hold 0.5]
"""
import os
import re
import sys
import time
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))          # repo root (zelda3/)
OUT_DIR = HERE
SHOTS = os.path.join(OUT_DIR, "shots_probe")
RUN_LOG = os.path.join(OUT_DIR, "probe.log")
INI_PATH = os.path.join(OUT_DIR, "zelda3_verify.ini")

BOOT_WAIT_S = 6.0
RELOAD_SETTLE_S = 4.0
DOOR_WALK_S = 1.8           # UP through the top door into the wide room
SETTLE_S = 1.0              # let go, let the scene stop moving
COAST_WAIT_S = 1.8          # post-release observation window

PROBE_RX = re.compile(r"^RESULT probe x=(-?\d+) y=(-?\d+) st=(\d+) ice=(\d+) vx=(\d+)$")


def read_probes(g):
    out = []
    with open(g.log_path, "r", errors="replace") as fh:
        for line in fh:
            m = PROBE_RX.match(line.strip())
            if m:
                out.append({"x": int(m.group(1)), "y": int(m.group(2)),
                            "st": int(m.group(3)), "ice": int(m.group(4)),
                            "vx": int(m.group(5))})
    return out


def ensure_foreground(g):
    if not rig.is_foreground(g.hwnd):
        if not rig.focus_window(g.hwnd, attempts=10):
            raise RuntimeError("lost window focus; aborting before key input")


def wait_gameplay(g, timeout=25.0):
    """heal|0 only prints a non-DROPPED RESULT in real gameplay."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        g.drop("heal|0|rig-probe|0")
        line, _ = g.wait_for(r"verb=heal (hp=\d+->\d+|DROPPED)", 3.0)
        if line and "DROPPED" not in line:
            return True
    return False


def reload_ref(g, chapter=2):
    before = os.path.getsize(g.log_path) if os.path.exists(g.log_path) else 0
    ensure_foreground(g)
    rig.tap(rig.loadref_key(chapter))
    marker = "*** Loading slot %d" % (256 + chapter - 1)
    deadline = time.time() + 10.0
    while time.time() < deadline:
        with open(g.log_path, "r", errors="replace") as fh:
            fh.seek(max(0, before - 64))
            if marker in fh.read():
                break
        time.sleep(0.2)
    else:
        print("    [warn] no reload marker seen")
    time.sleep(RELOAD_SETTLE_S)
    wait_gameplay(g)


def shot(g, name):
    try:
        p = g.shot(SHOTS, name)
        print("    [shot] %s" % os.path.basename(p))
        return p
    except Exception as e:
        print("    [shot failed: %s]" % e)
        return None


def run_arm(g, mode, label, hold_s):
    """One measurement arm. mode: 'control' or 'ice'. Returns dict of results."""
    res = {"mode": mode, "label": label, "coast": None, "release_x": None,
           "final_x": None, "max_vx": 0, "ice_seen": 0, "ok": False}
    reload_ref(g)

    # UP through the top door into the wide room above (confuse-probe path).
    # Walked BEFORE dropping ice so both arms arrive with identical legacy
    # physics (under the old ice code Link crawls and could never cross).
    ensure_foreground(g)
    rig.key_down("up")
    time.sleep(DOOR_WALK_S)
    rig.key_up("up")
    time.sleep(SETTLE_S)

    if mode == "ice":
        g.drop("ice||rig-ice|600")     # 600f = 10 s, outlives the measurement
        line, _ = g.wait_for(r"verb=ice effect=START frames=600", 8.0)
        if not line:
            print("    ice START not seen - aborting arm")
            return res
        time.sleep(0.3)                # let the first forced frames land

    before = read_probes(g)
    if not before:
        print("    no telemetry - aborting arm")
        return res
    shot(g, "%s_0_before_push" % label)

    # push LEFT briefly, then release and watch Link coast. The hold must be
    # SHORT: the room's statue bay is only ~40 px from the door landing in
    # every direction, and a long hold rams Link into a statue before the
    # release, leaving the coast no runway (measured in the first probe run:
    # x clamped at landing+40 with vx=384 still decaying against the wall).
    ensure_foreground(g)
    rig.key_down("left")
    time.sleep(hold_s)
    rig.key_up("left")
    release_lines = read_probes(g)          # taken immediately after release
    shot(g, "%s_1_released" % label)

    time.sleep(COAST_WAIT_S)
    final = read_probes(g)
    shot(g, "%s_2_after_coast" % label)

    rel = release_lines[-1] if release_lines else before[-1]
    fin = final[-1]
    pre = before[-1]
    res["release_x"] = rel["x"]
    res["final_x"] = fin["x"]
    # LEFT is the push direction: coast = how much farther left Link travelled
    # after the keys were released (positive = coasting)
    res["coast"] = rel["x"] - fin["x"]
    res["push_dist"] = pre["x"] - rel["x"]
    # corroborating metric independent of walls: integral of the seeded slide
    # velocity over the decay phase (vx=384 only while the key is held; the
    # 0 < vx < 384 samples are exactly the post-release coast) ->
    # distance ~= sum(vx_sample)*10/256 px (telemetry samples every 10 frames)
    res["vx_integral_px"] = sum(p["vx"] for p in final
                                if 0 < p["vx"] < 384) * 10 // 256
    for p in final:
        res["max_vx"] = max(res["max_vx"], p["vx"])
        res["ice_seen"] += 1 if p["ice"] else 0
    res["ok"] = True
    print("    push %dpx, release x=%d -> final x=%d  coast=%dpx  "
           "vx_integral=%dpx  max_vx=%d" % (
        res["push_dist"], res["release_x"], res["final_x"], res["coast"],
        res["vx_integral_px"], res["max_vx"]))
    return res


def run_mode(mode_cfg, hold_s):
    """One process launch with ice_legacy=mode_cfg; control then ice."""
    tag = "legacy" if mode_cfg else "new"
    print("== mode: ice_legacy=%d (%s build) ==" % (mode_cfg, tag))
    # replace (not append) any earlier knob so both lines never coexist
    rig.TEST_TWITCH_CFG = \
        rig.TEST_TWITCH_CFG.split("ice_legacy=")[0] + "ice_legacy=%d\n" % mode_cfg
    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI_PATH)
    os.makedirs(SHOTS, exist_ok=True)
    g = rig.Game(BASE, RUN_LOG, os.path.relpath(INI_PATH, BASE))
    out = []
    try:
        g.start()
        print("  window: 0x%X" % g.hwnd)
        time.sleep(BOOT_WAIT_S)
        ensure_foreground(g)
        rig.tap(rig.loadref_key(2))
        if not wait_gameplay(g):
            raise RuntimeError("never reached gameplay (LoadRef '2')")
        for mode in ("control", "ice"):
            print("  == arm: %s (%s)" % (mode, tag))
            try:
                out.append(run_arm(g, mode, "%s_%s" % (tag, mode), hold_s))
            except Exception as ex:
                print("    arm failed: %r" % ex)
            finally:
                rig.release_all()
            time.sleep(1.0)
    except Exception as ex:
        print("  mode failed: %r" % ex)
    finally:
        rig.release_all()
        g.stop()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modes", default="legacy,new",
                    help="comma list from {legacy,new} (default both)")
    ap.add_argument("--hold", type=float, default=0.1,
                    help="LEFT hold seconds before release (default 0.1)")
    args = ap.parse_args()

    results = []
    for m in args.modes.split(","):
        m = m.strip()
        if m == "legacy":
            results += run_mode(1, args.hold)
        elif m == "new":
            results += run_mode(0, args.hold)

    print("\n== ice coasting probe summary ==")
    print("%-14s %6s %8s %8s %8s %10s" % (
        "arm", "coast(px)", "push(px)", "rel_x", "final_x", "vx_int(px)"))
    for r in results:
        if r["ok"]:
            print("%-14s %6d %8d %8d %8d %10d" % (
                r["label"], r["coast"], r["push_dist"], r["release_x"],
                r["final_x"], r["vx_integral_px"]))
        else:
            print("%-14s   FAILED" % r["label"])

    by = {(r["label"]): r for r in results if r["ok"]}
    ok = True
    for tag in ("legacy", "new"):
        c = by.get("%s_control" % tag)
        i = by.get("%s_ice" % tag)
        if not c or not i:
            ok = False
            continue
        if tag == "legacy":
            # the flag-only implementation IS the bug baseline: ice coasting
            # at ~0 px here is the expected, confirming observation
            if i["coast"] < 16:
                print("BASELINE %s: legacy ice coast %dpx < 16px - the old "
                      "underdelivery, as expected" % (tag, i["coast"]))
            else:
                print("WARN %s: legacy ice coast %dpx >= 16px?! expected ~0" % (tag, i["coast"]))
        elif i["coast"] < 16:
            print("FAIL %s: ice coast %dpx < 16px target" % (tag, i["coast"]))
            ok = False
        else:
            print("PASS %s: ice coast %dpx >= 16px" % (tag, i["coast"]))
        if c["coast"] >= 16:
            print("FAIL %s: control coast %dpx >= 16px (should stop)" % (tag, c["coast"]))
            ok = False
        else:
            print("PASS %s: control coast %dpx < 16px (stops as expected)" % (tag, c["coast"]))
    if len(results) and "legacy_ice" in by and "new_ice" in by:
        print("improvement: legacy ice coast %dpx -> new ice coast %dpx" % (
            by["legacy_ice"]["coast"], by["new_ice"]["coast"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

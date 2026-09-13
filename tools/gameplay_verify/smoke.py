#!/usr/bin/env python3
"""Trustworthy boot/smoke test for zelda3.exe (rig hardening item 2).

Replaces the old "process didn't crash" boot check with real assertions:

  boot_window       game window appears and takes foreground
  telemetry         RESULT probe lines flow after LoadRef (gameplay proof,
                    zero side effects, replaces the old spawn-probe hack)
  frame_pacing      probe frame counter vs wall time in gameplay: ~60fps
                    (frame-delay pacing from the verify ini).  The window-
                    title FPS is the cost of one RENDER call (reads
                    thousands even when healthy) and is NOT the oracle.
  golden_state      post-title initial state matches the known-good
                    reference: Link at the chapter spawn (x=1272, y=568
                    +-4), handler state 0, dungeon module 7
  golden_placement  deterministic seed -> the pin must apply: seed 1234
                    puts Hookshot in Sanctuary (room 0x012 chest 0, item
                    id 10); asserted against BOTH the boot log's
                    [randomizer] lines and rando_placement.json
  no_error_lines    zero ERROR-pattern lines in the captured stdout
  clean_exit        WM_CLOSE -> SDL_QUIT -> exit code 0 (no hang, no crash)

Every failure prints a numbered reason and exits 1 (FAIL LOUDLY).

Usage:
  python tools/gameplay_verify/smoke.py [--seed 1234] [--chapter 1]
         [--pin-location Sanctuary] [--pin-item Hookshot] [--bootwait 8]
         [--fps-min 45] [--fps-max 80] [--no-rando] [--json OUT.json]
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))
INI = os.path.join(HERE, "zelda3_verify.ini")
RUN_LOG = os.path.join(HERE, "smoke_run.log")
RANDO_INI = os.path.join(BASE, "randomizer.ini")
PLACEMENT_JSON = os.path.join(BASE, "rando_placement.json")

# proven stream/harness error signatures (soak_final.py's list, plus
# unhandled-exception and generic error word)
ERR_PATTERNS = [
    r"unable to open", r"no playable tracks", r"connect failed",
    r"disconnected", r"reason=unknown", r"Segmentation", r"SIGSEGV",
    r"assert", r"ERROR", r"Fatal", r"unhandled", r"(?i)\berror\b",
]

# golden post-title initial state, chapter 1 ("Zelda's Rescue"): the save
# spawns Link standing in Sanctuary; probe telemetry of every healthy boot
# shows exactly this position and handler state 0 in dungeon module 7.
GOLDEN = {
    1: {"x": 1272, "y": 568, "tol": 4, "st": 0, "mod": 7, "room": 0x12},
}


def sample_fps(hwnd, seconds=2.5, interval=0.4, warmup=1.5):
    """Sample the window-title FPS for `seconds`.  WARNING: the title FPS is
    the measured cost of a single ZeldaDrawPpuFrame render call (see
    DrawPpuFrameWithPerf in main.c) - it reads THOUSANDS on a healthy
    machine and is NOT an emulation-pacing oracle.  Kept only for
    informational sampling; pacing asserts use probe frames instead."""
    time.sleep(warmup)
    samples = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        n = rig.user32.GetWindowTextLengthW(hwnd)
        if n > 0:
            buf = rig.ctypes.create_unicode_buffer(n + 1)
            rig.user32.GetWindowTextW(hwnd, buf, n + 1)
            m = re.search(r"\|\s*FPS:\s*(\d+)", buf.value)
            if m:
                samples.append(int(m.group(1)))
        time.sleep(interval)
    return samples


def check_placement(log_lines, seed, pin_location, expect_item):
    """Deterministic-seed golden check: boot log [randomizer] lines AND
    rando_placement.json must both show the pin applied to room 0x012 with
    the EXPECTED item (which the caller may have deliberately set wrong to
    prove the assertion fails loudly against real game output)."""
    fails = []
    seed_line = "[randomizer] enabled, seed=%d" % seed
    if not any(seed_line in l for l in log_lines):
        fails.append("boot log has no '%s'" % seed_line)
    pin_rx = (r"\[randomizer\] pin applied to chest record: room 0x012 "
              r"chest 0 = %s" % re.escape(expect_item))
    if not any(re.search(pin_rx, l) for l in log_lines):
        fails.append("boot log has no 'pin applied to chest record: "
                     "room 0x012 chest 0 = %s'" % expect_item)
    try:
        with open(PLACEMENT_JSON, "r", encoding="utf-8") as fh:
            placement = json.load(fh)
    except Exception as ex:
        fails.append("rando_placement.json unreadable: %r" % ex)
        return fails
    if placement.get("seed") != seed:
        fails.append("rando_placement.json seed=%r != %d"
                     % (placement.get("seed"), seed))
    pins = [p for p in placement.get("placements", [])
            if p.get("location") == pin_location]
    if not pins:
        fails.append("rando_placement.json has no '%s' placement"
                     % pin_location)
    elif pins[0].get("item") != expect_item or pins[0].get("origin") != "pin":
        fails.append("rando_placement.json %s = %r (origin %r), expected "
                     "%s origin=pin" % (pin_location, pins[0].get("item"),
                                        pins[0].get("origin"), expect_item))
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--chapter", type=int, default=1)
    ap.add_argument("--pin-location", default="Sanctuary")
    ap.add_argument("--pin-item", default="Hookshot")
    ap.add_argument("--expect-item", default=None,
                    help="golden item the placement check demands; defaults "
                    "to --pin-item. Set it DIFFERENT from --pin-item to "
                    "inject a real failure (assertion vs actual game output)")
    ap.add_argument("--bootwait", type=float, default=8.0)
    ap.add_argument("--fps-min", type=int, default=45)
    ap.add_argument("--fps-max", type=int, default=80)
    ap.add_argument("--no-rando", action="store_true",
                    help="skip the randomizer golden-placement check")
    ap.add_argument("--json", default=None, help="write results json here")
    args = ap.parse_args()

    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI,
                        load_ref=True, enable_audio=False, display_perf=True)
    results = []   # (name, ok, detail)

    def record(name, ok, detail):
        results.append((name, ok, detail))
        print("  [%s] %-16s %s" % ("PASS" if ok else "FAIL", name, detail),
              flush=True)

    print("== smoke: boot + real-state assertions ==")

    # crash-safe randomizer.ini swap (sidecar pattern, like twitch_config)
    rando_recovered = []
    rando_backup = None
    if not args.no_rando:
        rando_recovered = rig.config_recover([RANDO_INI])
        if rando_recovered:
            print("  [recover] restored %s from a leftover .rigbak sidecar "
                  "(previous run died mid-swap)"
                  % ", ".join(os.path.basename(p) for p in rando_recovered))
        with open(RANDO_INI, "r", encoding="utf-8") as fh:
            rando_backup = fh.read()
        rig.config_backup([RANDO_INI])
        with open(RANDO_INI, "w", encoding="utf-8") as fh:
            fh.write("enabled=1\nseed=%d\nlog=1\npin_location=%s\n"
                     "pin_item=%s\n" % (args.seed, args.pin_location,
                                        args.pin_item))

    g = rig.Game(BASE, RUN_LOG, os.path.relpath(INI, BASE))
    exit_code = None
    try:
        # -- 1 boot_window -------------------------------------------------
        t0 = time.time()
        try:
            g.start()
            n = rig.user32.GetWindowTextLengthW(g.hwnd)
            buf = rig.ctypes.create_unicode_buffer(n + 1) if n > 0 else None
            if buf:
                rig.user32.GetWindowTextW(g.hwnd, buf, n + 1)
            title_has_fps = bool(buf and "FPS:" in buf.value)
            record("boot_window", True,
                   "window up+focused in %.1fs (pid %d, DisplayPerf title %s)"
                   % (time.time() - t0, g.proc.pid,
                      "shows FPS" if title_has_fps else "MISSING FPS tag"))
        except Exception as ex:
            record("boot_window", False, "start() failed: %r" % ex)
            raise RuntimeError("cannot continue smoke without a window")

        # -- 3 telemetry / gameplay proof ------------------------------------
        try:
            p = g.load_chapter(args.chapter, settle=3.0, timeout=25.0)
            record("telemetry", True, "probe flowing: %r" % p)
        except Exception as ex:
            record("telemetry", False, "%r" % ex)
            p = None

        # -- 2 frame pacing (measured, probe-based) --------------------------
        # NOTE: the window-title FPS is the measured cost of ONE
        # ZeldaDrawPpuFrame call (main.c DrawPpuFrameWithPerf) - a healthy
        # machine reads THOUSANDS there, so it is NOT a pacing oracle.  The
        # real oracle is the probe frame counter vs wall time in gameplay.
        pace = g.measure_pace(2.0) if p else 0.0
        record("frame_pacing", args.fps_min <= pace <= args.fps_max,
               "emulation paced at %.1f fps over probes (allowed %d..%d; "
               "unlocked would read hundreds)"
               % (pace, args.fps_min, args.fps_max))

        # -- 4 golden initial state ------------------------------------------
        if p is None:
            record("golden_state", False, "no probe to check")
        else:
            golden = GOLDEN.get(args.chapter)
            if golden is None:
                record("golden_state", False,
                       "no golden values defined for chapter %d" % args.chapter)
            else:
                # settle first: the spawn drops Link in with ~2-4s of animated
                # descent; a fixed-time sample caught him mid-air (y drifted
                # 568->620->592 across runs). Wait for a stable position.
                sp = p
                stable = 0
                last = None
                deadline = time.time() + 15.0
                while stable < 6 and time.time() < deadline:
                    try:
                        sx, sy, sq = g.position(timeout=2.0)
                    except Exception:
                        time.sleep(0.2)
                        continue
                    if (sx, sy) == last:
                        stable += 1
                    else:
                        stable, last = 0, (sx, sy)
                    if stable >= 6:
                        sp = sq
                        break
                    time.sleep(0.2)
                dx = abs(sp.get("x", -9999) - golden["x"])
                dy = abs(sp.get("y", -9999) - golden["y"])
                ok = (dx <= golden["tol"] and dy <= golden["tol"]
                      and sp.get("st") == golden["st"]
                      and sp.get("mod") == golden["mod"]
                      and sp.get("r") == golden["room"])
                record("golden_state", ok,
                       "spawn probe x=%s y=%s st=%s mod=%s r=%s vs golden "
                       "(x=%d+-%d, y=%d+-%d, st=%d, mod=%d, room=0x%02X)"
                       % (sp.get("x"), sp.get("y"), sp.get("st"), sp.get("mod"),
                          sp.get("r"),
                          golden["x"], golden["tol"], golden["y"],
                          golden["tol"], golden["st"], golden["mod"],
                          golden["room"]))

        # -- 5 golden placement (deterministic seed) --------------------------
        if args.no_rando:
            record("golden_placement", True, "skipped (--no-rando)")
        else:
            time.sleep(1.0)   # let any trailing boot lines land
            expect_item = args.expect_item or args.pin_item
            with open(RUN_LOG, "r", errors="replace") as fh:
                log_lines = fh.read().splitlines()
            fails = check_placement(log_lines, args.seed,
                                    args.pin_location, expect_item)
            record("golden_placement", not fails,
                   "seed %d pin %s, expecting %s -> %s"
                   % (args.seed, args.pin_location, expect_item,
                      "log + rando_placement.json agree" if not fails
                      else "; ".join(fails)))

        # -- 6 zero ERROR-pattern lines ---------------------------------------
        with open(RUN_LOG, "r", errors="replace") as fh:
            log_txt = fh.read()
        hits = [(rx, ln.strip()) for rx in ERR_PATTERNS
                for ln in log_txt.splitlines() if re.search(rx, ln)]
        record("no_error_lines", not hits,
               "%d suspect lines in %d-line log%s"
               % (len(hits), log_txt.count("\n") + 1,
                  (": " + "; ".join("%s in %r" % h for h in hits[:3]))
                  if hits else " (clean)"))

        # -- 7 clean exit ------------------------------------------------------
        exit_code = g.close(timeout=15.0)
        if exit_code is None:
            record("clean_exit", False,
                   "game ignored WM_CLOSE for 15s - hang or blocked pump")
        else:
            record("clean_exit", exit_code == 0,
                   "WM_CLOSE -> exit code %d (0x%X)"
                   % (exit_code, exit_code & 0xFFFFFFFF))
    except Exception as ex:
        record("smoke_aborted", False, "%r" % ex)
    finally:
        g.stop()
        if rando_backup is not None:
            status = rig.config_restore([RANDO_INI])
            print("  [restore] randomizer.ini: %s"
                  % status.get("randomizer.ini", "?"))

    failed = [(n, d) for n, ok, d in results if not ok]
    print("== smoke verdict: %s (%d/%d checks passed) =="
          % ("FAIL" if failed else "PASS",
             len(results) - len(failed), len(results)))
    for n, d in failed:
        print("   FAIL %s: %s" % (n, d))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump({"verdict": "PASS" if not failed else "FAIL",
                       "results": [{"check": n, "ok": ok, "detail": d}
                                   for n, ok, d in results]}, fh, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

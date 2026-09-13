#!/usr/bin/env python3
"""Phase-B2 pin verification: boot zelda3.exe with the randomizer enabled,
capture the [randomizer] console lines + randomizer_log.txt, and snapshot
rando_placement.json for cross-boot determinism checks.  Boot-only (no keys
sent).  randomizer.ini is restored afterwards."""
import os, shutil, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig

BASE = os.path.dirname(os.path.dirname(HERE))
RUN_LOG = os.path.join(HERE, "run_pin_boot.log")
RANDO_INI = os.path.join(BASE, "randomizer.ini")

def boot_capture(seed, extra="", snapshot=None):
    with open(RANDO_INI, "r", encoding="utf-8") as fh:
        backup = fh.read()
    g = rig.Game(BASE, RUN_LOG, os.path.relpath(os.path.join(HERE, "zelda3_verify.ini"), BASE))
    try:
        with open(RANDO_INI, "w", encoding="utf-8") as fh:
            fh.write("enabled=1\nseed=%d\nlog=1\n%s" % (seed, extra))
        g.start()
        time.sleep(8.0)   # boot: LoadAssets -> Randomizer_Init -> ZeldaInitialize
    finally:
        g.stop()
        with open(RANDO_INI, "w", encoding="utf-8") as fh:
            fh.write(backup)
    with open(RUN_LOG, "r", errors="replace") as fh:
        lines = [l for l in fh.read().splitlines() if "[randomizer]" in l]
    log_txt = open(os.path.join(BASE, "randomizer_log.txt"), "r", errors="replace").read()
    if snapshot:
        shutil.copy(os.path.join(BASE, "rando_placement.json"), snapshot)
    return lines, log_txt

if __name__ == "__main__":
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1234
    snap = sys.argv[2] if len(sys.argv) > 2 else None
    lines, log_txt = boot_capture(seed, snapshot=snap)
    print("--- console lines ---")
    for l in lines: print(l)
    print("--- randomizer_log.txt head (first 8 lines) ---")
    print("\n".join(log_txt.splitlines()[:8]))
    print("--- pin lines in log ---")
    for l in log_txt.splitlines():
        if "pin" in l.lower(): print(l)

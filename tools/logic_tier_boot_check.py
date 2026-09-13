#!/usr/bin/env python3
"""Boot once per logic tier and prove the engine loaded THAT tier's rule
folder and produced a different fill from it (argus).

For each tier: write logic=<tier> into randomizer.ini, boot, clean-exit
(flushes stdout), then assert the boot banner names the tier folder, the
fill applied 164/164, and rando_placement.json differs between tiers with
different rule graphs (noglitches vs owglitches vs nologic). The seed pin
must hold on every tier. randomizer.ini is restored afterwards.
"""
import hashlib
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from phase2_scenarios import Session, BASE  # noqa: E402

INI = os.path.join(BASE, "randomizer.ini")
import sys as _s
TIERS = _s.argv[1].split(",") if len(_s.argv) > 1 else ["noglitches", "owglitches", "nologic"]


def set_logic(tier):
    with open(INI, "r", encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    out, seen = [], False
    for ln in lines:
        if ln.startswith("logic="):
            out.append("logic=" + tier); seen = True
        else:
            out.append(ln)
    if not seen:
        out.append("logic=" + tier)
    with open(INI, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(out) + "\n")


def placement_hash():
    p = os.path.join(BASE, "rando_placement.json")
    d = json.load(open(p))
    body = d.get("placements", d)
    return hashlib.md5(json.dumps(body, sort_keys=True).encode()).hexdigest()[:10]


def main():
    with open(INI, "rb") as fh:
        ini_bytes = fh.read()
    fails, hashes = [], {}
    try:
        for tier in TIERS:
            set_logic(tier)
            with Session("tier_" + tier, audio=False) as s:
                time.sleep(5.0)
                s.game.close(timeout=20)
                log = open(s.log_path, "r", errors="replace").read()
            m = re.search(r"\[randomizer\] enabled, seed=\d+ logic=(\w+) \(([^)]+)\)", log)
            applied = re.search(r"fill\+apply: (\d+)/(\d+) chest placements", log)
            pin = "pin applied to chest record: room 0x012 chest 0 = Hookshot" in log
            hashes[tier] = placement_hash()
            print("  %-12s banner=%s dir=%s applied=%s pin=%s fill=%s" % (
                tier, m.group(1) if m else None, m.group(2) if m else None,
                applied.group(0)[11:] if applied else None, pin, hashes[tier]), flush=True)
            if not m or m.group(1) != tier or not m.group(2).endswith("logic_" + tier):
                fails.append("%s: boot banner did not name the tier folder" % tier)
            if not applied or applied.group(1) != applied.group(2):
                fails.append("%s: fill did not apply every chest" % tier)
            if not pin:
                fails.append("%s: seed pin lost" % tier)
        if len(hashes) > 1 and len(set(hashes.values())) == 1:
            fails.append("all tiers produced an identical fill: tier folders are not changing the logic")
    finally:
        with open(INI, "wb") as fh:
            fh.write(ini_bytes)
    print("LOGIC TIER BOOT: %s" % ("PASS" if not fails else "FAIL " + "; ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

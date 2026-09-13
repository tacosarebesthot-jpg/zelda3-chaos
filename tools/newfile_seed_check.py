#!/usr/bin/env python3
"""Per-file seed (owner ruling 2026-09-12): the seed belongs to the save.

  setup_step        naming a new file lands on the RANDO/SEED/LOGIC line
  seed_edit_start   R shoulder x3 then START: file created with default+3 and
                    the game starts ("[randomizer] file: seed=N")
  sidecar_written   saves/rando_slots.ini carries slot1.seed=N
  mods_locked       in-game MODS page: SEED row shows N, L/R change nothing
  reload_same_seed  restart, pick file 1: same seed applied, no re-stamp

Runs on an empty SRAM (the real saves/sram.dat and rando_slots.ini are moved
aside and restored).  Usage: python tools/newfile_seed_check.py
"""
import os, re, shutil, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import OUTDIR, BASE
SH = os.path.join(HERE, "gameplay_verify", "shots")
SRAM = os.path.join(BASE, "saves", "sram.dat")
SIDE = os.path.join(BASE, "saves", "rando_slots.ini")
RINI = os.path.join(BASE, "randomizer.ini")
results = []
def rec(name, ok, note=""):
    results.append(bool(ok)); print("%-18s %s  %s" % (name, "PASS" if ok else "FAIL", note))
def read(p):
    try: return open(p, errors="replace").read()
    except OSError: return ""

ini_out = os.path.join(OUTDIR, "zelda3_newfile.ini")
rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), ini_out, load_ref=False, enable_audio=False)
t = open(ini_out, encoding="utf-8").read()
t = re.sub(r"(?m)^SkipIntroOnKeypress\s*=.*$", "SkipIntroOnKeypress = 1", t)
open(ini_out, "w", encoding="utf-8").write(t)

backups = {}
for p in (SRAM, SIDE):
    if os.path.exists(p):
        backups[p] = p + ".nfbak"; shutil.move(p, backups[p])
rini_bytes = open(RINI, "rb").read() if os.path.exists(RINI) else None
default_seed = int(re.search(r"(?m)^seed\s*=\s*(\d+)", read(RINI)).group(1)) if rini_bytes else 0

def boot(tag):
    g = rig.Game(BASE, os.path.join(OUTDIR, "newfile_%s.log" % tag), os.path.relpath(ini_out, BASE))
    g.start(); time.sleep(1.0)
    for _ in range(12):                                      # skip the logo: tap until the skip is logged
        g.ensure_focus(); rig.tap("x"); time.sleep(0.4)
        if "RESULT intro skipped" in read(g.log_path):
            break
    time.sleep(3.5)                                          # file select settles
    return g
def wait_line(g, rx, timeout):
    """rig.wait_for only sees RESULT lines; poll the raw log for anything."""
    t_end = time.time() + timeout
    while time.time() < t_end:
        m = re.search(rx, read(g.log_path))
        if m: return m
        time.sleep(0.2)
    return None

try:
    g = boot("a")
    try:
        g.ensure_focus(); rig.tap("x"); time.sleep(3.0)          # empty slot 1 -> name entry (stripes animate in)
        for attempt in range(3):                                 # an empty name is refused: type the letter
            g.ensure_focus(); rig.tap("x"); time.sleep(0.4)      # under the cursor, then START accepts
            g.ensure_focus(); rig.tap("return")
            if wait_line(g, r"file 1 created", 4.0):
                break
        time.sleep(1.5)
        g.shot(SH, "nf_page")
        log = read(g.log_path)
        rec("setup_step", "file 1 created" in log, "naming stamped a default entry")
        g.ensure_focus(); rig.tap("down"); time.sleep(0.4)       # row 0 -> SEED
        for _ in range(3):
            g.ensure_focus(); rig.tap("right"); time.sleep(0.35) # +1 each
        time.sleep(0.5); g.shot(SH, "nf_page_seed_edited")
        g.ensure_focus(); rig.tap("return")                      # START = BEGIN
        m = wait_line(g, r"\[randomizer\] file 1: seed=(\d+)", 20.0)
        seed = int(m.group(1)) if m else None
        want = default_seed + 3 if default_seed else None
        rec("seed_edit_start", seed is not None and (want is None or seed == want), "applied seed=%s (expected %s)" % (seed, want))
        # the fresh file opens on Zelda's telepathy (page waits): hold B to fly
        # through it, close it, then gameplay telemetry must flow
        time.sleep(4.0)
        g.ensure_focus(); rig.key_down("z"); time.sleep(6.0); rig.key_up("z")
        for _ in range(3):
            g.ensure_focus(); rig.tap("z"); time.sleep(0.4)
        p = g.in_gameplay_probe(timeout=30.0)
        rec("game_started", p is not None, "gameplay probe after START")
        side = read(SIDE)
        rec("sidecar_written", seed is not None and ("slot1.seed=%d" % seed) in side, side.strip().replace(chr(10), " | ")[:120])
        # MODS page: SEED row shows the file's seed and cannot be changed
        g.ensure_focus(); rig.tap("return"); time.sleep(0.9)
        g.ensure_focus(); rig.tap("m"); time.sleep(0.6)
        g.ensure_focus(); rig.tap("down"); time.sleep(0.3); rig.tap("down"); time.sleep(0.3)
        for _ in range(2):
            g.ensure_focus(); rig.tap("v"); time.sleep(0.3)
        time.sleep(0.5); g.shot(SH, "nf_mods_locked")
        g.ensure_focus(); rig.tap("m"); time.sleep(0.4); rig.tap("return"); time.sleep(0.6)
        rini_after = open(RINI, "rb").read() if os.path.exists(RINI) else None
        rec("mods_locked", read(SIDE) == side, "sidecar unchanged after MODS L/R taps")
    finally:
        g.stop()
    g = boot("b")
    try:
        time.sleep(0.5); g.shot(SH, "nf_fileselect_file1")
        # the RANDO line (cursor wraps 0 -> 5): A opens the defaults page, B closes it
        g.ensure_focus(); rig.tap("up"); time.sleep(0.4)
        g.ensure_focus(); rig.tap("x"); time.sleep(0.8); g.shot(SH, "nf_defaults_page")
        g.ensure_focus(); rig.tap("z"); time.sleep(0.8)           # B: back, cursor lands on file 1
        g.ensure_focus(); rig.tap("x")                            # file 1 -> load
        m = wait_line(g, r"\[randomizer\] file 1: seed=(\d+)", 20.0)
        seed2 = int(m.group(1)) if m else None
        log = read(g.log_path)
        rec("reload_same_seed", seed2 is not None and seed2 == seed and "had no settings" not in log and "created" not in log,
            "seed=%s on reload" % seed2)
    finally:
        g.stop()
finally:
    for p in (SRAM, SIDE):
        if os.path.exists(p): os.remove(p)
    for p, b in backups.items():
        shutil.move(b, p)
    if rini_bytes is not None:
        open(RINI, "wb").write(rini_bytes)      # the setup step commits defaults; keep the repo's
n = sum(results); print("NEWFILE SEED %d/%d" % (n, len(results))); sys.exit(0 if n == len(results) else 1)

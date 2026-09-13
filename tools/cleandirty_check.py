#!/usr/bin/env python3
"""TEXT row modes (owner 2026-09-12: "need an option for a clean and a dirty
script"). Uses the real dialogue.txt.

  clean_never_dirty   text.ini jokes=1: message 6 shown 8 times, every
                      "RESULT dialogue" line says dirty=0 mode=1
  dirty_can_curse     text.ini jokes=2: message 6 shown 20 times, at least
                      one pick is dirty=1 (6 has one tagged alternate)
  normal_off          text.ini jokes=0: no RESULT dialogue line at all
The real text.ini is put back afterwards.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import Session, BASE
TXT = os.path.join(BASE, "text.ini")
saved = open(TXT, "rb").read() if os.path.exists(TXT) else None
results = []
def rec(name, ok, note=""):
    results.append(bool(ok)); print("%-18s %s  %s" % (name, "PASS" if ok else "FAIL", note))
def picks(s, n):
    out = []
    for _ in range(n):
        s.game.ensure_focus(); s.game.fire_verb("msg|6|rig|0", None, timeout=8.0)
        line, _ = s.game.wait_for(r"RESULT dialogue msg=6 alt=(\d+) dirty=(\d+) mode=(\d+)", timeout=4.0)
        out.append(line)
        for _ in range(3):
            s.game.ensure_focus(); rig.tap("z"); time.sleep(0.3)
        time.sleep(0.6)
    return out
try:
    for mode, tag in ((1, "clean"), (2, "dirty"), (0, "normal")):
        open(TXT, "w").write("jokes=%d\n" % mode)
        with Session("cleandirty_" + tag, audio=False) as s:
            boot = open(s.log_path, errors="replace").read()
            s.load_chapter(1)
            n = 8 if mode == 1 else 20 if mode == 2 else 3
            got = picks(s, n)
            if mode == 1:
                ok = all(l and " dirty=0 mode=1" in l for l in got)
                rec("clean_never_dirty", ok, "%d picks, all clean" % n if ok else str(got))
            elif mode == 2:
                ok = any(l and " dirty=1 mode=2" in l for l in got)
                rec("dirty_can_curse", ok, "%d picks, dirty seen=%d" % (n, sum(1 for l in got if l and "dirty=1" in l)))
            else:
                rec("normal_off", all(l is None for l in got), "no override lines with jokes=0")
finally:
    if saved is None:
        if os.path.exists(TXT): os.remove(TXT)
    else:
        open(TXT, "wb").write(saved)
n = sum(results); print("CLEAN/DIRTY %d/%d" % (n, len(results))); sys.exit(0 if n == len(results) else 1)

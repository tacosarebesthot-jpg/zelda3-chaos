#!/usr/bin/env python3
"""Replay guard: chat effects must NOT run while a replay is playing (argus).

zelda_rtl.c now calls Twitch_PreFrame/Twitch_Tick only when !is_replay. Proof:
  1. load chapter 1, Shift+F1 = save state slot 1 (starts an input log)
  2. hold DOWN 2 s (recorded input), Shift+F1 again is NOT needed
  3. Ctrl+F1 = replay slot 1 (the engine replays the recorded 2 s)
  4. immediately drop hurt|8 : while replay_mode the drop is never drained,
     so NO "RESULT verb=hurt" line may appear during the replay window
  5. once the replay ends the queue drains: the RESULT line must appear

Usage: python tools/replay_guard_check.py
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from phase2_scenarios import Session  # noqa: E402
import phase2_scenarios  # noqa: E402
import rig  # noqa: E402

# Ctrl chords do not reach SDL through SendInput (Shift ones do), so the
# test-only ini binds slot-1 Save to y and Replay to u (unbound letters). Repo ini untouched.
rig.SC.setdefault("y", 0x15)
rig.SC.setdefault("u", 0x16)
rig.SC.setdefault("k", 0x25)   # ClearKeyLog: new recorder base = current gameplay state
_orig_make_ini = phase2_scenarios.make_ini


def _patch_ini(*a, **kw):
    path = _orig_make_ini(*a, **kw)
    out = []
    with open(path, "r", encoding="utf-8") as fh:
        for ln in fh:
            k = ln.split("=", 1)[0].strip()
            if k == "Save":
                out.append("Save = y\n")
            elif k == "Replay":
                out.append("Replay = u\n")
            else:
                out.append(ln)
    with open(path, "w", encoding="utf-8") as fh:
        fh.writelines(out)
    return path


phase2_scenarios.make_ini = _patch_ini


def hurt_lines(tail):
    tail.pump()
    return [ln for _t, ln in tail.events if re.search(r"verb=hurt", ln)]


def main():
    fails = []
    with Session("replay_guard", audio=False) as s:
        g = s.game
        s.load_chapter(1)
        time.sleep(1.0)
        g.ensure_focus()
        rig.tap("k")                     # ClearKeyLog: recorder base = here (in gameplay)
        time.sleep(0.5)
        rig.key_down("down"); time.sleep(3.0); rig.key_up("down")   # 3 s of recorded input
        time.sleep(0.5)
        rig.tap("y")                     # save slot 1 = base + that log
        time.sleep(0.5)
        rig.tap("u")                     # replay slot 1: back to base, replays ~3.5 s
        time.sleep(0.4)
        g.drop("hurt|8|rig|0")           # lands inside the replay window
        time.sleep(9.0)                  # replay ends, ticks resume, queue drains
        s.shot("rg_after_replay")
        # stdout is block-buffered in the game: only a clean exit flushes the
        # tail (the "*** Replaying" line and anything after it). Judge by
        # ORDER in the flushed log, not by wall-clock sampling.
        g.close(timeout=20)
        with open(s.log_path, "r", errors="replace") as fh:
            lines = fh.read().splitlines()
        i_rep = next((i for i, l in enumerate(lines) if "*** Replaying" in l), None)
        if i_rep is None:
            fails.append("no '*** Replaying' line: replay key never reached the engine")
        else:
            i_probe_after = next((i for i in range(i_rep + 1, len(lines)) if lines[i].startswith("RESULT probe")), None)
            hurts = [i for i, l in enumerate(lines) if "verb=hurt" in l]
            during = [i for i in hurts if i_rep < i < (i_probe_after if i_probe_after is not None else len(lines))]
            after = [i for i in hurts if i_probe_after is not None and i > i_probe_after]
            print("  replay at line %d, ticks resumed at line %s, hurt lines during=%d after=%d"
                  % (i_rep, i_probe_after, len(during), len(after)), flush=True)
            if i_probe_after is None:
                fails.append("ticks never resumed after the replay")
            if during:
                fails.append("chat verb applied DURING replay")
            if not after:
                fails.append("verb never applied after the replay ended")
            elif not any("hp=" in lines[i] for i in after):
                fails.append("verb drained after replay but not applied: %s" % lines[after[0]].strip())
    print("REPLAY GUARD: %s" % ("PASS" if not fails else "FAIL " + "; ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

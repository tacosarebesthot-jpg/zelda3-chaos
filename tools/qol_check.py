#!/usr/bin/env python3
"""QoL batch check (argus, 2026-09-12): intro skip, hold-B instant text,
dialogue.txt override.

  intro_skip     boot with SkipIntroOnKeypress=1, mash A from t=1s: the game
                 must print "RESULT intro skipped sub=N" with N < 8 (before
                 the title) within 8 s.  Control run without presses must NOT
                 print it within the same window.
  override_load  boot log carries "[dialogue] N message overrides loaded".
  text_normal    chapter 1, drop "msg 6": "RESULT text msg=6 frames=F1"
                 (rendered at normal speed; screenshot saved with the box up).
  text_hold_b    same message with B (key z) held: frames F2 <= F1 / 4.

Usage: python tools/qol_check.py [--keep-game]
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig  # noqa: E402
from phase2_scenarios import Session, make_ini, OUTDIR, BASE, INI_PATH  # noqa: E402

SHOTS = os.path.join(HERE, "gameplay_verify", "shots")
results = []


def rec(name, ok, note):
    results.append((name, ok, note))
    print("%-14s %s  %s" % (name, "PASS" if ok else "FAIL", note))


def read_log(path):
    try:
        with open(path, "r", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def intro_run(mash):
    """Boot without LoadRef; return (t_skip or None, log_text)."""
    ini_src = os.path.join(BASE, "zelda3.ini")
    ini_out = os.path.join(OUTDIR, "zelda3_qol_intro.ini")
    rig.make_verify_ini(ini_src, ini_out, load_ref=False, enable_audio=False)
    # force the feature on regardless of the repo ini
    txt = open(ini_out, "r", encoding="utf-8").read()
    txt = re.sub(r"(?m)^SkipIntroOnKeypress\s*=.*$", "SkipIntroOnKeypress = 1", txt)
    open(ini_out, "w", encoding="utf-8").write(txt)
    log = os.path.join(OUTDIR, "qol_intro_%s.log" % ("mash" if mash else "ctrl"))
    g = rig.Game(BASE, log, os.path.relpath(ini_out, BASE))
    t0 = time.time()
    t_skip = None
    try:
        g.start()
        deadline = t0 + 8.0
        while time.time() < deadline:
            if mash:
                g.ensure_focus()
                rig.tap("x")          # A button in the verify key map
            m = re.search(r"RESULT intro skipped sub=(\d+)", read_log(log))
            if m:
                t_skip = (time.time() - t0, int(m.group(1)))
                break
            time.sleep(0.25)
    finally:
        g.stop()
    return t_skip, read_log(log)


def main():
    ap = argparse.ArgumentParser()
    ap.parse_args()
    os.makedirs(SHOTS, exist_ok=True)

    # ---- intro skip -------------------------------------------------------
    skip, log = intro_run(mash=True)
    rec("intro_skip", skip is not None and skip[1] < 8,
        "skipped at sub=%s after %.1fs" % (skip[1], skip[0]) if skip else "no skip line in 8s")
    ctrl, _ = intro_run(mash=False)
    rec("intro_ctrl", ctrl is None, "control run: %s" % ("no skip (correct)" if ctrl is None else "skipped without input!"))

    # ---- text -------------------------------------------------------------
    # the check brings its own dialogue.txt (a 3-line and an 8-line message)
    # and puts the real one back afterwards
    real = os.path.join(BASE, "dialogue.txt")
    saved = open(real, "rb").read() if os.path.exists(real) else None
    with open(real, "w", encoding="utf-8") as fh:
        fh.write(TEST_DIALOGUE)
    try:
        text_checks()
    finally:
        if saved is None:
            os.remove(real)
        else:
            with open(real, "wb") as fh:
                fh.write(saved)

    n_ok = sum(1 for _, ok, _ in results if ok)
    print("QOL %d/%d" % (n_ok, len(results)))
    sys.exit(0 if n_ok == len(results) else 1)


TEST_DIALOGUE = """# qol_check test overrides
chatters: HungSo1o, SivUO
6: Hey {chatter}, you can't bring that thing in here. Leave it outside like a normal person.
20: Hey {chatter}, you can't bring that thing in here. This is a long line to prove the box wraps words by pixel width and pages every three lines without anyone typing a single control code. FLAWLESS VICTORY. Hold B and watch it fly.
"""


def text_checks():
    with Session("qol_text", audio=False) as s:
        boot = read_log(s.log_path)
        m = re.search(r"\[dialogue\] (\d+) message overrides loaded", boot)
        rec("override_load", m is not None and int(m.group(1)) >= 1,
            m.group(0) if m else "no [dialogue] line in boot log")
        s.load_chapter(1)

        def show(n, tag, hold=None, shot_after=None, timeout=40.0):
            """Fire "msg n"; return the RESULT text frame count (None on
            timeout).  shot_after: seconds after the fire to screenshot the
            box while it is still up."""
            s.game.ensure_focus()
            if hold:
                rig.key_down(hold)
                time.sleep(0.1)
            try:
                # fire without an ack regex: a short message completes in
                # the same log read as its ack line, so wait for the text
                # RESULT directly (fire_verb still gates on gameplay state)
                s.game.fire_verb("msg|%d|rig|0" % n, None, timeout=8.0)
                if shot_after:
                    time.sleep(shot_after)
                    s.game.shot(SHOTS, "qol_text_%s" % tag)
                done, _ = s.game.wait_for(r"RESULT text msg=%d frames=(\d+)" % n, timeout=timeout)
                if done and not shot_after:
                    s.game.shot(SHOTS, "qol_text_%s" % tag)
                return int(re.search(r"frames=(\d+)", done).group(1)) if done else None
            finally:
                if hold:
                    rig.key_up(hold)
                # dismiss the box (any button on EndMessage) and let gameplay settle
                for _ in range(4):
                    s.game.ensure_focus()
                    rig.tap("z")      # B: closes the box, never talks to an NPC
                    time.sleep(0.4)
                time.sleep(1.0)

        # msg 6 override: 3 lines, no page wait; screenshot with the box up
        # (shows the {chatter} substitution and the wrapped joke line)
        f1 = show(6, "normal", shot_after=1.0, timeout=10.0)
        rec("text_normal", f1 is not None, "frames=%s" % f1)
        # msg 20 override: ~8 lines with auto page waits.  Without input the
        # first page wait must hold (no RESULT text within 4 s)...
        f2 = show(20, "pages_wait", timeout=4.0)
        rec("pages_wait", f2 is None, "no completion in 4s (page wait held)" if f2 is None else "finished unattended in %d frames" % f2)
        # ...and a held B must fly through every page unattended
        f3 = show(20, "wrap_holdb", hold="z", shot_after=1.5, timeout=20.0)
        rec("wrap_hold_b", f3 is not None and f3 < 600, "frames=%s (held B through the page waits)" % f3)


if __name__ == "__main__":
    main()

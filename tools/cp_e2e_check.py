#!/usr/bin/env python3
"""Channel Points end to end, offline (argus).

  fake Twitch (test_server_mock.py)  ->  cp_listen.py --mock  ->  drop files
                                                 |
                              copied into the running game's twitch_drop/
                                                 |
                             RESULT lines prove the game applied each verb

Asserts the three mapped redemptions land in-game:
  "ARISE Chicken!! (ATHF)" -> arise            -> verb=cucco effect=START
  "Duck Vanishes"          -> spawn raven 2    -> verb=spawn ... count=2
  "So Disappointing"       -> tax 50           -> verb=tax rupees=A->B (B = A-50 or 0)
and that the two unmapped rewards produced no file (nothing extra reaches the game).

Usage: python tools/cp_e2e_check.py
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

from phase2_scenarios import Session, BASE  # noqa: E402

CP = os.path.join(HERE, "channel_points")
MOCK_OUT = os.path.join(CP, "mock_out")
RESULTS = []


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
    RESULTS.append(ok)


def expect(tail, rx, timeout=8.0):
    r = re.compile(rx)
    deadline = time.time() + timeout
    while time.time() < deadline:
        tail.pump()
        for _t, ln in tail.events:
            if r.search(ln):
                return ln
        time.sleep(0.05)
    return None


def run_listener_offline():
    for f in glob.glob(os.path.join(MOCK_OUT, "*.txt")):
        os.remove(f)
    mock = subprocess.Popen([sys.executable, os.path.join(CP, "test_server_mock.py")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        time.sleep(2.0)
        out = subprocess.run([sys.executable, os.path.join(CP, "cp_listen.py"), "--mock"],
                             capture_output=True, text=True, timeout=90).stdout
    finally:
        mock.terminate()
        try:
            mock.wait(5)
        except Exception:
            mock.kill()
    files = sorted(glob.glob(os.path.join(MOCK_OUT, "*.txt")))
    return out, files


def main():
    out, files = run_listener_offline()
    stats = re.search(r"stats: (\d+) received, (\d+) mapped, (\d+) ignored", out)
    check("listener_offline_run", stats is not None and stats.group(2) == "3" and stats.group(3) == "2",
          stats.group(0) if stats else "no stats line")
    check("three_drop_files_only", len(files) == 3, "%d files" % len(files))
    lines = {os.path.basename(f): open(f).read().strip() for f in files}
    for k, v in lines.items():
        print("    %s: %s" % (k, v))

    with Session("cp_e2e", audio=False) as s:
        s.load_chapter(1)
        time.sleep(1.0)
        drop_dir = os.path.join(BASE, "twitch_drop")
        os.makedirs(drop_dir, exist_ok=True)
        for f in files:
            shutil.copy2(f, os.path.join(drop_dir, os.path.basename(f)))
        check("cp_arise_applied", expect(s.tail, r"verb=cucco effect=START") is not None)
        ln = expect(s.tail, r"verb=spawn type=\d+ count=2")
        check("cp_spawn_raven2_applied", ln is not None, (ln or "none").strip())
        ln = expect(s.tail, r"verb=tax rupees=(\d+)->(\d+)")
        ok = False
        if ln:
            a, b = map(int, re.search(r"rupees=(\d+)->(\d+)", ln).groups())
            ok = b == max(0, a - 50)
        check("cp_tax50_applied", ok, (ln or "none").strip())
        time.sleep(0.5)
        leftover = glob.glob(os.path.join(drop_dir, "cp_*.txt"))
        check("drop_files_consumed", not leftover, "%d left" % len(leftover))
        s.shot("cp_e2e_end")
    print("CHANNEL POINTS E2E: %d/%d passed" % (sum(RESULTS), len(RESULTS)))
    return 0 if all(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())

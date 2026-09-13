#!/usr/bin/env python3
"""In-game Channel Points, end to end, offline (argus).

  test_server_mock.py (fake Helix + EventSub)  <-  src/channel_points.c (WinHTTP)
  scripted redemptions  ->  reward mapping  ->  Twitch_EnqueueExternal  ->  RESULT lines
  pause -> MODS -> REWARDS page: assign a verb with RIGHT, persisted in cp_rewards.ini

Config points the game at the mock through the harness-only twitch_config keys
cp_api / cp_ws; chat (IRC) stays disabled so no real Twitch is touched.
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))

import phase2_scenarios  # noqa: E402
from phase2_scenarios import Session, BASE  # noqa: E402
import rig  # noqa: E402

CP = os.path.join(HERE, "channel_points")
MAP = os.path.join(BASE, "cp_rewards.ini")
SELECT_KEY = "backspace"

rig.TEST_TWITCH_CFG = """\
token=mock-token-12345
user=mockcaster
channel=mockcaster
client_id=mock-client-id-67890
enabled=0
drop=1
debug=1
test=0
cooldown=1
effect_secs=5
vs_mode=0
cp_api=http://127.0.0.1:8931
cp_ws=ws://127.0.0.1:8932
"""

_orig_make_ini = phase2_scenarios.make_ini


def _patch_ini(*a, **kw):
    path = _orig_make_ini(*a, **kw)
    txt = open(path, "r", encoding="utf-8").read()
    txt = txt.replace("Controls = Up, Down, Left, Right, Right Shift, Return",
                      "Controls = Up, Down, Left, Right, Backspace, Return")
    open(path, "w", encoding="utf-8").write(txt)
    return path


phase2_scenarios.make_ini = _patch_ini
RESULTS = []


def check(name, ok, detail=""):
    print("  [%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
    RESULTS.append(ok)


def wait_log(path, rx, timeout):
    r = re.compile(rx)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            txt = open(path, "r", errors="replace").read()
        except OSError:
            txt = ""
        m = r.search(txt)
        if m:
            return m.group(0)
        time.sleep(0.1)
    return None


def main():
    saved_map = open(MAP, "rb").read() if os.path.exists(MAP) else None
    with open(MAP, "w", encoding="utf-8") as fh:   # two saved mappings; r-1 auto-maps by title, r-4 stays unmapped
        fh.write("r-2|swarm|Duck Vanishes\nr-3|tax|So Disappointing\n")
    mock = subprocess.Popen([sys.executable, os.path.join(CP, "test_server_mock.py")],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    time.sleep(1.5)
    try:
        with Session("cp_ingame", audio=False) as s:
            log = s.log_path
            s.load_chapter(1)
            check("cp_worker_started", wait_log(log, r"\[cp\] channel points worker started", 10) is not None)
            check("cp_rewards_listed", wait_log(log, r"\[cp\] 4 channel point reward\(s\) listed", 15) is not None)
            check("cp_online", wait_log(log, r"\[cp\] channel points online", 15) is not None)
            check("redeem_arise_automapped", wait_log(log, r"verb=cucco effect=START", 15) is not None)
            check("redeem_swarm_from_saved_map", wait_log(log, r"RESULT verb=swarm", 10) is not None)
            check("redeem_tax_from_saved_map", wait_log(log, r"verb=tax rupees=\d+->\d+", 10) is not None)
            check("unmapped_reward_logged", wait_log(log, r"no verb assigned", 10) is not None)

            # the REWARDS page: open it, move to the unmapped reward, RIGHT = HEAL
            s.game.ensure_focus()
            rig.tap("return"); time.sleep(1.3)
            rig.key_down(SELECT_KEY); time.sleep(0.15); rig.tap("c"); time.sleep(0.15); rig.key_up(SELECT_KEY)
            time.sleep(0.6)
            for _ in range(4):
                rig.tap("down"); time.sleep(0.18)
            rig.tap("x"); time.sleep(0.6)            # A -> REWARDS page
            s.shot("cpi_rewards_page")
            for _ in range(3):
                rig.tap("down"); time.sleep(0.18)     # r-4 is the 4th row
            rig.tap("right"); time.sleep(0.5)         # NONE -> HEAL
            s.shot("cpi_rewards_after")
            m = open(MAP, encoding="utf-8").read()
            check("rewards_page_assign_persisted", "r-4|heal|" in m, m.replace("\n", " / ")[:160])
            rig.tap("z"); time.sleep(0.4)             # B back to MODS
            rig.tap("return"); time.sleep(0.8)
    finally:
        mock.terminate()
        try:
            mock.wait(5)
        except Exception:
            mock.kill()
        if saved_map is None:
            if os.path.exists(MAP):
                os.remove(MAP)
        else:
            open(MAP, "wb").write(saved_map)
    print("CHANNEL POINTS IN-GAME: %d/%d passed" % (sum(RESULTS), len(RESULTS)))
    return 0 if all(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())

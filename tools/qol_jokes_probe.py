"""Boot with the real dialogue.txt: print its load line + any [dialogue]
warnings, then screenshot a few joke messages page by page."""
import os, sys, time, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import Session
SHOTS = os.path.join(HERE, "gameplay_verify", "shots")
PLAN = [(25, 4), (200, 1), (372, 6), (45, 4), (275, 3)]   # (msg, pages to shoot)
with Session("qol_jokes", audio=False) as s:
    time.sleep(1.0)
    log = open(s.log_path, errors="replace").read()
    for l in log.splitlines():
        if l.startswith("[dialogue]"):
            print(l)
    s.load_chapter(1)
    for n, pages in PLAN:
        s.game.ensure_focus()
        s.game.fire_verb("msg|%d|rig|0" % n, None, timeout=8.0)
        for pg in range(pages):
            time.sleep(2.2)
            s.game.shot(SHOTS, "joke_%d_p%d" % (n, pg + 1))
            s.game.ensure_focus(); rig.tap("x"); time.sleep(0.3)
        for _ in range(6):
            s.game.ensure_focus(); rig.tap("z"); time.sleep(0.35)
        time.sleep(1.0)
print("done")

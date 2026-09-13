"""New-game flow: skip the logo, pick file slot 1, accept the name, and
screenshot the opening narration (dialogue.txt 275-278 in the real intro
module) every 2.5 s.  Then in Sanctuary show the choice-style messages 25 and
45 page by page."""
import os, sys, time, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import Session, OUTDIR, BASE
SHOTS = os.path.join(HERE, "gameplay_verify", "shots")
ini_src = os.path.join(BASE, "zelda3.ini")
ini_out = os.path.join(OUTDIR, "zelda3_qol_intro.ini")
rig.make_verify_ini(ini_src, ini_out, load_ref=False, enable_audio=False)
txt = open(ini_out, encoding="utf-8").read()
txt = re.sub(r"(?m)^SkipIntroOnKeypress\s*=.*$", "SkipIntroOnKeypress = 1", txt)
open(ini_out, "w", encoding="utf-8").write(txt)
log = os.path.join(OUTDIR, "qol_intro_newgame.log")
g = rig.Game(BASE, log, os.path.relpath(ini_out, BASE))
try:
    g.start()
    time.sleep(1.0)
    for _ in range(4):
        g.ensure_focus(); rig.tap("x"); time.sleep(0.4)
    time.sleep(2.5)
    g.shot(SHOTS, "ng_00_fileselect")
    g.ensure_focus(); rig.tap("x"); time.sleep(1.5)        # slot 1 -> name entry
    g.shot(SHOTS, "ng_01_name")
    g.ensure_focus(); rig.tap("return"); time.sleep(1.5)   # accept
    g.shot(SHOTS, "ng_02_after_name")
    g.ensure_focus(); rig.tap("x"); time.sleep(1.5)        # in case a slot confirm is needed
    for i in range(14):
        time.sleep(2.5)
        g.shot(SHOTS, "ng_%02d_intro" % (3 + i))
finally:
    g.stop()
print("newgame done")

"""chatters.txt grows from live chat: a viewer who only says hello (no
command) over the fake IRC server must land in chatters.txt and in the
{chatter} pool.  Uses peruser_limits_check's FakeTwitch (127.0.0.1:16667).
Also re-checks the MODS page hotkey (M while paused) with a screenshot."""
import os, sys, time, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from peruser_limits_check import FakeTwitch, PORT    # sets rig.TEST_TWITCH_CFG (enabled=1, fake irc)
from phase2_scenarios import Session, BASE
SH = os.path.join(HERE, "gameplay_verify", "shots")
CH = os.path.join(BASE, "chatters.txt")
seed = open(CH, "rb").read()
ok_all = True
def check(name, ok, note=""):
    global ok_all; ok_all &= bool(ok); print("%-22s %s  %s" % (name, "PASS" if ok else "FAIL", note))
try:
    fake = FakeTwitch(); fake.start()
    with Session("chatterpool", audio=False) as s:
        s.load_chapter(1)
        check("fake_irc_login", fake.logged_in.wait(20))
        time.sleep(1.0)
        fake.say("asm0deus_probe", "hello chat, no command here")
        fake.say("StreamElements", "bot spam that must not be pooled")
        time.sleep(1.5)
        s.game.ensure_focus(); rig.tap("return"); time.sleep(0.9)
        s.game.ensure_focus(); rig.tap("m")
        l1, _ = s.game.wait_for(r"RESULT modspage via=hotkey now=1", timeout=4.0)
        time.sleep(0.6); s.game.shot(SH, "hk_mods_layout")
        s.game.ensure_focus(); rig.tap("m"); time.sleep(0.5)
        s.game.ensure_focus(); rig.tap("return"); time.sleep(0.6)
        check("hotkey_mods_page", l1 is not None)
    txt = open(CH, encoding="utf-8", errors="replace").read()
    check("chatter_persisted", "asm0deus_probe" in txt)
    check("bot_not_persisted", "StreamElements" not in txt)
finally:
    open(CH, "wb").write(seed)      # keep the shipped seed clean
print("CHATTER POOL", "PASS" if ok_all else "FAIL")
sys.exit(0 if ok_all else 1)

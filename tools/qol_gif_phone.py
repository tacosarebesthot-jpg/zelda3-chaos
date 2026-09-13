"""GIF of a fresh game's first message (Zelda's telepathy, dialogue.txt 32)
in Link's house: skip the logo, file slot 1, accept the name, record."""
import os, sys, time, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import OUTDIR, BASE
from PIL import Image
OUT = os.path.join(HERE, "gameplay_verify", "shots", "gifframes2")
os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT): os.remove(os.path.join(OUT, f))
ini_src = os.path.join(BASE, "zelda3.ini")
ini_out = os.path.join(OUTDIR, "zelda3_qol_intro.ini")
rig.make_verify_ini(ini_src, ini_out, load_ref=False, enable_audio=False)
txt = open(ini_out, encoding="utf-8").read()
txt = re.sub(r"(?m)^SkipIntroOnKeypress\s*=.*$", "SkipIntroOnKeypress = 1", txt)
open(ini_out, "w", encoding="utf-8").write(txt)
log = os.path.join(OUTDIR, "qol_gif_phone.log")
g = rig.Game(BASE, log, os.path.relpath(ini_out, BASE))
frames = []
def grab(secs, fps=5):
    t_end = time.time() + secs
    while time.time() < t_end:
        p = os.path.join(OUT, "f%04d.png" % len(frames))
        try:
            rig.capture_window(g.hwnd, p); frames.append(p)
        except Exception as e:          # minimized / covered for a moment: skip the frame
            print("frame skipped:", e)
        time.sleep(1.0 / fps)
try:
    g.start(); time.sleep(1.0)
    for _ in range(4):
        g.ensure_focus(); rig.tap("x"); time.sleep(0.4)
    time.sleep(2.0)
    g.ensure_focus(); rig.tap("x"); time.sleep(1.5)        # slot 1 -> name entry
    g.ensure_focus(); rig.tap("return"); time.sleep(0.5)   # accept the name
    grab(40.0)
finally:
    g.stop()
ims = [Image.open(f).convert("RGB") for f in frames]
w, h = ims[0].size
ims = [im.resize((w // 2, h // 2)).quantize(colors=96) for im in ims]
gif = os.path.abspath(os.path.join(HERE, "..", "..", "..", "briefs", "zelda3_jokes_phone3pct.gif"))
ims[0].save(gif, save_all=True, append_images=ims[1:], duration=200, loop=0, optimize=True)
print(gif, os.path.getsize(gif) // 1024, "KB", len(ims), "frames")

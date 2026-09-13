"""Replay the joke text on screen and record it as a GIF (owner asked)."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import Session
from PIL import Image
OUT = os.path.join(HERE, "gameplay_verify", "shots", "gifframes")
os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT): os.remove(os.path.join(OUT, f))
frames = []
def grab(n, secs, fps=6):
    t_end = time.time() + secs
    while time.time() < t_end:
        p = os.path.join(OUT, "f%04d.png" % len(frames))
        rig.capture_window(hwnd, p); frames.append(p)
        time.sleep(1.0 / fps)
with Session("qol_gif", audio=False) as s:
    hwnd = s.game.hwnd
    s.load_chapter(1)
    time.sleep(1.0)
    grab(0, 1.0)
    # 6: "Hey {chatter}, you can't bring that thing in here..."
    s.game.ensure_focus(); s.game.fire_verb("msg|6|rig|0", None)
    grab(6, 5.0)
    s.game.ensure_focus(); rig.tap("z"); time.sleep(0.5); grab(0, 1.0)
    # 25: "I sense that a mighty evil force guides the wizard's actions. It is called chat."  (4 pages)
    s.game.ensure_focus(); s.game.fire_verb("msg|25|rig|0", None)
    for pg in range(5):
        grab(25, 3.2)
        s.game.ensure_focus(); rig.tap("x"); time.sleep(0.2)
    for _ in range(4):
        s.game.ensure_focus(); rig.tap("z"); time.sleep(0.3)
    grab(0, 1.0)
ims = [Image.open(f).convert("RGB") for f in frames]
w, h = ims[0].size
ims = [im.resize((w // 2, h // 2)).quantize(colors=128) for im in ims]
gif = os.path.join(HERE, "..", "..", "..", "briefs", "zelda3_jokes_first_look.gif")
gif = os.path.abspath(gif)
ims[0].save(gif, save_all=True, append_images=ims[1:], duration=170, loop=0, optimize=True)
print(gif, os.path.getsize(gif) // 1024, "KB", len(ims), "frames")

"""Record the game window until the owner closes the game, then encode a GIF.
No key input is sent: the owner plays.  Frames are deduplicated (a repeated
frame just extends the previous frame's duration) to keep the GIF small.
Usage: python tools/qol_gif_record.py <out.gif> [fps]"""
import os, sys, time, re, hashlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig
from phase2_scenarios import OUTDIR, BASE
from PIL import Image
out_gif = os.path.abspath(sys.argv[1])
fps = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
FR = os.path.join(HERE, "gameplay_verify", "shots", "gifrec")
os.makedirs(FR, exist_ok=True)
for f in os.listdir(FR): os.remove(os.path.join(FR, f))
ini_src = os.path.join(BASE, "zelda3.ini")
ini_out = os.path.join(OUTDIR, "zelda3_qol_intro.ini")
rig.make_verify_ini(ini_src, ini_out, load_ref=False, enable_audio=True)
txt = open(ini_out, encoding="utf-8").read()
txt = re.sub(r"(?m)^SkipIntroOnKeypress\s*=.*$", "SkipIntroOnKeypress = 1", txt)
open(ini_out, "w", encoding="utf-8").write(txt)
log = os.path.join(OUTDIR, "qol_gif_record.log")
g = rig.Game(BASE, log, os.path.relpath(ini_out, BASE))
frames = []      # (path, duration_ms)
last_hash = None
t0 = time.time()
try:
    g.start()
    print("recording; close the game window when done", flush=True)
    while g.proc.poll() is None:
        t = time.time()
        p = os.path.join(FR, "f%05d.png" % len(frames))
        try:
            rig.capture_window(g.hwnd, p)
            h = hashlib.md5(open(p, "rb").read()).hexdigest()
            if h == last_hash:
                os.remove(p)
                frames[-1][1] += int(1000 / fps)
            else:
                frames.append([p, int(1000 / fps)])
                last_hash = h
        except Exception as e:
            pass   # minimized / covered: skip
        time.sleep(max(0.0, 1.0 / fps - (time.time() - t)))
finally:
    try: g.stop()
    except Exception: pass
print("captured %d distinct frames over %.0f s" % (len(frames), time.time() - t0), flush=True)
ims = []
for p, d in frames:
    im = Image.open(p).convert("RGB")
    w, h = im.size
    ims.append((im.resize((w // 2, h // 2)).quantize(colors=96), d))
if ims:
    ims[0][0].save(out_gif, save_all=True, append_images=[i for i, _ in ims[1:]],
                   duration=[d for _, d in ims], loop=0, optimize=True)
    print(out_gif, os.path.getsize(out_gif) // 1024, "KB", len(ims), "frames", flush=True)

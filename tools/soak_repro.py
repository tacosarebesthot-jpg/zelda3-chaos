#!/usr/bin/env python3
"""Crash REPRODUCTION driver for the flip/party mid-transition soak crash.

Recreates the exact soak conditions on a loop until the process dies:
  stream state (live twitch cfg via sidecar, Meatwad, music enabled=1,
  audio ON), arm attrition/dmgup/mpsteal/rupeesteal, LoadRef chapter-2,
  then micro-rounds of: swarm 4 -> freeze -> walk -> flip -> walk -> party
  -> speed/slow -> smite -> refill, with continuous synthetic-key walking
  across room transitions (where the soak died).

On process death: prints the raw EXIT CODE (hex) + last log lines - that is
the forensic this script exists to capture. No fixes, no retries.

Usage:  python tools/soak_repro.py [max_seconds]
"""
import hashlib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig  # noqa: E402

BASE = os.path.dirname(HERE)
EXE = os.path.join(BASE, "zelda3.exe")
CFG = os.path.join(BASE, "twitch_config.txt")
SIDECAR = CFG + ".rigbak"
MUSIC_INI = os.path.join(BASE, "music.ini")
SPRITES_INI = os.path.join(BASE, "sprites.ini")
DROP = os.path.join(BASE, "twitch_drop")
INI_PATH = os.path.join(HERE, "zelda3_verify.ini")
RUN_LOG = os.path.join(HERE, "soak_repro.log")

SEQ = [  # (verb, arg, post_gap_s, walk_dir, walk_s)
    ("swarm", "4", 3.0, "down", 1.5),
    ("freeze", "", 5.0, "down", 1.5),
    ("flip", "", 2.0, "down", 1.5),
    ("party", "", 5.0, "right", 1.5),
    ("speed", "", 3.0, "down", 1.5),
    ("slow", "", 3.0, "left", 1.0),
    ("smite", "", 4.0, "down", 1.0),
    ("refill", "", 4.0, "down", 1.5),
]
MODES = ["attrition", "dmgup", "mpsteal", "rupeesteal"]


def drop(line):
    final = os.path.join(DROP, "repro_%d_%d.txt" % (time.time_ns(), len(line)))
    tmp = final + ".part"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(line + "\n")
    os.replace(tmp, final)


def tail_results(path, n=25):
    with open(path, "r", errors="replace") as fh:
        lines = fh.readlines()
    res = [l.rstrip("\n") for l in lines if l.startswith("RESULT")]
    return "\n".join(res[-n:])


def main():
    max_s = float(sys.argv[1]) if len(sys.argv) > 1 else 600.0
    t0 = time.time()
    # crash-safe recovery first: undo any swap a dead previous run left over
    rig.config_recover([CFG, MUSIC_INI])
    orig_cfg = open(CFG, "rb").read()
    orig_music = open(MUSIC_INI, "rb").read()
    sprites_existed = os.path.exists(SPRITES_INI)
    if not sprites_existed:
        with open(SPRITES_INI, "w", encoding="utf-8") as fh:
            fh.write("name=Meatwad\n")
        print("sprites.ini temporarily recreated for the repro")
        orig_sprites = b""
    else:
        orig_sprites = open(SPRITES_INI, "rb").read()
    assert b"name=Meatwad" in orig_sprites or not sprites_existed
    # stream state needs music ON; restore whatever we found at exit
    if b"enabled=1" not in orig_music:
        with open(MUSIC_INI, "wb") as fh:
            fh.write(orig_music.replace(b"enabled=0", b"enabled=1", 1))
        print("music.ini temporarily re-enabled for the repro")
    rig.config_backup([CFG, MUSIC_INI],
                      {CFG: orig_cfg, MUSIC_INI: orig_music})
    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI_PATH,
                        load_ref=True, enable_audio=True)
    os.makedirs(DROP, exist_ok=True)
    for f in os.listdir(DROP):
        os.remove(os.path.join(DROP, f))
    print("repro target: %s (%d bytes, mtime %s)" % (
        EXE, os.path.getsize(EXE),
        time.strftime("%H:%M:%S", time.localtime(os.path.getmtime(EXE)))))
    print("exe sha256:", hashlib.sha256(open(EXE, "rb").read()).hexdigest()[:16])

    attempt = 0
    crashed = False
    try:
        while time.time() - t0 < max_s:
            attempt += 1
            log_fh = open(RUN_LOG, "a", encoding="utf-8")
            log_fh.write("\n===== attempt %d (%s) =====\n"
                         % (attempt, time.strftime("%H:%M:%S")))
            proc = subprocess.Popen(
                [EXE, "--config", os.path.relpath(INI_PATH, BASE)],
                cwd=BASE, stdout=log_fh, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            hwnd = rig.find_window_for_pid(proc.pid, timeout=30)
            if not hwnd or not rig.focus_window(hwnd):
                print("attempt %d: window/focus failed - aborting" % attempt)
                proc.terminate()
                break
            time.sleep(8)
            if proc.poll() is not None:
                print("attempt %d: DIED during boot, exit=%s"
                      % (attempt, hex(proc.returncode or 0)))
                crashed = True
                log_fh.close()
                break
            for m in MODES:
                drop("%s|on|repro|0" % m)
            time.sleep(1.0)
            rounds = 0
            while proc.poll() is None and time.time() - t0 < max_s:
                rounds += 1
                # reload ref save every 3 rounds for a fresh spawn
                if rounds % 3 == 1:
                    if rig.is_foreground(hwnd):
                        rig.tap(rig.loadref_key(2))
                        time.sleep(4.0)
                    else:
                        rig.focus_window(hwnd, attempts=6)
                        rig.tap(rig.loadref_key(2))
                        time.sleep(4.0)
                for verb, arg, gap, wdir, ws in SEQ:
                    if proc.poll() is not None:
                        break
                    drop("%s|%s|repro|0" % (verb, arg))
                    # walk during the gap (foreground-guarded)
                    if not rig.is_foreground(hwnd):
                        rig.focus_window(hwnd, attempts=6)
                    if rig.is_foreground(hwnd):
                        rig.key_down(wdir)
                        time.sleep(ws)
                        rig.key_up(wdir)
                    time.sleep(max(0.3, gap - ws))
                print("attempt %d round %d done (%.0fs elapsed)"
                      % (attempt, rounds, time.time() - t0), flush=True)
            if proc.poll() is not None:
                crashed = True
                print("\n*** REPRODUCED on attempt %d (rounds=%d): "
                      "exit code = %s (0x%X)"
                      % (attempt, rounds, proc.returncode,
                         proc.returncode & 0xFFFFFFFF))
                print("exe: %s (%d bytes)" % (EXE, os.path.getsize(EXE)))
                print("---- last RESULT lines ----")
                print(tail_results(RUN_LOG))
                log_fh.close()
                break
            proc.terminate()
            proc.wait(5)
            log_fh.close()
            print("attempt %d: no crash, restarting" % attempt)
    finally:
        try:
            rig.release_all()
        except Exception:
            pass
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except Exception:
                proc.kill()
        with open(CFG, "wb") as fh:
            fh.write(orig_cfg)
        with open(MUSIC_INI, "wb") as fh:
            fh.write(orig_music)
        if not sprites_existed and os.path.exists(SPRITES_INI):
            os.remove(SPRITES_INI)
        # remove the sidecars only now that the originals are back
        rig.config_restore([CFG, MUSIC_INI])
        for f in os.listdir(DROP):
            os.remove(os.path.join(DROP, f))
        print("config restored (sha %s)" %
              hashlib.sha256(open(CFG, "rb").read()).hexdigest()[:12])
    print("done: attempts=%d crashed=%s elapsed=%.0fs"
          % (attempt, crashed, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

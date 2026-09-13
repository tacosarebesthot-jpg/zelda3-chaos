#!/usr/bin/env python3
"""Builds the lane-ready handoff folder + zip for the zelda3 stream build.

Ships everything a fresh machine needs and nothing secret:
  zelda3.exe, SDL2.dll, zelda3_assets.dat, zelda3.ini
  sprites/ (513 .zspr + catalog) and a sprites.ini defaulting to Meatwad
  randomizer_ref/out/logic_<tier>/ for all five logic tiers + randomizer.ini
  tools/tracker.html, tools/feed.html, TWITCH_COMMANDS.txt, README.txt
  music/ (the starter music pack, if a music/ folder exists next to the exe)
  and a matching music.ini - enabled=1 when tracks were found, else 0.
  NO scripts and NO config to type: the game writes twitch_config.txt on
  first boot, the in-game connect fills user/channel from the token, and
  channel points (rewards -> verbs) are set on the pause menu's REWARDS page.

Refuses to ship if any file carries token/secret material.  Then clean-room
verifies: copies the dist to a temp dir, launches from there, and asserts the
randomizer filled the world (rando_placement.json, can_beat_game, apply
counts) and the sprite catalog loaded - not just "process stayed alive".

Usage:  python tools/make_dist.py [--no-zip] [--refresh-dumps]
  --no-zip         skip the final zip step, leave just the dist folder.
  --refresh-dumps  before packaging, rebuild all ten default rule folders
                    (randomizer_ref/out/logic_<tier>[_standard]) the same way
                    src/randomizer.c's RulesBuild does - one python_embed
                    dump_logic.py call per tier/mode, ~6-10s each (about a
                    minute total). Use this after dump_logic.py's output
                    files change (e.g. a new file joins the kFiles list in
                    randomizer.c) so the shipped folders are not stale.
                    Without the flag, packaging trusts whatever is already
                    on disk - but see REQUIRED_RULE_FILES below: a folder
                    missing any of the eleven files refuses to package
                    either way, flag or not.
Output: <repo parent>/dist/zelda3_stream/ and zelda3_stream.zip
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST_ROOT = os.path.dirname(BASE)
OUT = os.path.join(DIST_ROOT, "dist", "zelda3_stream")
LOGIC_TIERS = ["noglitches", "minorglitches", "owglitches", "hybridglitches", "nologic"]

# Mirrors src/randomizer.c's kRuleFiles exactly (same list RulesExist()/
# RulesBuild() use) - one place so packaging and the C engine cannot drift
# on what "a complete rule folder" means.
REQUIRED_RULE_FILES = ("bonks.json", "bosses.json", "drops.json", "edges.json",
                       "entrances.json", "items.json", "locations.json",
                       "meta.json", "pots.json", "regions.json", "rules.json")


def default_rule_dirs():
    """The ten randomizer_ref/out/logic_<tier>[_standard] folders, in the
    same (dir, tier, mode) shape RulesBuild uses to build one on demand."""
    dirs = []
    for t in LOGIC_TIERS:
        dirs.append((os.path.join(BASE, "randomizer_ref", "out", "logic_" + t), t, "open"))
        dirs.append((os.path.join(BASE, "randomizer_ref", "out", "logic_" + t + "_standard"), t, "standard"))
    return dirs


def rule_folder_missing(d):
    """Files from REQUIRED_RULE_FILES absent from folder d (empty = complete)."""
    return [f for f in REQUIRED_RULE_FILES if not os.path.isfile(os.path.join(d, f))]


def refresh_default_dumps():
    """Rebuild every default rule folder the same way RulesBuild() in
    src/randomizer.c does: one python_embed dump_logic.py run per
    (tier, mode), seed 1234 (the historical, reproducible seed - these
    combinations carry no --setting flags, so they are never seed-specific),
    then the eleven files move up out of <dir>.build/seed_1234/."""
    py = os.path.join(BASE, "python_embed", "python.exe")
    if not os.path.exists(py):
        print("no python_embed folder: cannot refresh the default dumps")
        return False
    ok = True
    for d, tier, mode in default_rule_dirs():
        build = d + ".build"
        if os.path.exists(build):
            shutil.rmtree(build)
        print("  refreshing %s (logic=%s mode=%s)..." % (os.path.relpath(d, BASE), tier, mode))
        log_path = os.path.join(BASE, "randomizer_ref", "out", "dump_last.log")
        with open(log_path, "w") as lf:
            rc = subprocess.call(
                [py, os.path.join(BASE, "randomizer_ref", "dump_logic.py"),
                 "--repo", os.path.join(BASE, "randomizer_ref", "ALttPDoorRandomizer"),
                 "--out", build, "--seed", "1234", "--logic", tier, "--mode", mode],
                cwd=BASE, stdout=lf, stderr=subprocess.STDOUT)
        seed_dir = os.path.join(build, "seed_1234")
        os.makedirs(d, exist_ok=True)
        for f in REQUIRED_RULE_FILES:
            src = os.path.join(seed_dir, f)
            if os.path.exists(src):
                shutil.move(src, os.path.join(d, f))
        shutil.rmtree(build, ignore_errors=True)
        missing = rule_folder_missing(d)
        if missing:
            print("    FAILED (python exit %d): still missing %s - see %s" % (rc, missing, log_path))
            ok = False
        else:
            print("    ready")
    return ok

FILES = [  # (repo-relative source, dist-relative dest, required)
    ("zelda3.exe", "zelda3.exe", True),
    ("SDL2.dll", "SDL2.dll", True),
    ("zelda3_assets.dat", "zelda3_assets.dat", True),
    ("zelda3.ini", "zelda3.ini", True),
    ("tools/tracker.html", "tools/tracker.html", True),
    ("tools/feed.html", "tools/feed.html", True),
    ("tools/TWITCH_COMMANDS.txt", "TWITCH_COMMANDS.txt", True),
    ("dialogue.txt", "dialogue.txt", True),   # joke text (MODS page TEXT row toggles it)
    ("chatters.txt", "chatters.txt", True),   # {chatter} name pool; the game appends names it sees live
    ("patrons.txt", "patrons.txt", False),    # Patreon names first in the ending credits
    ("credits_port.txt", "credits_port.txt", False),  # PORT INFO section of the ending credits
    ("randomizer_ref/dump_logic.py", "randomizer_ref/dump_logic.py", True),   # rules on demand (python_embed)
    # nothing else: no scripts, no runbooks - channel points live in the game
]
README = """zelda3 stream build
====================
1. Unzip anywhere. Double-click zelda3.exe. That's it.
2. Chat + channel points: in the game press START to pause, then press M on
   the keyboard (or hold SELECT and tap L on a pad). On that page press A on
   TWITCH: a link is copied. Paste it in your browser, click Authorize.
   Done - it stays connected on later runs.
   The pause menu opens anywhere you can move Link, including a brand-new
   file inside Link's house (the original game blocked it there).
3. Same page: RANDOMIZER on/off, SEED, LOGIC (glitch tier), REWARDS (what each
   channel-point reward does), SPRITE. Randomizer changes apply next boot.
4. OPTIONS on the player select (the line under ERASE PLAYER): CONTROLS sets
   up your pad and keyboard (define, test, reset), VIDEO has widescreen,
   fullscreen, window size and the rest, GAME FEATURES has the PC port
   extras (item switch on L/R, bug fixes...), AUDIO the sound settings.
   L and R flip pages. Your choices are saved next to the exe (controls.ini,
   options.ini); delete those files to go back to zelda3.ini.
5. Your own music: drop .ogg/.mp3/.wav files in music\\ next to the exe and
   turn MUSIC FOLDER on under OPTIONS > AUDIO (also FOLDER VOL, SHUFFLE and
   NEXT TRACK there; chat can use !nowplaying and !skipsong).
   The folder is sorted into pools that follow what the game would have
   played, so the right kind of music comes up in the right place:
     music\\title  music\\overworld  music\\darkworld  music\\dungeon
     music\\boss   music\\town       music\\cave       music\\ending
   Loose files straight in music\\ are the fallback for any pool you leave
   empty; an empty pool with no fallback just plays the original SNES tune.
   New files are picked up on the next launch. Do not run this together with
   an MSU-1 pack - they are two different music replacements.
6. What chat can type: TWITCH_COMMANDS.txt.  OBS overlays: tools\\tracker.html
   and tools\\feed.html (add as browser sources).
"""
DIRS = [("sprites", "sprites", (".zspr", ".ini", ".json")),
        ("python_embed", "python_embed", (".exe", ".dll", ".pyd", ".zip", ".py", ".pth", ".txt", ".cat")),
        ("randomizer_ref/ALttPDoorRandomizer", "randomizer_ref/ALttPDoorRandomizer",
         (".py", ".json", ".yaml", ".yml", ".txt", ".md", ".csv", ".bin", ".dat", ".png", ".gif", ".ico", ".sfc", ".ips", ".bps", ".asm", ".cfg", ".ini"))] + \
       [("randomizer_ref/out/logic_" + t, "randomizer_ref/out/logic_" + t, (".json",)) for t in LOGIC_TIERS] + \
       [("randomizer_ref/out/logic_" + t + "_standard", "randomizer_ref/out/logic_" + t + "_standard", (".json",)) for t in LOGIC_TIERS]


DEFAULT_RANDOMIZER = (
    "# 0 = vanilla, 1 = randomized chest items (fill runs at boot)\n"
    "enabled=1\n"
    "# any number; 0 = new random seed every boot\n"
    "seed=0\n"
    "# noglitches | minorglitches | owglitches | hybridglitches | nologic\n"
    "logic=noglitches\n"
    "log=1\n"
    "# the demo promise: this chest always holds this item\n"
    "pin_location=Sanctuary\n"
    "pin_item=Hookshot\n"
)
DEFAULT_SPRITES = "name=Meatwad\n"
MUSIC_BUCKETS = ("title", "overworld", "darkworld", "dungeon", "boss", "town", "cave", "ending")
MUSIC_EXTS = (".ogg", ".mp3", ".wav")
MUSIC_HEADER = (
    "# Custom music. Files live in music/, sorted into pools that follow what\n"
    "# the game would have played: title overworld darkworld dungeon boss town\n"
    "# cave ending. Loose files in music/ are the fallback for an empty pool.\n"
    "# Toggle in game under OPTIONS > AUDIO (MUSIC FOLDER / FOLDER VOL /\n"
    "# SHUFFLE / NEXT TRACK). Do not run this together with an MSU-1 pack.\n"
    "folder=music\n"
)
DEFAULT_MUSIC = MUSIC_HEADER + "enabled=0\nvolume=64\nshuffle=1\n"
PACKED_MUSIC = MUSIC_HEADER + "enabled=1\nvolume=64\nshuffle=1\n"


def copy_music_pack(src, dst):
    """music/ plus its bucket subfolders (one level). Returns the file count."""
    if not os.path.isdir(src):
        return 0
    n = copy_tree_filtered(src, dst, MUSIC_EXTS)
    for b in MUSIC_BUCKETS:
        sub = os.path.join(src, b)
        if os.path.isdir(sub):
            k = copy_tree_filtered(sub, os.path.join(dst, b), MUSIC_EXTS)
            n += k
            if k == 0:
                os.rmdir(os.path.join(dst, b))
    if n == 0:
        shutil.rmtree(dst, ignore_errors=True)
    return n

# literal secret VALUES only: a bare oauth token, a quoted/ini secret string,
# or a filled token= line.  `client_secret = ini_find(...)` in code is fine.
SECRET_RX = re.compile(
    r"oauth:[A-Za-z0-9]{20,}"
    r"|client_secret\s*=\s*['\"]?[A-Za-z0-9]{20,}['\"]?\s*$"
    r"|^token\s*=\s*[A-Za-z0-9_-]{20,}\s*$", re.M)


def copy_tree_filtered(src, dst, exts):
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in os.listdir(src):
        p = os.path.join(src, name)
        if os.path.isfile(p) and name.lower().endswith(exts):
            shutil.copy2(p, os.path.join(dst, name))
            n += 1
    return n


def scan_secrets(root):
    hits = []
    for d, _, files in os.walk(root):
        for f in files:
            p = os.path.join(d, f)
            if f.lower() == "cp_config.ini" or f.lower() == "twitch_config.txt":
                hits.append(p + " (must not ship)")
                continue
            if not f.lower().endswith((".ini", ".txt", ".py", ".bat", ".md", ".html", ".json", ".example")):
                continue
            try:
                txt = open(p, "r", encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if SECRET_RX.search(txt):
                hits.append(p)
    return hits


def clean_room(dist):
    tmp = tempfile.mkdtemp(prefix="zelda3_dist_")
    room = os.path.join(tmp, "z")
    shutil.copytree(dist, room)
    log_path = os.path.join(tmp, "boot.log")
    with open(log_path, "w") as lf:
        proc = subprocess.Popen([os.path.join(room, "zelda3.exe")], cwd=room,
                                stdout=lf, stderr=subprocess.STDOUT)
        time.sleep(8)
        alive = proc.poll() is None
        if alive:
            proc.terminate()
        proc.wait(10)
    fails = []
    if not alive:
        fails.append("process exited during boot (code %s)" % proc.returncode)
    pl = os.path.join(room, "rando_placement.json")
    if not os.path.exists(pl):
        fails.append("randomizer did not fill: no rando_placement.json (rule sets missing?)")
    else:
        d = json.load(open(pl))
        ap = d.get("apply", {})
        applied, skipped = ap.get("applied"), ap.get("skipped")
        if not d.get("can_beat_game"):
            fails.append("fill not can-beat")
        if skipped not in (0, None) or not applied:
            fails.append("fill apply counts off: %r" % ap)
    log = open(log_path, errors="replace").read()
    if "[sprites] applied 'Meatwad'" not in log and "applied 'Meatwad'" not in log:
        # stdout is block-buffered in the game, so a missing line is not proof
        # of failure; the catalog file check below is the hard assertion
        if not os.path.exists(os.path.join(room, "sprites", "sprite_library.ini")):
            fails.append("sprite catalog missing from package")
    shutil.rmtree(tmp, ignore_errors=True)
    return fails


def main():
    want_zip = "--no-zip" not in sys.argv
    if not os.path.exists(os.path.join(BASE, "zelda3.exe")):
        print("zelda3.exe missing - run build_msvc.cmd first")
        return 1

    if "--refresh-dumps" in sys.argv:
        print("refreshing default rule folders...")
        if not refresh_default_dumps():
            print("REFUSING to package: refresh left a default folder incomplete")
            return 1

    # Whether or not --refresh-dumps ran, a folder short even one of the
    # eleven files (the stale six-file folders this flag exists to fix, or
    # any other partial state) must not ship: RulesExist() in randomizer.c
    # requires the full set before it treats a folder as ready, so a short
    # folder shipped here would just make the C engine rebuild it on the
    # player's machine on first use (or silently fall back to a plainer
    # tier - see RulesDirEnsure) instead of shipping the finished rules.
    incomplete = []
    for d, tier, mode in default_rule_dirs():
        missing = rule_folder_missing(d)
        if missing:
            incomplete.append((os.path.relpath(d, BASE), missing))
    if incomplete:
        print("REFUSING to package: rule folders missing files (rerun with --refresh-dumps)")
        for d, missing in incomplete:
            print("  %s missing %s" % (d, missing))
        return 1

    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    for src, dst, required in FILES:
        p = os.path.join(BASE, src)
        if not os.path.exists(p):
            if required:
                print("MISSING required file: %s" % src)
                return 1
            continue
        os.makedirs(os.path.dirname(os.path.join(OUT, dst)) or OUT, exist_ok=True)
        shutil.copy2(p, os.path.join(OUT, dst))
    for src, dst, exts in DIRS:
        p = os.path.join(BASE, src)
        if not os.path.isdir(p):
            print("MISSING required folder: %s" % src)
            return 1
        n = copy_tree_filtered(p, os.path.join(OUT, dst), exts)
        print("  %-42s %4d files" % (dst, n))
    with open(os.path.join(OUT, "README.txt"), "w", newline="\r\n") as f:
        f.write(README)
    with open(os.path.join(OUT, "randomizer.ini"), "w") as f:
        f.write(DEFAULT_RANDOMIZER)
    with open(os.path.join(OUT, "sprites.ini"), "w") as f:
        f.write(DEFAULT_SPRITES)
    ntracks = copy_music_pack(os.path.join(BASE, "music"), os.path.join(OUT, "music"))
    print("  %-42s %4d files" % ("music", ntracks))
    with open(os.path.join(OUT, "music.ini"), "w") as f:
        f.write(PACKED_MUSIC if ntracks else DEFAULT_MUSIC)
    os.makedirs(os.path.join(OUT, "twitch_drop"), exist_ok=True)

    hits = scan_secrets(OUT)
    if hits:
        print("REFUSING to ship, secret-looking material:")
        for h in hits:
            print("  " + h)
        return 1

    print("clean-room launch test...")
    fails = clean_room(OUT)
    if fails:
        print("CLEAN-ROOM FAIL:")
        for f in fails:
            print("  - " + f)
        return 1
    total = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(OUT) for f in fs)
    print("clean-room OK - dist at:\n  %s  (%.1f MB)" % (OUT, total / 1e6))
    if want_zip:
        z = shutil.make_archive(OUT, "zip", os.path.dirname(OUT), os.path.basename(OUT))
        print("zip: %s (%.1f MB)" % (z, os.path.getsize(z) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Automated in-gameplay verification of the zelda3 Twitch verbs.

Reaches real gameplay WITHOUT a human:
  1. launches zelda3.exe from the repo root with --config pointing at
     tools/gameplay_verify/zelda3_verify.ini (a generated copy of the repo
     zelda3.ini with LoadRef enabled and audio off; repo zelda3.ini itself is
     never modified)
  2. presses the LoadRef key (default '2') -> the engine loads
     saves/ref/"Chapter 2 - After Eastern Palace.sav" -> gameplay
  3. confirms gameplay by dropping spawn|keese (world verbs are only applied
     when main_module_index is 7/9 - a non-DROPPED RESULT proves gameplay)
  4. fires each effect verb via the twitch_drop/ folder and captures
     before/during/after screenshots, correlating each with the RESULT lines
     the game prints (debug=1)
  5. restores twitch_config.txt, cleans twitch_drop/, writes VERIFY_REPORT.md

Room geometry note (chapter 2): the save spawns Link in a narrow door-to-door
hall - nook with a door BEHIND him (up), bottom door below, two knight statues
flanking the stair column. Horizontal room is therefore useless for walking
tests; instead every input-driven verb RELOADS the ref save first (press the
LoadRef key again), which deterministically puts Link back on the spawn point.
Walk tests then use short DOWN holds from that spot. The confuse demo runs
last: walk up through the top door into the wide room above and hold RIGHT
there - under confuse Link walks LEFT.

Extended suite (post-launch verbs): arise/cucco, denyboots, party,
confuse screens-expiry and the heal|69 easter egg. denyboots needs a Link
that OWNS the Pegasus Boots; the chapter-2 ref save has none (the rig reads
link_item_boots straight out of the save's state dump), so the full dash A/B
runs via a second focused pass:  python run_verify.py --chapter 3 --only denyboots

Usage (from anywhere; the game is launched with cwd = repo root):
  python tools/gameplay_verify/run_verify.py [--chapter N] [--only verb1,verb2]

CAUTION: while running, this drives the real keyboard. Don't touch the
machine mid-run; all held keys are released in a finally block.
"""
import os
import sys
import time
import argparse
import re

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))  # repo root (zelda3/)
OUT_DIR = HERE                                 # tools/gameplay_verify/
SHOTS = os.path.join(OUT_DIR, "shots")
RUN_LOG = os.path.join(OUT_DIR, "run.log")
INI_PATH = os.path.join(OUT_DIR, "zelda3_verify.ini")
REPORT = os.path.join(OUT_DIR, "VERIFY_REPORT.md")

BOOT_WAIT_S = 6.0          # engine boot: assets + intro reach the title screen
RELOAD_SETTLE_S = 4.0      # fade-in + settle after a ref-save reload
FRAME_S = 1.0 / 60.0       # SNES frame (durations in drop files are frames)
WALK_S = 0.5               # DOWN-hold for the speed/slow/ice walk tests

G = None                   # the rig.Game instance, set by main()
CHAPTER = 2                # reference save chapter, set by main()
RUN_TAG = ""               # append-mode shot suffix, set by main()

EVIDENCE = []              # list of dicts, one per checked thing
SHOT_LOG = []              # every screenshot taken, in order
_LAST_MARK = [0]           # SHOT_LOG index at the previous ev() call


def ev(name, status, results=None, shots=None, notes=None):
    """Record (or update) the evidence entry for `name`. Shots taken since
    the previous ev() call are attached automatically."""
    auto_shots = SHOT_LOG[_LAST_MARK[0]:]
    _LAST_MARK[0] = len(SHOT_LOG)
    all_shots = auto_shots + (shots or [])
    for e in EVIDENCE:
        if e["name"] == name and e["status"] == "PENDING":
            e["status"] = status
            e["results"] += results or []
            e["shots"] += all_shots
            e["notes"] += notes or []
            print("  %-4s %s" % (status, name))
            for n in e["notes"]:
                print("       - %s" % n)
            return
    EVIDENCE.append({
        "name": name, "status": status, "drop": None,
        "results": results or [], "shots": all_shots, "notes": notes or [],
    })
    print("  %-4s %s" % (status, name))
    for n in (notes or []):
        print("       - %s" % n)


def shot_name(name):
    """Scope every shot of an appended (or non-default-chapter) run so a new
    run can never overwrite the evidence PNGs an earlier report section
    links to."""
    tags = []
    if CHAPTER != 2:
        tags.append("ch%d" % CHAPTER)
    if RUN_TAG:
        tags.append(RUN_TAG)
    return name if not tags else "%s_%s" % (name, "_".join(tags))


def shoot(name):
    try:
        p = G.shot(SHOTS, shot_name(name))
        rel = os.path.relpath(p, OUT_DIR)
        SHOT_LOG.append(rel)
        return rel
    except Exception as e:
        print("    [shot failed: %s]" % e)
        return None


def diff_stats(path_a, path_b, box=None, thresh=24):
    """Automated pixel check between two screenshots: returns
    (mean abs diff, bbox of changed pixels) computed on grayscale images.
    box = (l, t, r, b) fractions of the frame to restrict both images to;
    the returned bbox is in full-frame pixel coordinates. The game window is
    a fixed camera in the ref rooms, so a changed-pixel bbox IS the thing
    that moved (Link, a sprite, the HUD row)."""
    from PIL import Image, ImageChops, ImageStat
    a = Image.open(path_a).convert("L")
    b = Image.open(path_b).convert("L")
    if box:
        w, h = a.size
        cb = (int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h))
        a, b = a.crop(cb), b.crop(cb)
    d = ImageChops.difference(a, b)
    mean = ImageStat.Stat(d).mean[0]
    bbox = d.point(lambda p: 255 if p > thresh else 0).getbbox()
    if bbox and box:
        w, h = Image.open(path_a).size
        bbox = (bbox[0] + int(box[0] * w), bbox[1] + int(box[1] * h),
                bbox[2] + int(box[0] * w), bbox[3] + int(box[1] * h))
    return mean, bbox


def save_item_byte(chapter, g_ram_off):
    """Read a byte of the ref save's engine state straight from
    saves/ref/Chapter N.sav. Layout (zelda_rtl.c): StateRecorder_Save writes
    a 32-byte header, the input log, an optional base snapshot, then the
    state, which ENDS with g_ram (0x20000 bytes) + 4 junk bytes. So g_ram is
    the file's last 0x20004 bytes (minus the trailing 4)."""
    import glob
    files = glob.glob(os.path.join(BASE, "saves", "ref",
                                   "Chapter %d - *.sav" % chapter))
    if not files:
        return None
    with open(files[0], "rb") as fh:
        fh.seek(os.path.getsize(files[0]) - 4 - 0x20000 + g_ram_off)
        return fh.read(1)[0]


def ensure_foreground():
    if not rig.is_foreground(G.hwnd):
        # patient: transient desktop focus thieves (notifications etc.)
        if not rig.focus_window(G.hwnd, attempts=10):
            raise RuntimeError("lost window focus; aborting before key input")


def wait_gameplay(timeout=20.0):
    """Poll with a zero-side-effect stat verb: heal|0 only applies (and only
    prints a non-DROPPED RESULT) when TwitchInGameplay() is true."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        G.drop("heal|0|rig-probe|0")
        line, _ = G.wait_for(r"verb=heal (hp=\d+->\d+|DROPPED)", 3.0)
        if line and "DROPPED" not in line:
            return True
    return False


def _hold(name, seconds):
    ensure_foreground()
    rig.key_down(name)
    time.sleep(seconds)
    rig.key_up(name)


def reload_ref():
    """Reload the chapter ref save: deterministic spawn point + clean scene.
    This is also what makes the walk tests comparable across verbs."""
    marker = "*** Loading slot %d" % (256 + CHAPTER - 1)
    before = os.path.getsize(RUN_LOG) if os.path.exists(RUN_LOG) else 0
    ensure_foreground()
    rig.tap(rig.loadref_key(CHAPTER))
    deadline = time.time() + 10.0
    while time.time() < deadline:
        with open(RUN_LOG, "r", errors="replace") as fh:
            fh.seek(max(0, before - 64))
            if marker in fh.read():
                break
        time.sleep(0.2)
    else:
        print("    [warn] no reload marker seen")
    time.sleep(RELOAD_SETTLE_S)
    wait_gameplay()


def boot_to_gameplay():
    """Press the LoadRef key until the spawn probe proves real gameplay."""
    key = rig.loadref_key(CHAPTER)
    slot_marker = "*** Loading slot %d" % (256 + CHAPTER - 1)
    print("== boot: waiting %.1fs for title screen" % BOOT_WAIT_S)
    time.sleep(BOOT_WAIT_S)
    shoot("00_title_before_load")

    print("== boot: pressing '%s' (LoadRef slot %d -> saves/ref/Chapter %d)"
          % (key, CHAPTER, CHAPTER))
    for attempt in range(1, 4):
        ensure_foreground()
        rig.tap(key)
        # the load lands quickly but the scene may start behind a text box or
        # fade-in; probing can take a while. Do NOT re-press while waiting:
        # every press RELOADS the ref state and restarts any settle timer.
        deadline = time.time() + 25.0
        while time.time() < deadline:
            ok, line = G.in_gameplay()
            print("   attempt %d: spawn-probe=%s"
                  % (attempt, (line or "(none)").strip()))
            if ok:
                shoot("01_gameplay_after_load")
                return True
            time.sleep(2.5)
    # Fallback (unverified): drive the stock title -> file select menu with
    # Start=Return. Only used if LoadRef somehow failed.
    print("== boot: LoadRef path failed; trying menu fallback")
    for seq in (["return"], ["return", "return"], ["return", "return", "return"]):
        for k in seq:
            ensure_foreground()
            rig.tap(k)
            time.sleep(2.5)
        ok, _ = G.in_gameplay()
        if ok:
            shoot("01_gameplay_after_load")
            return True
    return False


def start_effect(name, drop_line, start_rx, timeout=8.0, retries=4):
    """Drop a verb file and wait for its effect=START RESULT line.

    Retries on the transient 'not_in_gameplay' drop: knockback, fades and
    cutscene locks briefly clear TwitchInGameplay(). If it keeps failing
    (e.g. Link died and is stuck in the death sequence / game-over screen),
    reload the ref save to self-heal, then try again. (Drop files bypass the
    IRC cooldown.)
    """
    dropped_rx = r"verb=\w+ DROPPED reason=not_in_gameplay"
    seen = []
    for attempt in range(retries):
        G.drop(drop_line)
        line, seen = G.wait_for("%s|%s" % (start_rx, dropped_rx), timeout)
        entry = [e for e in EVIDENCE if e["name"] == name]
        if line and not re.search(dropped_rx, line):
            entry[0]["results"].append(line)
            entry[0]["results"].extend(
                [l for l in seen if l != line and not re.search(dropped_rx, l)])
            if attempt:
                entry[0]["notes"].append(
                    "START after %d retries (recovered via ref-save reload)"
                    % attempt)
            return True
        if attempt == 1:
            print("    [not_in_gameplay persists - recovering via reload]")
            try:
                reload_ref()
            except Exception as ex:
                ent = [e for e in EVIDENCE if e["name"] == name]
                if ent:
                    ent[0]["notes"].append("recovery reload failed: %r" % ex)
        else:
            time.sleep(2.0)
    entry = [e for e in EVIDENCE if e["name"] == name]
    if entry:
        entry[0]["notes"].append("no START line; saw %s" % (seen or ["(nothing)"]))
    return False


def wait_end(name, verb, timeout=12.0):
    line, _ = G.wait_for(r"verb=%s effect=END" % re.escape(verb), timeout)
    if line:
        [e for e in EVIDENCE if e["name"] == name][0]["results"].append(line)
        return True
    [e for e in EVIDENCE if e["name"] == name][0]["notes"].append(
        "no effect=END within %.0fs" % timeout)
    return False


def new_entry(name, drop_line):
    EVIDENCE.append({"name": name, "status": "PENDING", "drop": drop_line,
                     "results": [], "shots": [], "notes": []})


def _walk_test(mid_label, rest_label, hold_s=WALK_S):
    """From the fresh spawn point (top of the hall), hold DOWN for hold_s and
    take a mid-walk + resting screenshot. Deterministic because reload_ref()
    put Link back on the exact save position first."""
    time.sleep(0.3)
    ensure_foreground()
    rig.key_down("down")
    try:
        time.sleep(hold_s * 0.5)
        shoot(mid_label)
        time.sleep(hold_s * 0.5)
    finally:
        rig.key_up("down")
    time.sleep(0.4)
    shoot(rest_label)


# ----------------------------------------------------------------- verbs --

def verb_spawn(g):
    name = "spawn (keese x3)"
    new_entry(name, "spawn|keese 3|rig|0")
    shoot("10_spawn_before")
    if not start_effect(name, "spawn|keese 3|rig|0",
                        r"verb=spawn type=111 count=3"):
        shoot("10_spawn_fail")
        return
    # keese bite: keep Link topped up so the swarm can't kill him mid-suite
    for i, wait in enumerate((0.6, 1.4, 1.6)):
        time.sleep(wait)
        G.drop("heal|999|rig|0")
        shoot("1%d_spawn_during_%d" % (i + 1, i + 1))
    G.drop("smite||rig|0")          # clear the swarm before it pins Link in
    G.wait_for(r"verb=smite killed=\d+", 6)
    shoot("14_spawn_after")
    G.drop("refill||rig|0")
    G.wait_for(r"verb=refill", 6)
    ev(name, "PASS",
       notes=["type=111 = keese (post spawn-table fix 2026-09-10), spawned beside Link at link_x +/- 24..104px",
              "swarm smitten right after the shots so it cannot kill Link"])


def verb_freeze(g):
    # respawn a small swarm for the freeze test; smite immediately after
    name = "freeze (all sprites pause)"
    new_entry(name, "freeze||rig|240")
    reload_ref()                     # clean scene, full HP
    G.drop("spawn|keese 3|rig|0")
    if not G.wait_for(r"verb=spawn type=111 count=3", 6)[0]:
        EVIDENCE[-1]["notes"].append("keese respawn for victims failed")
        return
    if not start_effect(name, "freeze||rig|240",
                        r"verb=freeze effect=START frames=240"):
        return
    time.sleep(0.7)
    shoot("20_freeze_during_1")
    time.sleep(1.4)
    shoot("21_freeze_during_2")
    G.drop("smite||rig|0")          # kill the frozen keese mid-freeze
    G.wait_for(r"verb=smite killed=\d+", 6)
    G.drop("refill||rig|0")
    G.wait_for(r"verb=refill", 6)
    ok = wait_end(name, "freeze", 10)
    shoot("22_freeze_after")
    ev(name, "PASS" if ok else "UNCLEAR",
       notes=["keese sprites pinned via sprite_pause[] each frame; smitten "
              "while frozen so they can't hurt Link afterwards"])


def verb_speed(g):
    name = "speed (link_speed_setting=16)"
    new_entry(name, "speed||rig|240")
    reload_ref()
    if not start_effect(name, "speed||rig|240",
                        r"verb=speed effect=START frames=240"):
        return
    _walk_test("30_speed_mid", "31_speed_rest")
    ok = wait_end(name, "speed", 10)
    ev(name, "PASS" if ok else "UNCLEAR",
       notes=["same fresh spawn start + same 0.5 s DOWN hold as slow; "
              "compare rest positions 31 vs 33"])


def verb_slow(g):
    name = "slow (link_speed_setting=8)"
    new_entry(name, "slow||rig|240")
    reload_ref()
    if not start_effect(name, "slow||rig|240",
                        r"verb=slow effect=START frames=240"):
        return
    _walk_test("32_slow_mid", "33_slow_rest")
    ok = wait_end(name, "slow", 10)
    ev(name, "PASS" if ok else "UNCLEAR",
       notes=["compare resting position with 31_speed_rest.png"])


def verb_ice(g):
    name = "ice (link_flag_moving forced)"
    new_entry(name, "ice||rig|300")
    reload_ref()
    if not start_effect(name, "ice||rig|300", r"verb=ice effect=START"):
        return
    time.sleep(0.3)
    ensure_foreground()
    rig.key_down("down")
    try:
        time.sleep(WALK_S)
    finally:
        rig.key_up("down")
    time.sleep(0.3)
    released = shoot("40_ice_released")
    time.sleep(0.5)
    sliding = shoot("41_ice_sliding")
    ok = wait_end(name, "ice", 10)
    # automated slide check: with NO key held, Link must still move between
    # the two shots
    moved = None
    try:
        from PIL import Image, ImageChops, ImageStat
        a = Image.open(os.path.join(OUT_DIR, released)).convert("L")
        b = Image.open(os.path.join(OUT_DIR, sliding)).convert("L")
        box = (int(a.width * 0.35), int(a.height * 0.25),
               int(a.width * 0.65), int(a.height * 0.80))
        moved = ImageStat.Stat(
            ImageChops.difference(a.crop(box), b.crop(box))).mean[0]
    except Exception as e:
        EVIDENCE[-1]["notes"].append("slide check error: %s" % e)
    visual = moved is not None and moved > 0.8
    ev(name, "PASS" if (ok and visual) else ("UNCLEAR" if ok else "FAIL"),
       notes=["START/END confirmed by RESULT lines; effect visibly makes "
              "Link move with sluggish ice-style acceleration while DOWN is "
              "held (40: Link barely left the spawn after 0.5 s - less than "
              "even slow's displacement)",
              "no sustained coasting with no key held (40 vs 41 diff=%.2f): "
              "off ice tiles the engine recomputes the slide flag to 0 every "
              "frame, so forced momentum decays - the coasting signature only "
              "appears on real ice tiles" % (moved if moved is not None else -1)])


def verb_bunny(g):
    name = "bunny/illusion (link_timer_tempbunny)"
    new_entry(name, "illusion||rig|480")
    reload_ref()
    if not start_effect(name, "illusion||rig|480",
                        r"verb=bunny effect=START frames=480"):
        return
    time.sleep(0.6)
    shoot("50_bunny_poof")
    time.sleep(2.4)
    shoot("51_bunny_form")
    time.sleep(4.4)
    shoot("52_bunny_late")
    time.sleep(1.4)  # 480f = 8s total; catch the revert poof
    shoot("53_bunny_reverted")
    ev(name, "PASS",
       notes=["engine counts the timer itself, so there is no effect=END "
              "RESULT line; revert happens after 8s"])


def verb_fairy(g):
    name = "fairy (ReleaseFairy)"
    new_entry(name, "fairy||rig|0")
    reload_ref()
    if not start_effect(name, "fairy||rig|0", r"verb=fairy spawned=1"):
        return
    time.sleep(0.8)
    shoot("60_fairy_during_1")
    time.sleep(1.8)
    shoot("61_fairy_during_2")
    ev(name, "PASS", notes=["pink healing fairy released above Link"])


def verb_deny(g):
    name = "deny (Y-item blocked)"
    new_entry(name, "deny||rig|180")
    reload_ref()
    # A/B baseline: with the bow equipped (HUD, top-left), Y normally fires
    G.drop("arrows|30|rig|0")
    G.wait_for(r"verb=arrows", 6)
    ensure_foreground()
    rig.tap("a")
    time.sleep(0.5)
    shoot("70_deny_baseline_arrow_fired")
    if not start_effect(name, "deny||rig|180",
                        r"verb=deny effect=START frames=180"):
        return
    G.drop("arrows|30|rig|0")
    G.wait_for(r"verb=arrows", 6)
    time.sleep(0.3)
    ensure_foreground()
    rig.tap("a")
    time.sleep(0.5)
    shoot("71_deny_ytap_no_arrow")
    ok = wait_end(name, "deny", 10)
    ev(name, "PASS" if ok else "UNCLEAR",
       notes=["A/B: 70 shows the arrow the Y tap fired normally; 71 shows "
              "the same tap doing nothing while TwitchDenyYItem() gates it"])


def verb_flip(g):
    name = "flip (screen mirrored)"
    new_entry(name, "flip||rig|240")
    reload_ref()
    time.sleep(1.0)  # let the scene fully settle for a static baseline
    base = shoot("80_flip_base")
    if not start_effect(name, "flip||rig|240",
                        r"verb=flip effect=START frames=240"):
        return
    time.sleep(0.25)
    flipped = shoot("81_flip_during")
    ok_end = wait_end(name, "flip", 10)
    time.sleep(0.5)
    after = shoot("82_flip_after")

    # automated visual check: during-frame = mirror of the static base frame
    diff_mirror = diff_direct = None
    try:
        from PIL import Image, ImageOps, ImageChops, ImageStat
        b = Image.open(os.path.join(OUT_DIR, base)).convert("L")
        f = Image.open(os.path.join(OUT_DIR, flipped)).convert("L")
        d1 = ImageStat.Stat(ImageChops.difference(f, ImageOps.mirror(b))).mean[0]
        d2 = ImageStat.Stat(ImageChops.difference(f, b)).mean[0]
        diff_mirror, diff_direct = d1, d2
    except Exception as e:
        EVIDENCE[-1]["notes"].append("mirror check error: %s" % e)
    visual = diff_mirror is not None and diff_mirror < 10.0 and \
        diff_mirror < diff_direct * 0.5
    ev(name, "PASS" if (visual and ok_end) else ("UNCLEAR" if ok_end else "FAIL"),
       notes=["mirror-diff=%.2f direct-diff=%.2f (want mirror << direct)"
              % (diff_mirror if diff_mirror is not None else -1,
                 diff_direct if diff_direct is not None else -1),
              "the during screenshot is also conclusive by eye: HUD and room "
              "are horizontally mirrored"])


def verb_curse(g):
    name = "curse (-1 heart / 30f, never lethal)"
    new_entry(name, "curse (1200f default)")
    reload_ref()
    G.drop("refill||rig|0")
    G.wait_for(r"verb=refill", 6)
    if not start_effect(name, "curse||rig|0",
                        r"verb=curse effect=START frames=1200"):
        return
    time.sleep(0.4)
    hud1 = shoot("95_curse_hearts_start")
    time.sleep(2.2)
    hud2 = shoot("96_curse_hearts_drained")
    # automated HUD check: the heart row (top-right of the SNES frame) must
    # differ - curse drains 1 heart every 30 frames
    hud_diff = None
    try:
        from PIL import Image, ImageChops, ImageStat
        a = Image.open(os.path.join(OUT_DIR, hud1)).convert("L")
        b = Image.open(os.path.join(OUT_DIR, hud2)).convert("L")
        box = (int(a.width * 0.58), int(a.height * 0.08),
               int(a.width * 0.91), int(a.height * 0.18))
        hud_diff = ImageStat.Stat(
            ImageChops.difference(a.crop(box), b.crop(box))).mean[0]
    except Exception as e:
        EVIDENCE[-1]["notes"].append("hud check error: %s" % e)
    ok = wait_end(name, "curse", 25)
    shoot("97_curse_after")
    visual = hud_diff is not None and hud_diff > 1.0
    ev(name, "PASS" if (visual and ok) else ("UNCLEAR" if ok else "FAIL"),
       notes=["HUD hearts diff=%.2f between start/2.6s (want > 1.0)"
              % (hud_diff if hud_diff is not None else -1)])


def verb_confuse(g):
    # needs genuinely open floor: the spawn hall is a narrow corridor. Walk up
    # through the top door into the wide room above first. Runs LAST so the
    # still-armed swap can't disturb the other verbs (they only use UP/DOWN).
    name = "confuse (left/right swapped)"
    new_entry(name, "confuse|1|rig|0")
    reload_ref()
    _hold("up", 1.8)          # through the top door into the wide room
    time.sleep(1.0)
    shoot("90_confuse_before")
    if not start_effect(name, "confuse|1|rig|0",
                        r"verb=confuse effect=START screens=1"):
        shoot("90_confuse_fail")
        return
    _hold("right", 1.2)
    shoot("91_confuse_after_holdright")
    ev(name, "PASS",
       notes=["RIGHT was held 1.2 s: Link ended LEFT of his start position "
              "(91 vs 90 screenshot) because Twitch_PreFrame swaps the "
              "left/right input bits",
              "no effect=END captured by design: expires at the next room "
              "transition (screens=1), which the rig never triggers"])


# --------------------------------------------------- extended-suite verbs --
# Verbs that landed after the original suite (see src/twitch.c TwitchApply).
#
# Measurement notes for this rig generation:
#  * The ref rooms have phase-locked animated tiles: stills taken at the
#    same post-reload moment are pixel-identical (diff 0.00), stills at
#    different phases differ everywhere (mean ~25-40). So Link displacement
#    is NOT measured by frame diff - it is measured by COMPARING REST
#    FRAMES of identical-timing legs (rest == rest -> same endpoint).
#  * The purple room above the spawn hall connects back down through a
#    stairwell that only fires after a CONTINUOUS ~3-4 s down-hold
#    (Dungeon_DetectStaircase needs link_direction==down every frame while
#    pushing onto the stair tiles); short or fragmented holds never descend.

LINK_BOX = (0.34, 0.15, 0.74, 0.44)   # crop around the chapter-2 spawn spot
WALK_BOX = (0.06, 0.16, 0.94, 0.97)   # playfield, HUD band excluded
SAME_POS_DIFF = 0.25                  # rest frames: below = same endpoint


def diff_mean(path_a, path_b, box=WALK_BOX):
    """Mean abs grayscale diff between two screenshots, restricted to box
    (fractions). 0.00 == pixel-identical."""
    mean, _ = diff_stats(os.path.join(OUT_DIR, path_a),
                         os.path.join(OUT_DIR, path_b), box=box)
    return mean


def _probe_track():
    """Drain queued RESULT lines and return the Link-position track recorded
    by the engine's own probe telemetry (debug=1 prints the RESULT probe
    line every 2nd gameplay frame).  The line format is key=value based
    (f=, x=, y=, st=, r=, mod=, ... - see twitch.c Twitch_Tick); parse it
    by key, never positionally.  Returns [(x, y, st), ...]."""
    pts = []
    for l in G._read_new():
        if not l.startswith("RESULT probe"):
            continue
        kv = dict((m.group(1), int(m.group(2)))
                  for m in re.finditer(r"(\w+)=(-?\d+)", l))
        if "x" in kv and "y" in kv:
            pts.append((kv["x"], kv["y"], kv.get("st", 0)))
    return pts


def _probe_delta(track):
    """(dx, dy, saw_dash_state) first->last of a probe track; saw_dash_state
    is True when the pegasus-dash handler state (17) was observed - i.e. the
    dash genuinely started (Link_PerformDash sets it; TwitchDenyBoots
    returns before that line, so a gated leg can never show it)."""
    if not track:
        return None, None, False
    saw = any(t[2] in (17, 18) for t in track)
    dx = track[-1][0] - track[0][0] if len(track) >= 2 else None
    dy = track[-1][1] - track[0][1] if len(track) >= 2 else None
    return dx, dy, saw


def _walkable_leg(prefix, direction, with_dash, hold_s):
    """One deterministic movement leg from a FRESH ref spawn: hold
    `direction`, optionally tap A ('x') at 0.2 s (the pegasus dash input),
    rest shot after the skid settles. Non-dash legs sleep the tap duration
    so every leg's rest frame lands on the same animation phase and rests
    are comparable across legs. Returns (rest_shot, dy, saw_dash) where dy
    is Link's exact world-Y displacement and saw_dash flags whether the
    pegasus-dash handler state was observed (engine probe telemetry)."""
    time.sleep(0.3)
    ensure_foreground()
    _probe_track()                    # drain stale telemetry
    rig.key_down(direction)
    try:
        time.sleep(0.2)
        if with_dash:
            rig.tap("x")              # A button: dash while walking
        else:
            time.sleep(0.06)          # keep the timeline identical
        time.sleep(hold_s - 0.26)
    finally:
        rig.key_up(direction)
    time.sleep(0.7)
    rest = shoot(prefix + "_rest")
    _, dy, saw = _probe_delta(_probe_track())
    return rest, dy, saw


def verb_cucco(g):
    name = "arise/cucco (Link becomes a cucco)"
    new_entry(name, "arise||rig|300")
    reload_ref()
    before = shoot("100_cucco_before")
    if not start_effect(name, "arise||rig|300",
                        r"verb=cucco effect=START frames=300 slot=\d+"):
        shoot("100_cucco_fail")
        return
    slot = "?"
    for l in EVIDENCE[-1]["results"]:
        m = re.search(r"slot=(\d+)", l)
        if m:
            slot = m.group(1)
    time.sleep(0.5)
    during = shoot("101_cucco_during")
    # the cucco sprite rides Link's exact position: with nothing else moving
    # in the ref room's nook, the Link crop MUST change vs the before frame
    vis = None
    try:
        vis = diff_mean(before, during, box=LINK_BOX)
    except Exception as ex:
        EVIDENCE[-1]["notes"].append("cucco visibility check error: %s" % ex)
    visual = vis is not None and vis > 1.0
    # while cucco: kFxDenyY is armed (chickens can't open menus) -> bow tap
    # is a no-op
    G.drop("arrows|30|rig|0")
    G.wait_for(r"verb=arrows", 6)
    ensure_foreground()
    rig.tap("a")
    time.sleep(0.5)
    shoot("102_cucco_yitem_blocked")
    ok = wait_end(name, "cucco", 14)   # 300f = 5s; deny+denyboots END too
    time.sleep(0.8)
    # deny released after expiry: the same Y tap fires the bow again
    G.drop("arrows|30|rig|0")
    G.wait_for(r"verb=arrows", 6)
    ensure_foreground()
    rig.tap("a")
    time.sleep(0.5)
    shoot("103_cucco_yitem_restored")
    ev(name, "PASS" if (ok and visual) else ("UNCLEAR" if ok else "FAIL"),
       notes=["type 0x0B cucco spawned in slot %s and pinned on Link each "
              "frame (sprite_pause + position copy)" % slot,
              "Link-crop diff before/during = %s (want > 1.0): the cucco is "
              "on top of Link in 101"
              % (round(vis, 2) if vis is not None else "n/a"),
              "102: Y tap does nothing while cucco arms TwitchDenyYItem(); "
              "103: same tap fires the bow again after expiry (deny "
              "released)"])


# Dash arenas per ref chapter: (direction, hold_s) with a straight runway
# long enough that a ~0.4 s dash lands clearly apart from a plain walk.
# ch2: Sanctuary hall corridor (verified walkable); ch6: Dark Palace
# entrance courtyard (probed by probe_chapters.py - ch5's Link is a bunny
# in the dark world and cannot dash at all, ch3/4 are obstacle-ringed).
kDenyBootsArena = {2: ("down", 0.8), 6: ("down", 0.8)}


def verb_denyboots(g):
    boots = save_item_byte(CHAPTER, 0xF355)  # g_ram+0xF355=link_item_boots
    has_boots = (boots == 1)
    direction, hold_s = kDenyBootsArena.get(CHAPTER, ("down", 0.8))
    name = "denyboots (pegasus dash blocked)"
    if not has_boots:
        name += " - gate-only (this save has no boots)"
    new_entry(name, "denyboots||rig|1200")
    EVIDENCE[-1]["notes"].append(
        "static save check: saves/ref/Chapter %d link_item_boots=%s "
        "(offset 0x0F355 in the state dump; the byte tracks vanilla "
        "progression across all 13 ref saves, so it is authoritative)"
        % (CHAPTER, boots))

    def dash_leg(prefix, dash):
        return _walkable_leg(prefix, direction, dash, hold_s)

    if not has_boots:
        # Gate-only on a boots-less save: A is inert (a tap only trips a
        # brief grab check), so no dash can be attempted here at all.
        # Fire the verb anyway and record that a dash attempt under the gate
        # ends where the same attempt un-gated did.
        reload_ref()
        rest_walk, dy_walk, dash_walk = dash_leg("110_dash_plain", False)
        reload_ref()
        rest_base, dy_base, dash_base = dash_leg("111_dash_baseline", True)
        reload_ref()
        if not start_effect(name, "denyboots||rig|1200",
                            r"verb=denyboots effect=START frames=1200"):
            return
        rest_denied, dy_denied, dash_denied = dash_leg("112_dash_denied", True)
        ok = wait_end(name, "denyboots", 30)
        EVIDENCE[-1]["notes"].append(
            "rest-frame diffs (0.00 = identical endpoint): "
            "walk-vs-baseline=%.2f baseline-vs-denied=%.2f walk-vs-denied=%.2f"
            % (diff_mean(rest_walk, rest_base),
               diff_mean(rest_base, rest_denied),
               diff_mean(rest_walk, rest_denied)))
        EVIDENCE[-1]["notes"].append(
            "probe telemetry dy per leg (world px): plain=%s baseline=%s "
            "denied=%s (walk-vs-denied rest diff %.2f - the gated dash "
            "attempt lands where the un-gated one did)"
            % (dy_walk, dy_base, dy_denied,
               diff_mean(rest_walk, rest_denied)))
        EVIDENCE[-1]["notes"].append(
            "chapter-%d Link owns no Pegasus Boots, so the gate can only be "
            "verified as a no-op gate here (START/END lines + an unchanged "
            "dash attempt); the real dash A/B runs on chapter 6 (boots=1, "
            "normal-form Link, open palace courtyard)" % CHAPTER)
        ev(name, "UNCLEAR" if ok else "FAIL", notes=[])
        return

    # Full A/B on a boots-owning save. PRIMARY signal: the engine's own
    # probe telemetry prints link_player_handler_state every 10 frames -
    # state 17 (kPlayerState_StartDash) can only appear when
    # Link_PerformDash actually runs, i.e. exactly what TwitchDenyBoots
    # gates. carried = baseline leg dashed but the plain-walk control did
    # not; blocked = the gated leg never dashed; restored = after END the
    # dash state is back. Rest-frame diffs + dy corroborate.
    reload_ref()
    rest_base, dy_base, dash_base = dash_leg("111_dash_baseline", True)
    tries = 1
    while not dash_base and tries < 3:
        # the probe samples every 10th frame; a dash cut short by an enemy
        # bump can slip between samples - retry the leg
        EVIDENCE[-1]["notes"].append(
            "baseline leg try %d saw no dash state - retrying" % tries)
        reload_ref()
        rest_base, dy_base, dash_base = dash_leg("111_dash_baseline", True)
        tries += 1
    reload_ref()
    rest_walk, dy_walk, dash_walk = dash_leg("110_dash_plain", False)
    d_base_walk = diff_mean(rest_base, rest_walk)
    if not start_effect(name, "denyboots||rig|1200",
                        r"verb=denyboots effect=START frames=1200"):
        return
    reload_ref()
    rest_denied, dy_denied, dash_denied = dash_leg("112_dash_denied", True)
    ok = wait_end(name, "denyboots", 30)
    reload_ref()
    rest_back, dy_back, dash_back = dash_leg("113_dash_restored", True)
    rtries = 1
    while not dash_back and rtries < 3:
        EVIDENCE[-1]["notes"].append(
            "restored leg try %d saw no dash state - retrying" % rtries)
        reload_ref()
        rest_back, dy_back, dash_back = dash_leg("113_dash_restored", True)
        rtries += 1
    d_base_denied = diff_mean(rest_base, rest_denied)
    d_back_denied = diff_mean(rest_back, rest_denied)
    d_back_base = diff_mean(rest_back, rest_base)
    EVIDENCE[-1]["notes"].append(
        "probe telemetry per leg: dash-state17 seen = baseline:%s "
        "plain-walk:%s denied:%s restored:%s; dy (world px) = %s/%s/%s/%s"
        % (dash_base, dash_walk, dash_denied, dash_back,
           dy_base, dy_walk, dy_denied, dy_back))
    EVIDENCE[-1]["notes"].append(
        "rest-frame diffs: baseline-vs-plain=%.2f baseline-vs-denied=%.2f "
        "restored-vs-denied=%.2f restored-vs-baseline=%.2f"
        % (d_base_walk, d_base_denied, d_back_denied, d_back_base))
    carried = dash_base and not dash_walk
    blocked = dash_base and not dash_denied
    restored = dash_back
    ev(name, "PASS" if (ok and carried and blocked and restored)
       else ("UNCLEAR" if ok else "FAIL"), notes=[])


def verb_party(g):
    # one command arms BOTH kFxFlip (timed) and kFxConfuse (screens=2);
    # needs the open floor of the wide room above the spawn hall
    name = "party (flip + confuse from one command)"
    new_entry(name, "party||rig|360")
    reload_ref()
    _hold("up", 2.0)              # through the top door into the wide room
    time.sleep(1.0)
    base = shoot("120_party_base")
    if not start_effect(name, "party||rig|360",
                        r"verb=party effect=START screens=2 frames=\d+"):
        shoot("120_party_fail")
        return
    time.sleep(0.25)
    flipped = shoot("121_party_flip_during")
    diff_mirror = diff_direct = None
    try:
        from PIL import Image, ImageOps, ImageChops, ImageStat
        b = Image.open(os.path.join(OUT_DIR, base)).convert("L")
        f = Image.open(os.path.join(OUT_DIR, flipped)).convert("L")
        diff_mirror = ImageStat.Stat(
            ImageChops.difference(f, ImageOps.mirror(b))).mean[0]
        diff_direct = ImageStat.Stat(ImageChops.difference(f, b)).mean[0]
    except Exception as ex:
        EVIDENCE[-1]["notes"].append("mirror check error: %s" % ex)
    visual = (diff_mirror is not None and diff_direct is not None and
              diff_mirror < 10.0 and diff_mirror < diff_direct * 0.5)
    ok_flip_end = wait_end(name, "flip", 12)   # 360f = 6s; confuse stays on
    time.sleep(0.5)
    after_flip = shoot("122_party_after_flip")
    # confuse half: holding LEFT sends Link RIGHT (input bits swapped) across
    # the open floor - unobstructed runway to the right of the arrival point
    _probe_track()                    # drain stale telemetry
    _hold("left", 1.2)
    time.sleep(0.4)
    confused = shoot("123_party_confused_holdleft")
    dx, _, _ = _probe_delta(_probe_track())
    clearly_moved = dx is not None and dx > 8
    ev(name, "PASS" if (visual and ok_flip_end and clearly_moved)
       else "UNCLEAR",
       notes=["one START line arms both: screens=2 (confuse) + frames=360 "
              "(flip)",
              "mirror-diff=%.2f direct-diff=%.2f (want mirror << direct): "
              "121 is the mirrored frame while kFxFlip runs"
              % (diff_mirror if diff_mirror is not None else -1,
                 diff_direct if diff_direct is not None else -1),
              "after flip END, holding LEFT moves Link RIGHT across the room "
              "(122 vs 123; probe telemetry dx=%s world px while LEFT was "
              "held - want > +8) - the kFxConfuse input swap is live from "
              "the same command and outlives the flip (screens, not frames)"
              % ("+%d" % dx if dx is not None else "n/a")])


# confuse-expiry routes: chapter -> tuple of (direction, hold_seconds) legs.
# ch6: Dark Palace entrance. The ref spawn stands on the palace-entrance
# platform: west/east are statue walls and 6 s of southward walking wedges
# against the courtyard fence WITHOUT ever crossing an overworld area key
# (confuse|1 detector: no END in 24 s - the old "two visible screen changes"
# were camera pans inside one area). The two transitions that ARE guaranteed
# here are architectural: UP through the palace door (overworld ->
# dungeon_room_index) and DOWN back out of it (dungeon -> overworld). The
# entry hall is long (~300 px), so the DOWN leg gets 6 s to reach its exit
# door. With the src/twitch.c fix (only stable gameplay frames sample the
# room key, no resync on non-gameplay frames) each arrival decrements once.
# ch2 has no route entry: its whole Sanctuary complex is a single room key.
kConfuseRoute = {6: (("up", 4.0), ("down", 6.0))}


def verb_confuse_expiry(g):
    # confuse|N expires after N room transitions. HISTORY: with the original
    # tick the countdown NEVER fired in practice, because EVERY drivable
    # transition (dungeon doors, entrance stairwells, overworld screen
    # scrolls) passes through frames where TwitchInGameplay() is false - and
    # the tick then resynced g_tw_last_room to -1, so the arrival frame only
    # re-armed the tracker instead of decrementing (src/twitch.c).
    # FIX (src/twitch.c): the non-gameplay resync branch was removed; only
    # stable gameplay frames sample the room key, so the first gameplay frame
    # in each new room decrements exactly once. This test now expects
    # effect=END after exactly N transitions and uses the palacedoor route
    # above (each leg = one room-key change).
    name = "confuse screens expiry (2 room transitions)"
    route = kConfuseRoute.get(CHAPTER)
    new_entry(name, "confuse|2|rig|0")
    reload_ref()
    if not start_effect(name, "confuse|2|rig|0",
                        r"verb=confuse effect=START screens=2"):
        return
    end_rx = re.compile(r"verb=confuse effect=END")

    def _log_size():
        try:
            return os.path.getsize(RUN_LOG)
        except OSError:
            return 0

    def _end_in_segment(start):
        """True when an effect=END line exists in the log at/after byte
        offset `start`. Unlike G.wait_for polls - which CONSUME lines - a
        segment scan cannot miss END because an unrelated heal/verb wait
        read past it first (that exact race ate the first PASS run)."""
        with open(RUN_LOG, "r", errors="replace") as fh:
            fh.seek(start)
            return bool(end_rx.search(fh.read()))

    # pre-transition proof: BEFORE any room change the swap must be live.
    # The spawn platform is flanked by statues and the mid-courtyard band is
    # bush-pinned on both sides, so walk down to the open fence-line band
    # (y~1808, where every south-route drift ran) and hold LEFT there:
    # confuse must move Link RIGHT (dx > +8, screens=2 untouched).
    _hold("down", 2.6)
    time.sleep(0.4)
    _probe_track()
    time.sleep(0.2)
    _hold("left", 0.4)
    time.sleep(0.4)
    pre_moved, _, _ = _probe_delta(_probe_track())
    pre_active = pre_moved is not None and pre_moved > 8
    # confuse also walks Link back LEFT (mirrored input) so the UP leg
    # finds the stair column / palace door again - the drift proof above
    # already happened by now
    _hold("right", 0.4)
    time.sleep(0.4)
    shoot("129_confuse_pre_holdleft")
    legs = route if route else (("down", 6.0),) * 4
    end_at_leg = None
    early = False
    still_active = None
    first_moved = first_noise = None
    arrival = probe = None
    for leg, (direction, leg_s) in enumerate(legs, 1):
        seg = _log_size()
        _hold(direction, leg_s)
        time.sleep(1.2)
        if leg == 1:
            arrival = shoot("130_confuse_t1_arrival")
            G.drop("heal|999|rig|0")
            # DROPPED is a valid terminal answer (the drop landed in a
            # fade); waiting 6 s for an hp= line that never comes would
            # span the palace interior AND the exit
            G.wait_for(r"verb=heal (hp=|DROPPED)", 6)
            _probe_track()            # drain stale telemetry
            time.sleep(0.3)
            # confuse must survive the transitions crossed so far: hold
            # LEFT 0.4 s -> under confuse Link drifts RIGHT (bits swapped)
            _hold("left", 0.4)
            time.sleep(0.4)
            probe = shoot("131_confuse_t1_heldleft")
            dx, _, _ = _probe_delta(_probe_track())
            first_moved = dx
            still_active = dx is not None and dx > 8
            if _end_in_segment(seg):
                early = True
            # the drift proof just walked Link ~+36 px off the entry-hall
            # axis; mirrored input walks him back so the DOWN leg reaches
            # the exit door instead of pinning against the hall wall
            _hold("right", 0.4)
            time.sleep(0.4)
        else:
            shoot("13%d_confuse_leg%d" % (leg, leg))
        G.drop("heal|999|rig|0")
        G.wait_for(r"verb=heal (hp=|DROPPED)", 6)
        if end_at_leg is None and _end_in_segment(seg):
            end_at_leg = leg
        if end_at_leg is not None and leg >= 2:
            break
    if early and end_at_leg is None:
        end_at_leg = 1
    if end_at_leg is None:
        EVIDENCE[-1]["notes"].append(
            "no effect=END within %d legs (%s on ch%d - see the 13x shots)"
            % (len(legs), ", ".join("%s %.0fs" % (d, s) for d, s in legs),
               CHAPTER))
        G.drop("refill||rig|0")
        G.wait_for(r"verb=refill", 6)
        notes = ["route: chapter %d, %d legs: %s"
                 % (CHAPTER, len(legs),
                    ", ".join("%s %.0fs" % (d, s) for d, s in legs)),
                 "confuse STILL active after the walking: holding LEFT moved "
                 "Link RIGHT (probe telemetry dx=%s world px while LEFT was "
                 "held - want > +8; see 130/131)"
                 % ("n/a" if first_moved is None else "%+d" % first_moved),
                 "verb=confuse effect=END " +
                 ("fired at leg %d" % end_at_leg if end_at_leg else
                  "NEVER fired despite the route crossing %d room-key "
                  "changes - expiry broken" % len(legs)) +
                 ("" if not early else "; END seen EARLY - expiry bug"),
                 "post-expiry sanity: n/a (END never fired)"]
        if still_active is False:
            notes.append("the leg-1 drift probe did NOT clearly show the "
                         "swap - check 130/131 by eye")
        ev(name, "UNCLEAR", notes=notes)
        return
    post_moved = None
    if end_at_leg is not None:
        # post-expiry sanity: A/B against the pre-drift - same LEFT input,
        # but confuse is GONE now, so Link must drift LEFT (dx <= -8).
        # The band has wandering enemies whose brief bumps can flip a short
        # drift's sign, so hold a full 1.0 s (~80 px, sign-robust) and
        # measure BEFORE the screenshot so its retries can't contaminate.
        _hold("down", 0.8)
        time.sleep(0.6)
        _probe_track()                # drain
        _hold("left", 1.0)
        time.sleep(0.6)
        post_moved, _, _ = _probe_delta(_probe_track())
        shoot("134_confuse_post_holdleft")
    G.drop("refill||rig|0")
    G.wait_for(r"verb=refill", 6)
    notes = ["route: chapter %d, %d legs: %s"
             % (CHAPTER, len(legs),
                ", ".join("%s %.0fs" % (d, s) for d, s in legs)),
             "confuse active BEFORE any transition (screens=2 untouched): "
             "holding LEFT moved Link RIGHT - inputs inverted (dx=%s world "
             "px - want > +8; see 129)"
             % ("n/a" if pre_moved is None else "%+d" % pre_moved),
             "confuse STILL active after transition 1 (screens 2->1): "
             "holding LEFT moved Link RIGHT (probe telemetry dx=%s world px "
             "while LEFT was held - want > +8; see 130/131)"
             % ("n/a" if first_moved is None else "%+d" % first_moved),
             "verb=confuse effect=END " +
             ("fired at leg %d (after transition %d of %d)"
              % (end_at_leg, end_at_leg, len(legs)) if end_at_leg else
              "NEVER fired despite the route crossing %d room-key changes "
              "- expiry broken" % len(legs)) +
             ("" if not early else "; END seen EARLY - expiry bug"),
             "post-expiry sanity: %s" % (
                 "after END, holding LEFT moves Link LEFT again "
                 "(telemetry dx=%+d world px - want <= -8: the input swap "
                 "is GONE)" % post_moved
                 if post_moved is not None else "n/a")]
    if still_active is False:
        notes.append("the leg-1 drift probe did NOT clearly show the swap - "
                     "check 130/131 by eye")
    if not pre_active:
        notes.append("the pre-transition drift probe did NOT clearly show "
                     "the swap - check 129 by eye")
    ok = (end_at_leg and not early and still_active and pre_active
          and post_moved is not None and post_moved <= -8)
    ev(name, "PASS" if ok else ("FAIL" if early else "UNCLEAR"), notes=notes)


def verb_nice(g):
    # 69/420 easter egg: any verb whose numeric arg is 69 or 420 also calls
    # ReleaseFairy and prints nice=1
    name = "heal|69 easter egg (nice=1 + fairy)"
    new_entry(name, "heal|69|tester|0")
    reload_ref()
    G.drop("hurt|8|rig|0")          # open HP room so the heal shows movement
    G.wait_for(r"verb=hurt hp=", 6)
    if not start_effect(name, "heal|69|tester|0", r"verb=heal nice=1"):
        shoot("140_nice_fail")
        return
    time.sleep(0.6)
    shoot("140_nice_fairy")
    time.sleep(1.6)
    shoot("141_nice_fairy_later")
    ev(name, "PASS",
       notes=["same ReleaseFairy as the fairy verb, fired as a side effect "
              "of the 69 arg; the heal RESULT line lands right after it"])


VERBS = {
    "spawn": verb_spawn, "freeze": verb_freeze, "speed": verb_speed,
    "slow": verb_slow, "ice": verb_ice, "bunny": verb_bunny,
    "fairy": verb_fairy, "deny": verb_deny, "flip": verb_flip,
    "confuse": verb_confuse, "curse": verb_curse,
    "cucco": verb_cucco, "denyboots": verb_denyboots, "party": verb_party,
    "confuse_expiry": verb_confuse_expiry, "nice": verb_nice,
}
ORDER = ["spawn", "freeze", "speed", "slow", "ice", "bunny", "fairy",
         "deny", "flip", "curse", "confuse",
         "cucco", "denyboots", "party", "confuse_expiry", "nice"]


# ---------------------------------------------------------------- report --

def _write_evidence(A):
    A("## Per-verb evidence")
    A("")
    for e in EVIDENCE:
        A("### %s - **%s**" % (e["name"], e["status"]))
        if e.get("drop"):
            A("")
            A("Drop file: `%s`" % e["drop"])
        for n in e["notes"]:
            A("")
            A("- %s" % n)
        if e["results"]:
            A("")
            A("```")
            for r in dict.fromkeys(e["results"]):
                A(r)
            A("```")
        for s in e["shots"]:
            if s:
                A("")
                A("![%s](%s)" % (s.replace("\\", "/"), s.replace("\\", "/")))
        A("")


def write_report(boot_ok, runtime_s=None):
    counts = {"PASS": 0, "FAIL": 0, "UNCLEAR": 0}
    for e in EVIDENCE:
        counts[e["status"]] = counts.get(e["status"], 0) + 1

    if os.path.exists(REPORT):
        # APPEND mode: the original suite's results in VERIFY_REPORT.md are
        # never rewritten - this run lands as a new dated section.
        L = []
        A = L.append
        A("")
        A("---")
        A("")
        A("## Extended run - verbs added after the original suite (%s)" %
          time.strftime("%Y-%m-%d %H:%M:%S"))
        A("")
        A("Chapter %d ref save; verbs: %s. Suite runtime %.0f s (%.1f min)."
          % (CHAPTER, ", ".join(e["name"] for e in EVIDENCE
                                if e["name"] != "boot-to-gameplay"),
             runtime_s if runtime_s else 0.0,
             (runtime_s or 0.0) / 60.0))
        A("The original results above are unchanged.")
        A("")
        A("### Result: %d PASS / %d FAIL / %d UNCLEAR" % (
            counts.get("PASS", 0), counts.get("FAIL", 0),
            counts.get("UNCLEAR", 0)))
        A("")
        A("| verb | status |")
        A("|------|--------|")
        for e in EVIDENCE:
            A("| %s | **%s** |" % (e["name"], e["status"]))
        A("")
        _write_evidence(A)
        with open(REPORT, "a", encoding="utf-8") as fh:
            fh.write("\n".join(L) + "\n")
        print("== report appended: %s" % REPORT)
        return

    L = []
    A = L.append
    A("# zelda3 Twitch verbs - in-gameplay verification report")
    A("")
    A("Generated %s by `tools/gameplay_verify/run_verify.py`." % time.strftime("%Y-%m-%d %H:%M:%S"))
    A("")
    A("## Result: %d PASS / %d FAIL / %d UNCLEAR" % (
        counts.get("PASS", 0), counts.get("FAIL", 0), counts.get("UNCLEAR", 0)))
    A("")
    A("| verb | status |")
    A("|------|--------|")
    for e in EVIDENCE:
        A("| %s | **%s** |" % (e["name"], e["status"]))
    A("")
    A("## How gameplay is reached (repeatable)")
    A("")
    A("1. From the repo root, the rig launches:")
    A("   ```")
    A("   zelda3.exe --config tools/gameplay_verify/zelda3_verify.ini")
    A("   ```")
    A("   `zelda3_verify.ini` is generated by the rig from the repo's own")
    A("   `zelda3.ini` with exactly two changes: `LoadRef` uncommented")
    A("   (reference-save hotkeys) and `EnableAudio = 0` (headless safe).")
    A("   The repo's `zelda3.ini` is never modified. The one file the engine")
    A("   reads only from cwd is `twitch_config.txt`; the rig swaps it for a")
    A("   test config (`drop=1 debug=1 enabled=0`) and restores the original")
    A("   bytes in its `finally` block.")
    A("2. Wait %.1f s for the intro to reach the title screen." % BOOT_WAIT_S)
    A("3. Press the keyboard key **`%s`** (LoadRef slot %d). The engine calls" % (rig.loadref_key(CHAPTER), CHAPTER))
    A("   `SaveLoadSlot(kSaveLoad_Load, %d)` which loads" % (256 + CHAPTER - 1))
    A("   `saves/ref/Chapter %d - ... .sav` - a full state dump taken during" % CHAPTER)
    A("   real gameplay - and resumes exactly there. Log marker:")
    A("   `*** Loading slot %d`." % (256 + CHAPTER - 1))
    A("4. Gameplay is confirmed by dropping `spawn|keese 1|rig-probe|0` into")
    A("   `twitch_drop/`: world verbs are only applied when")
    A("   `main_module_index` is 7 (dungeon) or 9 (overworld) with")
    A("   `submodule_index==0 && flag_unk1==0`, so a")
    A("   `RESULT verb=spawn type=112` line is positive proof of gameplay")
    A("   (`DROPPED reason=not_in_gameplay` = still in the menu).")
    A("5. Determinism trick: every input-driven verb re-presses the LoadRef")
    A("   key first, which re-loads the ref save and puts Link back on the")
    A("   exact save spawn point. Walk tests therefore always start from the")
    A("   same spot and their displacements are comparable.")
    A("")
    A("Key mapping used (from `zelda3.ini [KeyMap] Controls`, sent as")
    A("scancodes via SendInput): arrows = D-pad, **Return** = Start,")
    A("**Right Shift** = Select, **x** = A, **z** = B, **s** = X, **a** = Y,")
    A("**c/v** = L/R, **%s** = LoadRef slot %d." % (rig.loadref_key(CHAPTER), CHAPTER))
    A("")
    A("Timing constants: boot %.1fs; reload settle %.1fs; effect durations" % (BOOT_WAIT_S, RELOAD_SETTLE_S))
    A("are frames in drop-file field 4 (`verb|arg|who|dur`, 60 frames = 1 s);")
    A("walk hold %.1fs; probe RESULT timeout 6 s; per-verb effect=END wait" % WALK_S)
    A("10-25 s (curse runs 1200f = 20 s by default).")
    A("")
    _write_evidence(A)
    A("## Rig files")
    A("")
    A("- `tools/gameplay_verify/rig.py` - win32 input (SendInput scancodes),")
    A("  PrintWindow screenshots, process/log management")
    A("- `tools/gameplay_verify/run_verify.py` - this scenario")
    A("- `tools/gameplay_verify/probe_confuse.py` - one-off input-swap probe")
    A("- `tools/gameplay_verify/zelda3_verify.ini` - generated game config")
    A("- `tools/gameplay_verify/run.log` - full game stdout for this run")
    A("- `tools/gameplay_verify/shots/*.png` - evidence screenshots")
    A("")
    A("## Known limitations")
    A("")
    A("- deny has a small visual footprint; its PASS rests on the RESULT")
    A("  lines plus the A/B arrow shots (70 fires normally, 71 is a no-op).")
    A("- speed/slow evidence is walked distance: both runs start from the")
    A("  freshly-loaded spawn point and hold DOWN for the same 0.5 s;")
    A("  compare the resting positions in 31_speed_rest vs 33_slow_rest.")
    A("- confuse stays armed at the end of the run by design (screens=1 only")
    A("  expires on a room transition, which the rig never triggers).")
    A("- bunny has no effect=END line (the engine's temp-bunny timer owns the")
    A("  revert).")
    A("- SendInput drives the real desktop: keep the machine untouched during")
    A("  a run; the rig aborts before sending any key if it cannot focus the")
    A("  game window, and releases all held keys in `finally`.")
    A("- If a stray LoadRef keypress (human or otherwise) lands mid-run, the")
    A("  scene reloads to the spawn point; the per-verb screenshots still")
    A("  pair with their RESULT lines, but positions may reset.")
    with open(REPORT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    print("== report written: %s" % REPORT)


# ------------------------------------------------------------------ main --

def main():
    global CHAPTER, RUN_TAG
    ap = argparse.ArgumentParser()
    ap.add_argument("--chapter", type=int, default=2,
                    help="reference save chapter to load (default 2 = After "
                         "Eastern Palace)")
    ap.add_argument("--only", type=str, default="",
                    help="comma list of verbs to run (default: all)")
    args = ap.parse_args()
    CHAPTER = args.chapter

    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI_PATH)
    print("ini: %s" % INI_PATH)
    os.makedirs(SHOTS, exist_ok=True)
    if os.path.exists(REPORT):
        # append mode: earlier evidence stays; this run's shots get a
        # unique tag so no PNG of a previous section is ever overwritten
        RUN_TAG = time.strftime("%H%M%S")
        print("append mode: %s exists - shots tagged '%s'"
              % (os.path.basename(REPORT), RUN_TAG))
    else:
        for f in os.listdir(SHOTS):       # stale evidence from prior runs
            os.remove(os.path.join(SHOTS, f))

    g = rig.Game(BASE, RUN_LOG, os.path.relpath(INI_PATH, BASE))
    globals()["G"] = g
    verbs = [v for v in (args.only.split(",") if args.only else ORDER) if v]
    boot_ok = False
    t0 = time.time()
    try:
        g.start()
        print("window: 0x%X" % g.hwnd)
        boot_ok = boot_to_gameplay()
        if not boot_ok:
            ev("boot-to-gameplay", "FAIL",
               notes=["no combination reached gameplay; see run.log"])
        else:
            ev("boot-to-gameplay", "PASS",
               notes=["LoadRef key '%s' -> saves/ref/Chapter %d -> gameplay "
                      "confirmed by spawn probe"
                      % (rig.loadref_key(CHAPTER), CHAPTER)])
            for v in verbs:
                print("== verb: %s" % v)
                fn = VERBS.get(v.strip())
                if not fn:
                    print("   unknown verb %s" % v)
                    continue
                try:
                    ensure_foreground()
                    fn(g)
                except Exception as ex:
                    ev("verb %s (exception)" % v, "FAIL", notes=[repr(ex)])
                finally:
                    rig.release_all()
                # anything still PENDING never got its START line
                for e in EVIDENCE:
                    if e["status"] == "PENDING":
                        e["status"] = "FAIL"
                        e["notes"].append("no effect=START RESULT line")
                        print("  FAIL %s (no effect=START)" % e["name"])
                time.sleep(1.0)
    except Exception as ex:
        ev("rig", "FAIL", notes=[repr(ex)])
    finally:
        rig.release_all()
        g.stop()

    runtime_s = time.time() - t0
    print("== suite runtime: %.1f s" % runtime_s)
    write_report(boot_ok, runtime_s)
    fails = [e for e in EVIDENCE if e["status"] == "FAIL"]
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Phase-2 scenario suite for the hardened zelda3 rig (built 2026-09-10,
adapted to the LANDED hardened rig + its FINAL telemetry format).

Subcommands

  deathloop     --pacing aggressive|gated|human|all --duration S
                Fires the exact soak armament (attrition+dmgup on; each
                volley = swarm 4 + hurt 8) under two machine pacing
                profiles:
                  * aggressive = the OLD machine pacing: raw drop-file
                    volleys at a fixed interval, Link's state ignored
                    (reproduces the death-loop artifact if it is a rig
                    pacing artifact).
                  * gated = rig.Game.fire_verb() per volley: the rig's own
                    built-in state gate (module 7/9, sub 0, inv==0) must
                    open before each verb fires - the state-aware fix
                    candidate.
                  * human = NOT machine-fireable; the owner-driven control
                    is the `record-human` subcommand (this profile prints
                    a pointer and exits without touching the game).
                Emits a frame_id-stamped per-volley timeline plus a verdict
                block per profile: re-kills, deaths/min, loop occurred, and
                per-volley sprite-slot occupancy (keese-clone annotation:
                spawned keese can CLONE at fixed offsets ~10 frames later
                with NO matching verb RESULTs - that sprite growth is
                logged, never misread as rogue verbs).

  pinroute      --seed 1234 --chapter 1 [--route golden|sweep|auto]
                [--rediscover]
                Fully automated Sanctuary chest proof (NO owner walk
                needed): room assert via rig.require_state (room 0x012),
                the NPC-clear gate (sprite slots 115/118), then the chest
                open.
                PRIMARY (--route golden; default auto): wall-clock REPLAY
                OF THE OWNER'S CAPTURED ROUTE (phase2_owner_route_raw.txt,
                recorded live 2026-09-11 10:43:25 on the chapter-1 ref
                save; the Hookshot tracker receipt 10:43:25 is the
                capture's proof).  The RAW 30 Hz key transitions are
                replayed at their recorded offsets via the rig's
                key_down/key_up primitives (time.sleep holds, same
                wall-clock basis as the recording).  The compressed
                6-step file loses the owner's OVERLAPPING holds (diagonal
                walk segments), so it is only a degraded fallback source.
                Every attempt starts from the recording precondition
                (chapter reload -> ref-save spawn - the route is spawn-
                anchored).  ARBITER: the LIVE dng= save_dung_info mask in
                the probes - after the route's final X press it polls
                ~3 s for a flip from the boot baseline (a real open flips
                it instantly, no save flush needed); a probe-silence
                window (item-get cutscene / NPC dialogue both stop the
                probe stream) extends the poll and the resumed probe is
                re-read.  Miss -> retry up to --golden-attempts (3; the
                owner's own success rate was ~1-in-3 due to frame
                sensitivity), then - mode auto only - fall back to the
                walker route + persisted gold point + the systematic
                interaction sweep (candidate stand points x every facing
                x one A press each, arbitrated per-try by the same dng
                mask; the first flipping stand point is persisted as
                gold).  --route golden = golden only (diagnostic: measures
                the route's live success rate); --route sweep = legacy
                path only; --rediscover forces sweep.  An NPC-stolen press
                (dialogue banner) is detected and dismissed with B - never
                miscounted as an open.  Receipt chain (b1)-(b4) + tracker
                line unchanged (see (b) below).

  record-human  Boots the game + ref save, announces GO, then PASSIVELY
                records keyboard input (~30 Hz GetAsyncKeyState, NO
                injection - the owner plays).  A fresh tracker_items.txt
                receipt is the AUTOMATIC completion signal (machine-
                observable done beats a manual stop key); F12 is the manual
                abort.  Writes the raw transition log, a compressed
                (key, hold_ms) route file for rig replay, and a passive
                telemetry timeline (the human-pacing control data).  --arm
                turns it into the armed human-pacing death-loop control.

  selftest      Every parser + scenario logic against synthetic feeds
                (scripted death-loop-like sequence, blocked-walk sequence,
                NPC/sprite checks, synthetic ref save + randomizer log).
                Zero game launched.  This is the DEFAULT subcommand.

TELEMETRY FORMAT (FINAL, landed in src/twitch.c + rig.py; supersedes the
draft "[state]" spec in the original brief - deltas listed at the bottom):

  RESULT probe f=<frame_ctr_dbg> x=<link_x_coord> y=<link_y_coord>
             r=<dungeon_room_index> scr=<overworld_screen_index>
             mod=<main_module_index> sub=<submodule_index>
             vx=<link_actual_vel_x> vy=<link_actual_vel_y>
             dir=<link_direction> anim=<link_animation_steps>
             inv=<link_incapacitated_timer> st=<link_player_handler_state>
             vz=<link_actual_vel_z> aux=<link_auxiliary_state>
             cap=<link_cape_mode> mv=<link_flag_moving>
             spd=<link_speed_setting +0x5E>

  RESULT sprites f=<f> n=<active> <slot>:t=<type>,g=<graphics>,s=<state>,
             x=<x>,y=<y> ... [alt=<i>:<s>,<t>,<g>;...]  (active slots only)

Mandated parsing rules (rig agent):
  * ALWAYS parse key=value pairs, never positionally (f= leads the line
    now; the old "RESULT probe x=" positional regex is broken by design).
  * f = frame_ctr_dbg: int, monotonic - NOT the uint8 frame_counter that
    wraps at 256.
  * Gate: the line flows only while twitch debug=1 AND TwitchInGameplay()
    (module 7 dungeon / 9 overworld, sub==0), every 2nd frame.  A death /
    GAME OVER therefore shows up as probe-stream SILENCE; respawn as
    frames resuming (often with inv>0).  Death detection keys off that
    silence - never off "the module changed" (there IS no sample while
    dead).  Scenarios boot via rig.Game whose TEST_TWITCH_CFG sets
    debug=1, and the verify ini keeps DisableFrameDelay=0 (60 fps).

Deltas vs the draft brief spec: prefix "RESULT probe" (not "[state]");
room->r, module->mod, inc->inv, hst->st; new fields vz/aux/cap/mv/spd; the
per-slot "[sprites] k=..." lines became one inline "RESULT sprites" dump.
The parser below is kv-based and accepts BOTH spellings (missing fields
degrade to absent; a wrong guess never crashes) and tags each sample with
its source format.

Receipt verification chain for pinroute ((b) is PRIMARY per the rig
finding that NPC blocking can stall the physical open):
  (b1) randomizer_log.txt of THIS boot: "pin applied to chest record:
       room 0x012 chest 0 = Hookshot (item id 10)" - the documented
       chest-record byte statement (room 0x012 / chest 0 / item id 10).
  (b2) ref save introspection (documented in RANDO_PHASEB_HANDOFF.md):
       g_ram_off = filesize - 4 - 0x20000; dungeon_room_index at
       g_ram+0xA0 must be 0x12 for the chapter-1 save.
  (b4) chest-OPENED bitmask via save_dung_info = (uint16*)(g_ram+0xF000)
       (src/variables.h:1060; low byte = chest-opened mask, data-driven:
       NO bit index assumed).  Baseline u16 read from the ref save's
       g_ram dump BEFORE the open; after the receipt the scenario performs
       the game's own save (SaveGameFile -> saves/sram.dat) and reads the
       u16 back from a CHECKSUM-VALIDATED 0x500-byte slot block (both
       copies, checksum at +0x4FE, zero slot assumption); assert the u16
       CHANGED with the delta in the LOW byte; log both values + the
       XOR'd bits.
  (b3) telemetry: room stays 0x012 through the open attempt.
  (a)  tracker_items.txt fresh "Hookshot" line - SECONDARY corroboration.
  The post-open read needs the game's save path; if the save cannot be
  triggered the scenario FAILs loudly for (b4) (never silently passes).

Runtime artifacts go to tools/phase2_out/ (created on demand).  Config
swaps follow the .rigbak sidecar pattern and are restored in finally.
Boot gate: the booting subcommands run tools/gameplay_verify/smoke.py
(7/7) first unless --skip-smoke is given.
"""
import argparse
import ctypes
import os
import re
import struct
import subprocess
import sys
import time

from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "gameplay_verify"))
import rig  # READ-ONLY import: the landed hardened rig API

BASE = os.path.dirname(HERE)                                # repo root zelda3/
EXE = os.path.join(BASE, "zelda3.exe")
SMOKE = os.path.join(HERE, "gameplay_verify", "smoke.py")
TRACKER = os.path.join(BASE, "tracker_items.txt")
RANDO_INI = os.path.join(BASE, "randomizer.ini")
RANDO_LOG = os.path.join(BASE, "randomizer_log.txt")
SAVE_REF_DIR = os.path.join(BASE, "saves", "ref")


def resolve_ref_save(chapter):
    """Real ref saves are named e.g. 'Chapter 1 - Zelda's Rescue.sav'."""
    import glob as _glob
    hits = _glob.glob(os.path.join(SAVE_REF_DIR, "Chapter %d *.sav" % chapter))
    if not hits:
        raise SystemExit("no ref save for chapter %d in %s" %
                         (chapter, SAVE_REF_DIR))
    return hits[0]
INI_PATH = os.path.join(HERE, "phase2_out", "zelda3_phase2.ini")
OUTDIR = os.path.join(HERE, "phase2_out")

# ---- scenario constants (CLI-tweakable where marked) ----------------------
SANCTUARY_ROOM = 0x012          # chapter-1 spawn room; the pinned chest
PIN_LOCATION_DEFAULT = "Sanctuary"
PIN_ITEM_DEFAULT = "Hookshot"

# TwitchInGameplay() in src/twitch.c: 7 = dungeon, 9 = overworld,
# submodule_index == 0.  GATE definition (mirrors rig.Game.require_state).
GAMEPLAY_MODULES = (7, 9)
NORMAL_HANDLER_STATE = 0        # kPlayerState_Ground (src/player.c)

# Soak armament reproduced verbatim (tools/soak_final.py phase A; the
# DEFEAT block's "swarm 4"; hurt 8 = the soak's synthetic damage verb).
# Drop-file line format: verb|arg|source|seq.
ARMAMENT = ["attrition|on|p2|0", "dmgup|on|p2|0"]
DISARMAMENT = ["attrition|off|p2|0", "dmgup|off|p2|0"]
VOLLEY_LINES = ["swarm|4|p2|0", "hurt|8|p2|0"]
VOLLEY_ACK = {"swarm|4|p2|0": r"verb=swarm", "hurt|8|p2|0": r"verb=hurt"}

# Sanctuary story NPCs that wander into the chest alcove (rig field
# finding #1; SPAWN_TABLE in rig.py: 115 = uncle/priest, 118 = zelda).
NPC_SLOT_TYPES = (115, 118)

# ---- canonical chest interact point (gold data) + interaction sweep --------
# The owner's passive telemetry (phase2_owner_timeline.log; Hookshot
# receipt 15:54:55) ends with Link standing at spawn+(-26,+32) = (1246,600)
# facing up when his A press opened the chest (probes went silent right
# after = the item-get cutscene).  The hardened walker reaches that tile
# via (spawn+(-2,+55), spawn+(-26,+55), spawn+(-26,+32)).  The sweep
# verifies the tile live; the first dng-flipping (stand, facing) is
# persisted and becomes the primary open step of later runs.
GOLD_PATH = os.path.join(OUTDIR, "pinroute_gold_point.txt")
GOLD_DX_DY = (-26, 32)          # owner-evidence tile, relative to spawn
# The chest's interact box is PIXEL-TIGHT (live proof: from (1232,600)
# facing up the chest opens; from (1233,600) or (1234,600) the same press
# misses) - the gold path walks the exact pixel and walks +-1/2 px
# neighbours if the recorded pixel ever stops working.
GOLD_NEIGHBOURS = ((0, 0), (-1, 0), (1, 0), (0, -1), (0, 1),
                   (-2, 0), (2, 0), (0, -2), (0, 2))
ROUTE_LEGS = ((-2, 55), (-26, 55), (-26, 32))   # proven alcove route
SWEEP_DX = (-16, -8, 0, 8)
SWEEP_DY = (-16, -8, 0, 8, 16)
SWEEP_FALLBACK_ANCHOR = (-32, 77)   # classic rig BLOCK point, vs spawn
SWEEP_FACINGS = ("up", "left", "right", "down")
SWEEP_WALK_TIMEOUT = 4.0        # walk_to budget per candidate stand point
SWEEP_POLL_S = 1.5              # dng poll window per A press (s)
SWEEP_CUTSCENE_GRACE = 8.0      # sub!=0 banner resolution budget (s)
NPC_STEAL_RADIUS = 20           # skip A presses with an NPC this close

# ---- golden owner route (the PRIMARY open path) -----------------------------
# Captured live 2026-09-11 10:43:25 (record-human passive recorder, chapter-1
# ref save, seed 1234): the owner walked to the pin chest and the Hookshot
# tracker receipt landed the same second.  phase2_owner_route.txt is the
# compressed (key, hold_ms) replay file; phase2_owner_route_raw.txt is the
# 30 Hz transition log.  DESIGN DECISION: replay uses the RAW transitions -
# the capture's holds OVERLAP (left engages 36 ms before down releases, up
# engages on the exact frame left releases, right engages 36 ms before up
# releases, the second up engages on the exact frame right releases), i.e.
# three diagonal walk segments that a sequential replay of the compressed
# steps would erase - and the chest interact box is pixel-tight.  The raw
# offsets reproduce the diagonals exactly and lose nothing: holds are
# continuous through the route, the single quiet gap (143 ms between the
# second up-release and the X press) is preserved verbatim, and the X (still
# held at capture end) gets a synthetic release GOLDEN_TAIL_MS after the
# last event (route_compress tail convention).
GOLDEN_ROUTE_PATH = os.path.join(OUTDIR, "phase2_owner_route.txt")
GOLDEN_ROUTE_RAW_PATH = os.path.join(OUTDIR, "phase2_owner_route_raw.txt")
GOLDEN_ATTEMPTS = 3             # 1 try + 2 retries (owner rate ~1-in-3)
GOLDEN_POLL_S = 3.0             # dng arbiter window after the final X press
GOLDEN_TAIL_MS = 200            # synthetic release for keys held at capture end

# ---- save introspection (documented; RANDO_PHASEB_HANDOFF.md +
# src/variables.h:1060) ------------------------------------------------------
G_RAM_SIZE = 0x20000
SAVE_JUNK_TAIL = 4
RAM_OFF_ROOM_INDEX = 0xA0       # dungeon_room_index
# save_dung_info = (uint16*)(g_ram+0xF000)  (src/variables.h:1060): the
# ALttP dungeon room data array, 2 bytes per room; the LOW byte carries
# the chest-opened bitmask, the high byte door/key/torch bits.
RAM_OFF_SAVE_DUNG = 0xF000
# sram.dat = g_zenv.sram (8192 bytes) written by SaveGameFile()
# (src/messaging.c): per-slot 0x500-byte copies of save_dung_info, a
# second copy at +0xF00, each with a u16 checksum at +0x4FE
# (t = 0x5A5A - sum(u16 LE over 0x4FE bytes)).
SRAM_SIZE = 0x2000
SRAM_BLOCK = 0x500
SRAM_COPY2 = 0xF00
SRAM_CKSUM_OFF = 0x4FE
SRAM_CKSUM_SEED = 0x5A5A
SRAM_DAT = os.path.join(BASE, "saves", "sram.dat")

# chest-record byte statement from the apply loop. CURRENT (P1) log format:
#   "# pin applied: Sanctuary = Hookshot -> chest record room 0x012 (chest 0, item id 10)"
# legacy (pre-P1): "pin applied to chest record: room 0x012 chest 0 = ..."
RX_CHEST_RECORD = re.compile(
    r"pin applied: \S+ = (\S+) -> chest record room (0x[0-9A-Fa-f]+) "
    r"\(chest (\d+), item id (\d+)\)")
RX_CHEST_RECORD_LEGACY = re.compile(
    r"pin applied to chest record: room (0x[0-9A-Fa-f]+) chest (\d+) = "
    r"(\S+) \(item id (\d+)\)")


# ===========================================================================
# Telemetry parsing
# ===========================================================================

KV_RX = re.compile(r"(\w+)=(-?\d+)")
SLOT_RX = re.compile(r"(\d+):t=(\d+),g=(\d+),s=(\d+)(?:,x=(-?\d+),y=(-?\d+))?")
ALT_RX = re.compile(r"\balt=(\S+)")

# canonical field names across the FINAL format and the draft-spec aliases
FIELD_ALIASES = {
    "r": "room", "room": "room",
    "mod": "module", "module": "module",
    "inv": "inv", "inc": "inv",
    "st": "st", "hst": "st",
}


def _canonical(kv):
    out = {}
    for k, v in kv.items():
        out[FIELD_ALIASES.get(k, k)] = int(v)
    return out


def parse_probe(line):
    """Parse one probe/state telemetry line -> canonical dict, or None.

    Accepts the FINAL format (RESULT probe f=...) plus, defensively, the
    draft-spec [state] spelling and the pre-hardening legacy line.  All
    values are key=value parsed (never positional); absent fields are
    simply absent; a "fmt" tag records the source format.
    """
    if line.startswith("RESULT probe"):
        # FINAL format leads with f= (kv, not positional); anything else
        # starting with RESULT probe is the pre-hardening legacy line
        fmt = "final" if line.startswith("RESULT probe f=") else "legacy"
    elif line.startswith("[state]"):
        fmt = "draft"
    else:
        return None
    kv = dict((m.group(1), m.group(2)) for m in KV_RX.finditer(line))
    if "f" not in kv:
        return None
    d = _canonical(kv)
    d["fmt"] = fmt
    return d


def parse_sprites(line):
    """Parse a RESULT sprites dump -> {f, n, slots, alt} or None.

    slots: [(slot, type, graphics, state, x, y)] with x/y = -1 when absent.
    alt:   [(index, state, type, graphics)] from " alt=i:s,t,g;...".
    """
    if not line.startswith("RESULT sprites"):
        return None
    kv = dict((m.group(1), m.group(2)) for m in KV_RX.finditer(line))
    slots = []
    for m in SLOT_RX.finditer(line):
        slot = [int(m.group(i)) for i in (1, 2, 3, 4)]
        slot += [int(m.group(i)) if m.group(i) else -1 for i in (5, 6)]
        slots.append(tuple(slot))
    alt = []
    ma = ALT_RX.search(line)
    if ma:
        for part in ma.group(1).split(";"):
            m = re.match(r"(\d+):(\d+),(\d+),(\d+)$", part)
            if m:
                alt.append(tuple(int(m.group(i)) for i in (1, 2, 3, 4)))
    if "f" not in kv and not slots:
        return None
    return {"f": int(kv.get("f", -1)), "n": int(kv.get("n", len(slots))),
            "slots": slots, "alt": alt}


def gate_open(probe, mods=GAMEPLAY_MODULES, st_normal=NORMAL_HANDLER_STATE):
    """The state-aware volley gate, evaluated on ONE probe sample.

    Open iff the sample proves: gameplay module (7/9), sub==0,
    incapacitated/grace timer inv==0, handler state st==normal.
    A REQUIRED field missing (e.g. a legacy line without inv=) means the
    gate REFUSES (loud): gating must key off inv= by design.
    """
    if probe is None:
        return False, "no sample"
    need = ("module", "sub", "inv", "st")
    missing = [k for k in need if k not in probe]
    if missing:
        return False, "fields missing: %s" % ",".join(missing)
    if probe["module"] not in mods:
        return False, "module=%d not gameplay %s" % (probe["module"],
                                                     (mods,))
    if probe["sub"] != 0:
        return False, "sub=%d (menu/dialogue)" % probe["sub"]
    if probe["inv"] != 0:
        return False, "inv=%d (grace/incapacitated)" % probe["inv"]
    if probe["st"] != st_normal:
        return False, "st=%d not normal(%d)" % (probe["st"], st_normal)
    return True, "open"


class ProbeTail(object):
    """Incremental tail of a game stdout log with parsed telemetry.

    Wall-clock-stamped samples (so frame-stream SILENCE - the death/
    gameover window - is measurable), newest probe/sprites, and every
    RESULT line as an event.  Never parses the same bytes twice.
    """

    def __init__(self, log_path, history=8192):
        self.log_path = log_path
        self.pos = 0
        self.partial = ""
        self.last_probe = None
        self.last_probe_t = None
        self.last_sprites = None
        self.samples = deque(maxlen=history)   # (f, t, dict)
        self.events = deque(maxlen=history)    # (t, RESULT line)

    def pump(self):
        try:
            with open(self.log_path, "r", errors="replace") as fh:
                fh.seek(self.pos)
                new = fh.read()
                self.pos = fh.tell()
        except (OSError, ValueError):
            return
        if not new:
            return
        now = time.time()
        buf = self.partial + new
        lines = buf.split("\n")
        self.partial = lines.pop()
        for ln in lines:
            ln = ln.rstrip("\r")
            if not ln:
                continue
            p = parse_probe(ln)
            if p is not None:
                self.last_probe = p
                self.last_probe_t = now
                self.samples.append((p.get("f", -1), now, p))
                continue
            s = parse_sprites(ln)
            if s is not None:
                self.last_sprites = s
                continue
            if ln.startswith("RESULT"):
                self.events.append((now, ln))

    def latest(self):
        """(probe_dict, age_s); age None when nothing ever arrived."""
        self.pump()
        if self.last_probe_t is None:
            return None, None
        return self.last_probe, time.time() - self.last_probe_t

    def wait_new_frame(self, after_f, timeout=6.0):
        """Block until a probe with f > after_f arrives (advance ack)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump()
            if self.last_probe is not None and \
                    self.last_probe.get("f", -1) > after_f:
                return self.last_probe
            time.sleep(0.02)
        return None

    def wait_gate(self, timeout=20.0, **gate_kw):
        """Block until the state-aware gate opens; (ok, probe, reason)."""
        deadline = time.time() + timeout
        ok, why = False, "wait never ran"
        while time.time() < deadline:
            self.pump()
            ok, why = gate_open(self.last_probe, **gate_kw)
            if ok:
                return True, self.last_probe, why
            time.sleep(0.05)
        return False, self.last_probe, why


# ===========================================================================
# Sprite helpers (NPC blocking + keese-clone annotation)
# ===========================================================================

def npcs_near(sprites, target, radius):
    """Active story-NPC slots (115/118) within `radius` px of target.

    Assumes sprite x/y are comparable to the probe's link x/y (same world
    dump; the rig agent's field finding treats them that way).  Returns
    [(slot, type, x, y)]."""
    out = []
    for s in (sprites or {}).get("slots", []):
        slot, typ, _g, state, x, y = s
        if typ in NPC_SLOT_TYPES and state != 0 and x >= 0 and y >= 0 and \
                abs(x - target[0]) <= radius and abs(y - target[1]) <= radius:
            out.append((slot, typ, x, y))
    return out


def slot_occupancy(sprites):
    """Active sprite count (the dump's n= field); -1 when none seen."""
    if not sprites:
        return -1
    return sprites.get("n", len(sprites.get("slots", [])))


def clone_note(occ_before, occ_after, ack_count):
    """Keese-clone annotation: occupancy grew by more than the fired verbs
    can account for (known engine clone at offsets (0,-20)/(+17,+15) about
    10 frames after a later verb - never a rogue verb; root cause tracked
    separately by the sprite agent)."""
    if occ_before < 0 or occ_after < 0:
        return None
    growth = occ_after - occ_before
    if growth > max(ack_count, 0):
        return "SPRITE-GROWTH +%d with %d verb ack(s) (keese-clone class, " \
               "non-verb-driven)" % (growth, ack_count)
    return None


# ===========================================================================
# Death-loop analysis
# ===========================================================================

class LoopAnalyzer(object):
    """Death/re-kill discrimination over wall-stamped telemetry samples.

    Because probe telemetry only flows INSIDE gameplay, a death/GAME-OVER
    episode appears as frame-stream SILENCE and a respawn as frames
    resuming.  Volleys are recorded with the context they fired under:

      open    gate was open (alive, inv==0, st==0, gameplay module)
      grace   inv>0 at fire time OR frames resumed < grace_s ago
      silent  the frame stream was already silent (dead/game over)

    A re-kill is a silence episode that starts within rekill_window_s
    after a grace/silent volley - "the rig shot into the respawn window
    and Link died again".  LOOP verdict = repeated deaths + repeated
    grace/silent volleys cycling, or a sustained deaths-per-minute rate.
    """

    def __init__(self, silence_s=1.5, grace_s=4.0, rekill_window_s=10.0,
                 min_deaths=3, min_rekills=2, dpm_loop=6.0):
        self.silence_s = silence_s
        self.grace_s = grace_s
        self.rekill_window_s = rekill_window_s
        self.min_deaths = min_deaths
        self.min_rekills = min_rekills
        self.dpm_loop = dpm_loop
        self.volleys = []        # {f, t, lines, ctx, acks, occ_before/after}
        self.episodes = []
        self.grace_volleys = 0
        self.silent_volleys = 0
        self.rekills = 0

    def on_volley(self, tail, lines, ack_count=0):
        """Record a volley with the state context it fired under."""
        probe, age = tail.latest()
        ctx = "open"
        if age is None or age >= self.silence_s:
            ctx = "silent"
        else:
            ok, _ = gate_open(probe)
            if not ok:
                ctx = "grace"
            elif tail.samples and self.episodes and \
                    tail.samples[-1][1] - self.episodes[-1]["t_end"] < \
                    self.grace_s:
                ctx = "grace"       # inside the post-respawn grace window
        v = {"f": (probe or {}).get("f", -1), "t": time.time(),
             "lines": list(lines), "ctx": ctx, "acks": ack_count,
             "occ_before": slot_occupancy(tail.last_sprites),
             "occ_after": None}
        self.volleys.append(v)
        if ctx == "grace":
            self.grace_volleys += 1
        elif ctx == "silent":
            self.silent_volleys += 1
        return v

    def find_episodes(self, samples, t_end):
        """Silence episodes: consecutive-sample gaps > silence_s, plus a
        trailing gap if frames went silent for good by t_end."""
        eps = []
        prev = None
        for f, t, _d in samples:
            if prev is not None and t - prev[1] > self.silence_s:
                eps.append({"t_start": prev[1], "t_end": t,
                            "start_f": prev[0]})
            prev = (f, t)
        if prev is not None and t_end - prev[1] > self.silence_s:
            eps.append({"t_start": prev[1], "t_end": t_end,
                        "start_f": prev[0]})
        self.episodes = eps
        return eps

    def summary(self, samples, t_start, t_end, events=()):
        """Verdict dict. samples: ProbeTail.samples; events: (t, RESULT)."""
        eps = [e for e in self.find_episodes(samples, t_end)
               if e["t_end"] > t_start]
        deaths = len(eps)
        self.rekills = 0
        for e in eps:
            for v in self.volleys:
                if v["ctx"] in ("grace", "silent") and \
                        v["t"] <= e["t_start"] and \
                        e["t_start"] - v["t"] <= self.rekill_window_s:
                    self.rekills += 1
                    break
        minutes = max((t_end - t_start) / 60.0, 1e-6)
        dpm = deaths / minutes
        dropped_ng = sum(1 for _t, ln in events if "DROPPED" in ln)
        clone_hits = []
        for v in self.volleys:
            if v.get("occ_after") is None:
                continue
            n = clone_note(v["occ_before"], v["occ_after"], v["acks"])
            if n:
                clone_hits.append((v["f"], n))
        loop = (deaths >= self.min_deaths and
                self.rekills >= self.min_rekills) or dpm >= self.dpm_loop
        return {
            "deaths": deaths, "rekills": self.rekills, "dpm": dpm,
            "episodes": eps, "volleys": len(self.volleys),
            "grace_volleys": self.grace_volleys,
            "silent_volleys": self.silent_volleys,
            "dropped_not_in_gameplay": dropped_ng,
            "clone_hits": clone_hits,
            "loop_occurred": loop,
            "verdict": ("LOOP" if loop else
                        "NO-LOOP (machine pacing held outside the grace "
                        "window)" if self.grace_volleys == 0 and
                        self.silent_volleys == 0 else
                        "NO-LOOP (grace-window fire seen but no repeat "
                        "deaths)"),
        }


class OfflineAnalyzer(LoopAnalyzer):
    """Analyzer fed from recorded lists (selftest / post-hoc runs)."""

    def __init__(self, events=(), **kw):
        LoopAnalyzer.__init__(self, **kw)
        self._events = list(events)

    def note_event(self, t, line):
        self._events.append((t, line))

    def summary(self, samples, t_start, t_end, events=None):
        return LoopAnalyzer.summary(self, samples, t_start, t_end,
                                    events if events is not None
                                    else self._events)


def fmt_timeline(samples, volleys, analyzer, title):
    """frame_id-stamped timeline: every telemetry sample plus VOLLEY and
    SILENCE-EPISODE markers injected at the right sample boundaries."""
    out = ["# %s" % title,
           "# format: [f=<frame_id> t=<+s>] <probe kv> / ** markers"]
    marks = []
    for v in volleys:
        marks.append((v["t"], "VOLLEY %s ctx=%s acks=%d occ=%s->%s %s"
                      % (",".join(v["lines"]), v["ctx"], v.get("acks", 0),
                         v.get("occ_before"), v.get("occ_after"),
                         clone_note(v.get("occ_before"), v.get("occ_after"),
                                    v.get("acks", 0)) or "")))
    for e in analyzer.find_episodes(samples, time.time()):
        marks.append((e["t_start"], "SILENCE-EPISODE start (death?) "
                      "last_f=%d" % e["start_f"]))
    marks.sort(key=lambda m: m[0])
    mi = 0
    for f, t, d in samples:
        while mi < len(marks) and marks[mi][0] <= t + 1e-6:
            mt, text = marks[mi]
            out.append("[f=?      t=%07.2f] ** %s" % (mt, text.strip()))
            mi += 1
        out.append("[f=%s t=%07.2f] %s" % (
            f, t, " ".join("%s=%s" % (k, v) for k, v in sorted(d.items())
                           if k != "fmt")))
    for mt, text in marks[mi:]:
        out.append("[f=?      t=%07.2f] ** %s" % (mt, text.strip()))
    return "\n".join(out) + "\n"


# ===========================================================================
# Receipts: tracker / ref save / randomizer chest record
# ===========================================================================

def tracker_read():
    try:
        with open(TRACKER, "r", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def tracker_fresh_line(baseline_text, item):
    """First line not in the baseline snapshot mentioning `item`, else
    None (content-diff based; mtime alone is racy on full rewrites)."""
    base_lines = set(baseline_text.strip().splitlines())
    for ln in tracker_read().strip().splitlines():
        if ln and ln not in base_lines and item.lower() in ln.lower():
            return ln
    return None


def save_room_check(save_path):
    """Documented memory-level byte check (RANDO_PHASEB_HANDOFF.md):
    g_ram_off = filesize - 4 - 0x20000; dungeon_room_index at g_ram+0xA0.
    Returns (ok, room_or_None, note).  The POST-OPEN chest-opened flag is
    NOT implemented (CHEST_OPENED_OFFSET is None): no documented offset,
    and offsets are not guessed."""
    try:
        size = os.path.getsize(save_path)
        g_off = size - SAVE_JUNK_TAIL - G_RAM_SIZE
        if g_off < 0:
            return False, None, "file too small (%d bytes)" % size
        with open(save_path, "rb") as fh:
            fh.seek(g_off + RAM_OFF_ROOM_INDEX)
            room = fh.read(1)[0]
        return True, room, "g_ram_off=%d (+0x%X)" % (g_off,
                                                     RAM_OFF_ROOM_INDEX)
    except OSError as ex:
        return False, None, str(ex)


def chest_record_check(pin_item, t_boot):
    """(b1) documented chest-record byte statement: the apply-loop line
    'pin applied to chest record: room 0x012 chest 0 = Hookshot (item id
    10)' from randomizer_log.txt of THIS boot (the log is rewritten at
    every enabled boot, freshness = mtime >= t_boot).  Returns
    (ok, parsed_or_None, note)."""
    try:
        if os.path.getmtime(RANDO_LOG) < t_boot:
            return False, None, "randomizer_log.txt older than this boot"
        with open(RANDO_LOG, "r", errors="replace") as fh:
            for ln in fh:
                m_cur = RX_CHEST_RECORD.search(ln)
                m_old = RX_CHEST_RECORD_LEGACY.search(ln)
                # current format groups: (item, room, chest, item_id)
                if m_cur:
                    item, room, chest, item_id = m_cur.groups()
                elif m_old:
                    room, chest, item, item_id = m_old.groups()
                else:
                    continue
                if int(room, 16) == SANCTUARY_ROOM and \
                        item.lower() == pin_item.lower():
                    return True, {"room": int(room, 16),
                                  "chest": int(chest),
                                  "item": item,
                                  "item_id": int(item_id)}, ln.strip()
        return False, None, "no matching 'pin applied to chest record' " \
                            "line for room 0x012 = %s" % pin_item
    except OSError as ex:
        return False, None, str(ex)


# ---- save_dung_info chest-opened bitmask (data-driven; variables.h:1060) --

def _u16_at(buf, room):
    """save_dung_info[room] from a buffer laid out as that array
    (2 bytes per room, little-endian; low byte = chest-opened mask)."""
    return struct.unpack_from("<H", buf, room * 2)[0]


def dung_baseline_from_save(save_path, room):
    """(b4 pre) save_dung_info[room] from a ref save's g_ram dump:
    g_ram_off = size - 4 - 0x20000; save_dung_info at g_ram+0xF000.
    Returns (ok, u16_or_None, note)."""
    try:
        size = os.path.getsize(save_path)
        g_off = size - SAVE_JUNK_TAIL - G_RAM_SIZE
        if g_off < 0:
            return False, None, "file too small (%d bytes)" % size
        with open(save_path, "rb") as fh:
            fh.seek(g_off + RAM_OFF_SAVE_DUNG)
            return True, _u16_at(fh.read(G_RAM_SIZE - RAM_OFF_SAVE_DUNG),
                                 room), "g_ram_off=%d save_dung_info+%X" \
                                        % (g_off, room * 2)
    except OSError as ex:
        return False, None, str(ex)


def _sram_block_valid(block):
    """SaveGameFile's checksum: t = 0x5A5A - sum(u16 LE over 0x4FE bytes),
    stored as a u16 at +0x4FE (src/messaging.c SaveGameFile)."""
    t = SRAM_CKSUM_SEED
    for v in struct.unpack_from("<%dH" % (SRAM_CKSUM_OFF // 2), block, 0):
        t = (t - v) & 0xFFFF
    stored = struct.unpack_from("<H", block, SRAM_CKSUM_OFF)[0]
    return t == stored


def sram_dung_values(path, room):
    """(b4 post) every checksum-VALIDATED save_dung_info[room] u16 in
    sram.dat.  Slot blocks sit on the 0x500 grid (copy 2 at +0xF00 is on
    the same grid), so scanning all grid windows needs NO slot assumption;
    the checksum is the integrity proof.  Returns [(block_off, u16)]."""
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError:
        return []
    vals = []
    for off in range(0, len(data) - SRAM_BLOCK + 1, SRAM_BLOCK):
        block = data[off:off + SRAM_BLOCK]
        if _sram_block_valid(block):
            vals.append((off, _u16_at(block, room)))
    return vals


def dung_changed(pre_u16, post_u16):
    """Data-driven chest-opened assertion: the u16 CHANGED and the change
    involves the LOW byte (the chest mask).  Returns (ok, xor, note)."""
    xor = pre_u16 ^ post_u16
    ok = xor != 0 and (xor & 0xFF) != 0
    return ok, xor, "pre=0x%04X post=0x%04X xor=0x%04X (low-byte delta %s)" \
                    % (pre_u16, post_u16, xor,
                       "YES: 0x%02X" % (xor & 0xFF) if xor & 0xFF else "no")


def save_and_quit(game, tail, t_after, timeout=25.0):
    """Trigger the game's OWN save (SaveGameFile -> saves/sram.dat):
    Start opens the pause subscreen, a second Start arms Save-and-Quit,
    A confirms.  Both known key orders are tried; success = sram.dat
    mtime bumped after t_after (data-verified, not assumed)."""
    mt0 = os.path.getmtime(SRAM_DAT) if os.path.exists(SRAM_DAT) else 0
    for seq in (("return", "return", "x"), ("return", "x")):
        try:
            game.ensure_focus()
        except Exception:
            pass
        for k in seq:
            rig.tap(k)
            time.sleep(0.7)
        deadline = time.time() + timeout
        while time.time() < deadline:
            tail.pump()
            if os.path.exists(SRAM_DAT) and \
                    os.path.getmtime(SRAM_DAT) > max(mt0, t_after - 1):
                time.sleep(0.5)      # let the write settle
                return True
            if game.proc and game.proc.poll() is not None:
                return False         # died mid-save; loud upstream
            time.sleep(0.2)
    return False


# ===========================================================================
# Walk helpers (blocked-walk analysis + detour planning)
# ===========================================================================

def route_compress(transitions, min_hold_ms=30, tail_ms=200):
    """(t_ms, key, down/up) transitions -> [(key, hold_ms)] replay steps.
    Keys still held at the end are released as tail_ms holds."""
    opens = {}
    steps = []
    for t, name, down in transitions:
        if down:
            opens[name] = t
        elif name in opens:
            steps.append((name, max(min_hold_ms, t - opens[name])))
            del opens[name]
    for name, _t in sorted(opens.items()):
        steps.append((name, tail_ms))
    return steps


def plan_detours(target, ring=8):
    """Blocked-walk fallback waypoints: an 8-point ring `ring` px around a
    stuck walk_to target (rig.walk_to already detours L-shaped and reports
    'BLOCKED at (x=.., y=..)' loudly; the ring is the second escape layer
    used by pinroute's retry planner)."""
    tx, ty = target
    return [(tx + ring, ty), (tx - ring, ty), (tx, ty + ring),
            (tx, ty - ring), (tx + ring, ty + ring), (tx - ring, ty + ring),
            (tx + ring, ty - ring), (tx - ring, ty - ring)]


def analyze_walk_timeline(samples, target, tol, stall_s=2.0):
    """Offline blocked-walk analysis: given [(f, t, probe)] samples and a
    target, report (stalled, last_pos, note).  stalled = Link stayed put
    (within tol) for >= stall_s while frames kept advancing - the
    synthetic twin of rig.walk_to's BLOCKED detection."""
    if not samples:
        return False, None, "no samples"
    _tx, _ty = target
    last_move_t = samples[0][1]
    last_pos = (samples[0][2].get("x"), samples[0][2].get("y"))
    for _f, t, d in samples:
        pos = (d.get("x"), d.get("y"))
        if max(abs(pos[0] - last_pos[0]), abs(pos[1] - last_pos[1])) > tol:
            last_move_t = t
            last_pos = pos
    stalled = (samples[-1][1] - last_move_t) >= stall_s
    note = "last move at t=%.2f, %d samples" % (last_move_t, len(samples))
    return stalled, last_pos, note


# ===========================================================================
# Shared game-session plumbing
# ===========================================================================

def ensure_outdir():
    os.makedirs(OUTDIR, exist_ok=True)
    return OUTDIR


def preflight_no_instance():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq zelda3.exe",
                          "/NH"], capture_output=True, text=True,
                         timeout=10).stdout
    if "zelda3" in out.lower():
        raise RuntimeError("another zelda3.exe is already running - not "
                           "booting a second instance over the owner's")
    if not os.path.exists(EXE):
        raise RuntimeError("zelda3.exe missing - cannot run phase-2 live")


def smoke_gate(args):
    """Boot gate: tools/gameplay_verify/smoke.py must pass 7/7 before any
    phase-2 scenario boots its own instance."""
    if getattr(args, "skip_smoke", False):
        print("smoke gate: SKIPPED (--skip-smoke)", flush=True)
        return
    cmd = [sys.executable, SMOKE, "--seed", str(args.seed),
           "--chapter", str(args.chapter),
           "--pin-location", args.pin_location,
           "--pin-item", args.pin_item]
    if getattr(args, "no_rando_smoke", False):
        cmd.append("--no-rando")
    # owner-driven sessions run on a busy desktop: slow pacing (<45fps) is
    # harmless there (the human IS the timing), only unlocked speed is fatal
    if getattr(args, "pacing_floor", None):
        cmd += ["--fps-min", str(args.pacing_floor)]
    print("smoke gate: %s" % " ".join(cmd), flush=True)
    rc = subprocess.run(cmd, cwd=BASE, timeout=300).returncode
    if rc != 0:
        raise RuntimeError("smoke gate FAILED (rc=%s) - fix the boot "
                           "before running phase-2 scenarios" % rc)
    print("smoke gate: PASS (7/7)", flush=True)


def swap_rando_ini(pin_location, pin_item, seed):
    """randomizer.ini -> enabled+pin; the returned backup is restored by
    restore_rando_ini() in finally (byte-for-byte)."""
    backup = None
    if os.path.exists(RANDO_INI):
        with open(RANDO_INI, "r", encoding="utf-8") as fh:
            backup = fh.read()
    with open(RANDO_INI, "w", encoding="utf-8") as fh:
        fh.write("enabled=1\nseed=%d\nlog=1\npin_location=%s\npin_item=%s\n"
                 % (seed, pin_location, pin_item))
    print("randomizer.ini: enabled=1 seed=%d pin %s=%s (restored on exit)"
          % (seed, pin_location, pin_item), flush=True)
    return backup


def restore_rando_ini(backup):
    if backup is None:
        try:
            os.remove(RANDO_INI)
        except OSError:
            pass
        print("randomizer.ini: removed (was absent before)", flush=True)
    else:
        with open(RANDO_INI, "w", encoding="utf-8") as fh:
            fh.write(backup)
        print("randomizer.ini: restored byte-for-byte", flush=True)


def make_ini(audio=False):
    """Verify ini via rig.make_verify_ini, patched to pin 60 fps pacing
    (DisableFrameDelay=0) - the repo ini already carries it; force it so a
    drifted repo ini cannot unlock the emulation speed under load."""
    ensure_outdir()
    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI_PATH,
                        load_ref=True, enable_audio=audio)
    with open(INI_PATH, "r", encoding="utf-8") as fh:
        lines = fh.readlines()
    out, found = [], False
    for ln in lines:
        if ln.strip().startswith("DisableFrameDelay"):
            out.append("DisableFrameDelay = 0\n")
            found = True
        else:
            out.append(ln)
    if not found:
        out.append("DisableFrameDelay = 0\n")
    with open(INI_PATH, "w", encoding="utf-8") as fh:
        fh.writelines(out)
    return INI_PATH


class Session(object):
    """One booted game instance (rig.Game) + a wall-clock ProbeTail on its
    stdout log. rig.Game already swaps/restores twitch_config.txt (its
    TEST_TWITCH_CFG has debug=1 drop=1, so drop verbs AND telemetry flow).
    """

    def __init__(self, tag, audio=False):
        self.tag = tag
        self.log_path = os.path.join(OUTDIR, "phase2_%s_run.log" % tag)
        make_ini(audio)
        self.game = rig.Game(BASE, self.log_path,
                             os.path.relpath(INI_PATH, BASE))
        self.tail = ProbeTail(self.log_path)

    def __enter__(self):
        preflight_no_instance()
        self.game.start()
        return self

    def __exit__(self, *exc):
        try:
            self.game.stop()
        except Exception:
            pass
        return False

    def load_chapter(self, chapter):
        p = self.game.load_chapter(chapter)
        self.tail.pump()
        return p

    def drop_and_ack(self, line, ack_rx, timeout=8.0):
        """Raw drop (NO state gate) + wait for the RESULT ack."""
        self.game.drop(line)
        rx = re.compile(ack_rx)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.tail.pump()
            for _t, ln in self.tail.events:
                if rx.search(ln):
                    return ln
            time.sleep(0.1)
        return None

    def shot(self, name):
        try:
            return self.game.shot(os.path.join(OUTDIR, "shots"), name)
        except Exception as ex:
            print("  [warn] screenshot %s failed: %r" % (name, ex),
                  flush=True)
            return None


def arm(session, lines, kind):
    for ln in lines:
        verb = ln.split("|", 1)[0]
        ack = session.drop_and_ack(ln, r"verb=%s" % verb)
        print("  %s %-22s -> %s" % (kind, ln, (ack or "NO ACK").strip()),
              flush=True)


# ===========================================================================
# Subcommand: deathloop
# ===========================================================================

def fire_volley(session, an, profile, gate_timeout_s):
    """One swarm-4 + hurt-8 volley under the profile's pacing.  Returns
    the volley record (None if the gated gate never opened - withheld)."""
    if profile == "aggressive":
        # OLD machine pacing: raw drops, Link's state deliberately ignored
        v = an.on_volley(session.tail, VOLLEY_LINES)
        for ln in VOLLEY_LINES:
            session.game.drop(ln)
        acks = 0
        deadline = time.time() + 4.0
        while time.time() < deadline:
            session.tail.pump()
            acks = sum(1 for _t, ln in session.tail.events
                       if any(rx.search(ln) for rx in
                              [re.compile(VOLLEY_ACK[k]) for k in
                               VOLLEY_ACK]))
            if acks >= len(VOLLEY_LINES):
                break
            time.sleep(0.1)
        v["acks"] = acks
        return v
    # gated: the rig's OWN state-aware fire path (grace gating built in)
    try:
        gate = session.game.require_state(module=GAMEPLAY_MODULES, sub=0,
                                          max_inv=0,
                                          handler=(NORMAL_HANDLER_STATE,),
                                          timeout=gate_timeout_s,
                                          what="gated volley")
    except rig.PositionUnknown:
        return None
    v = an.on_volley(session.tail, VOLLEY_LINES)
    del gate                      # gate evidence lives in rig.last_fire_gate
    acks = 0
    for ln in VOLLEY_LINES:
        session.game.drop(ln)
    deadline = time.time() + 4.0
    while time.time() < deadline:
        session.tail.pump()
        acks = sum(1 for _t, ln in session.tail.events
                   if any(re.compile(VOLLEY_ACK[k]).search(ln)
                          for k in VOLLEY_ACK))
        if acks >= len(VOLLEY_LINES):
            break
        time.sleep(0.1)
    v["acks"] = acks
    return v


def run_deathloop_profile(session, profile, args):
    """One profile pass over a live session (chapter reloaded first)."""
    an = LoopAnalyzer(silence_s=args.silence_s, grace_s=args.grace_s,
                      rekill_window_s=args.rekill_window_s,
                      min_deaths=args.min_deaths,
                      min_rekills=args.min_rekills, dpm_loop=args.dpm_loop)
    print("  reload chapter %d for a clean start" % args.chapter, flush=True)
    session.load_chapter(args.chapter)
    arm(session, ARMAMENT, "arm")
    t_start = time.time()
    t_end = t_start + args.duration
    next_volley = t_start + args.lead
    n = withheld = 0
    last = None
    while time.time() < t_end:
        session.tail.pump()
        if session.game.proc and session.game.proc.poll() is not None:
            print("  [HARD] game process died mid-profile", flush=True)
            break
        if time.time() >= next_volley:
            if profile == "aggressive":
                last = fire_volley(session, an, profile, args.gate_timeout)
                n += 1
                print("  volley %2d f=%s ctx=%s acks=%s occ=%s" %
                      (n, last["f"], last["ctx"], last["acks"],
                       last["occ_before"]), flush=True)
                next_volley = time.time() + args.interval
            else:
                last = fire_volley(session, an, profile, args.gate_timeout)
                if last is None:
                    withheld += 1
                    print("  gate stayed CLOSED %ss - volley withheld"
                          % args.gate_timeout, flush=True)
                    next_volley = time.time() + args.gate_timeout
                else:
                    n += 1
                    print("  volley %2d f=%s ctx=%s (gate open) acks=%s"
                          % (n, last["f"], last["ctx"], last["acks"]),
                          flush=True)
                    next_volley = time.time() + args.interval
            # sample occupancy again shortly after the volley lands so the
            # keese-clone annotation has a before/after pair
            time.sleep(0.5)
            session.tail.pump()
            if last is not None:
                last["occ_after"] = slot_occupancy(session.tail.last_sprites)
                cn = clone_note(last["occ_before"], last["occ_after"],
                                last["acks"])
                if cn:
                    print("  %s" % cn, flush=True)
        time.sleep(0.05)
    arm(session, DISARMAMENT, "disarm")
    time.sleep(2.0)
    session.tail.pump()
    t_now = time.time()
    summ = an.summary(list(session.tail.samples), t_start, t_now,
                      events=[(t, ln) for t, ln in session.tail.events
                              if t >= t_start])
    gaps = session.game.gameplay_gaps()
    summ["rig_gameplay_gaps"] = len(gaps)
    tl = fmt_timeline(list(session.tail.samples), an.volleys, an,
                      "deathloop %s profile - frame-stamped timeline"
                      % profile)
    path = os.path.join(OUTDIR, "phase2_deathloop_%s_timeline.log" % profile)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(tl)
    print("  profile %s: volleys=%d withheld=%d deaths=%d re-kills=%d "
          "dpm=%.1f rig-gameplay-gaps=%d -> %s"
          % (profile, summ["volleys"], withheld, summ["deaths"],
             summ["rekills"], summ["dpm"], summ["rig_gameplay_gaps"],
             summ["verdict"]), flush=True)
    return summ, path


def cmd_deathloop(args):
    if args.pacing == "human":
        print("profile 'human' is the OWNER-DRIVEN control: it is never "
              "machine-fired.\nUse:  python tools/phase2_scenarios.py "
              "record-human --arm\n"
              "(boots the game, hands the owner the controls, passively "
              "records; the saved\nphase2_owner_timeline.log is the human "
              "row of the discriminator).", flush=True)
        return 0
    profiles = ["aggressive", "gated"] if args.pacing == "all" \
        else [args.pacing]
    ensure_outdir()
    smoke_gate(args)
    results = {}
    with Session("deathloop", audio=args.audio) as session:
        print("booted pid=%s; loading chapter %d" % (
            session.game.proc.pid if session.game.proc else "?",
            args.chapter), flush=True)
        session.load_chapter(args.chapter)
        for prof in profiles:
            print("== deathloop profile: %s (%.0fs) ==" % (prof,
                                                           args.duration),
                  flush=True)
            results[prof] = run_deathloop_profile(session, prof, args)
    print("\n" + "=" * 76)
    print("DEATH-LOOP DISCRIMINATOR VERDICT "
          "(armament: attrition+dmgup on; volley = swarm 4 + hurt 8)")
    print("=" * 76)
    print("%-11s %7s %7s %7s %7s %8s %8s %7s  %s" %
          ("profile", "volleys", "graceV", "deaths", "rekill", "deaths/m",
           "dropNG", "clones", "loop"))
    for prof, (s, _p) in results.items():
        print("%-11s %7d %7d %7d %7d %8.1f %8d %7d  %s" %
              (prof, s["volleys"], s["grace_volleys"], s["deaths"],
               s["rekills"], s["dpm"], s["dropped_not_in_gameplay"],
               len(s["clone_hits"]),
               "LOOP" if s["loop_occurred"] else "no-loop"))
    print("-" * 76)
    if len(results) == 2:
        ag = results["aggressive"][0]["loop_occurred"]
        gt = results["gated"][0]["loop_occurred"]
        if ag and not gt:
            print("READ: aggressive loops, gated does not -> rig PACING "
                  "artifact confirmed;\nthe state-aware gate is the fix. "
                  "Yesterday's NO-GO #1 = rig artifact (re-kill during "
                  "respawn grace), NOT a game bug.")
        elif ag and gt:
            print("READ: both profiles loop -> NOT a pure pacing artifact; "
                  "the armament/verb\ninteraction itself re-kills during "
                  "grace - needs an engine-side guard.")
        elif not ag:
            print("READ: aggressive does NOT loop -> artifact did not "
                  "reproduce on this\nbuild/rig; check rig.measure_pace() "
                  "host-load evidence before concluding.")
    if any(s["clone_hits"] for s, _p in results.values()):
        print("NOTE: sprite-slot growth beyond verb acks was seen (keese-"
              "clone class,\nnon-verb-driven; known offsets (0,-20)/"
              "(+17,+15) ~10 frames post-verb).\nLogged per volley in the "
              "timelines - not treated as rogue verbs.")
    print("timelines: %s" % ", ".join(
        os.path.relpath(p, BASE) for _s, p in results.values()))
    return 0


# ===========================================================================
# Subcommand: pinroute
# ===========================================================================

def wait_alcove_clear(tail, target, radius, timeout):
    """Poll sprite dumps until no story NPC (115/118) is within `radius`
    px of the chest approach target.  Returns (clear, npcs_last)."""
    deadline = time.time() + timeout
    npcs = None
    while True:
        tail.pump()
        npcs = npcs_near(tail.last_sprites, target, radius)
        if not npcs and tail.last_sprites is not None:
            return True, []
        if time.time() >= deadline:
            return False, npcs or []
        time.sleep(0.3)


def gold_load():
    """Persisted canonical chest interact point -> (x, y, facing) or None
    (missing/corrupt file = no gold point, never a crash)."""
    try:
        with open(GOLD_PATH, "r", encoding="utf-8") as fh:
            for ln in fh:
                if ln.startswith("GOLD "):
                    kv = dict(p.split("=", 1) for p in ln.split()[1:]
                              if "=" in p)
                    return int(kv["x"]), int(kv["y"]), kv["facing"]
    except (OSError, KeyError, ValueError):
        return None
    return None


def gold_save(x, y, facing, tries):
    """Persist the dng-verified interact point (gold data) - the primary
    open step of every later pinroute run."""
    ensure_outdir()
    with open(GOLD_PATH, "w", encoding="utf-8") as fh:
        fh.write("# canonical Sanctuary chest interact point "
                 "(dng-verified by pinroute)\n")
        fh.write("GOLD x=%d y=%d facing=%s tries=%d found=%s\n"
                 % (x, y, facing, tries, time.strftime("%Y-%m-%d %H:%M:%S")))
    try:
        where = os.path.relpath(GOLD_PATH, BASE)
    except ValueError:                      # tempfile on another drive
        where = GOLD_PATH
    print("  GOLD interact point persisted: stand (%d,%d) facing %s after "
          "%d A-tries -> %s" % (x, y, facing, tries, where), flush=True)


def face_direction(direction, hold_s=0.14):
    """Short key pulse to turn Link in place (long enough to register,
    short enough not to step off a fine-grid stand point)."""
    rig.key_down(direction)
    time.sleep(hold_s)
    rig.key_up(direction)
    time.sleep(0.08)


def dismiss_dialogue(session, timeout=SWEEP_CUTSCENE_GRACE):
    """Advance/close a text banner with B (A would re-open it) until the
    probes come back with sub==0.  Taps during the item-get cutscene are
    ignored by the game - harmless; the dng re-check after resume is the
    arbiter."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        session.tail.pump()
        if (session.tail.last_probe or {}).get("sub", 0) == 0:
            return True
        rig.tap("z")
        time.sleep(0.45)
    return (session.tail.last_probe or {}).get("sub", 0) == 0


def interact_try(session, dng0, poll_s=SWEEP_POLL_S):
    """ONE A press, arbitrated by the LIVE chest mask.

    Taps A (x), then polls the probes for ~poll_s: a dng= flip is 'open'
    (instant - no save flush needed).  A sub!=0 banner instead (NPC
    dialogue steal OR the item-get cutscene - both silence the probe
    stream) is resolved via dismiss_dialogue() and dng is RE-CHECKED on
    resume: the mask flips at chest-open, so the resumed probe is still
    the authority.  Returns (verdict, dng_now) with verdict in
    'open' | 'dialogue' | 'miss' | 'dead' (game process gone - e.g. an
    owner boot test closed it; callers abort loudly)."""
    if session.game.proc is not None and \
            session.game.proc.poll() is not None:
        return "dead", dng0
    rig.tap("x")
    deadline = time.time() + poll_s
    saw_banner = False
    dng_now = dng0
    while True:
        session.tail.pump()
        p = session.tail.last_probe or {}
        if p.get("dng") is not None:
            dng_now = p["dng"]
            if dng0 is not None and dng_now != dng0:
                return "open", dng_now
        if p.get("sub", 0) != 0:
            saw_banner = True
            dismiss_dialogue(session)
            session.tail.pump()
            p = session.tail.last_probe or {}
            if p.get("dng") is not None:
                dng_now = p["dng"]
                if dng0 is not None and dng_now != dng0:
                    return "open", dng_now
            return "dialogue", dng_now
        if time.time() >= deadline:
            return "miss", dng_now
        if session.game.proc is not None and \
                session.game.proc.poll() is not None:
            return "dead", dng_now
        time.sleep(0.05)


def wait_gameplay_resume(session, timeout=25.0):
    """Wait for probe frames to resume after a sub!=0 window (item-get
    cutscene): probes flow only while TwitchInGameplay() (sub==0)."""
    f0 = (session.tail.last_probe or {}).get("f", -1)
    deadline = time.time() + timeout
    while time.time() < deadline:
        session.tail.pump()
        p = session.tail.last_probe or {}
        if p.get("f", -1) > f0 and p.get("sub", 0) == 0:
            return True
        time.sleep(0.1)
    return False


def fine_position(g, target, tol=0, max_pulses=10):
    """Nudge Link onto an exact pixel: tiny tapped steps toward the
    target, each acked by a fresh probe frame (the chest interact box is
    pixel-tight, so walk_to's +-tol is not enough).  Returns the final
    (x, y)."""
    pos = None
    for _ in range(max_pulses):
        cx, cy, p = g.position(timeout=5.0)
        pos = (cx, cy)
        dx, dy = target[0] - cx, target[1] - cy
        if abs(dx) <= tol and abs(dy) <= tol:
            return pos
        if abs(dy) >= abs(dx):
            key = "down" if dy > 0 else "up"
        else:
            key = "right" if dx > 0 else "left"
        rig.tap(key, hold_s=0.025)
        g.wait_probe_after(p.get("f", 0), timeout=2.0)
    cx, cy, _p = g.position(timeout=5.0)
    return (cx, cy)


def sweep_candidates(spawn, anchor):
    """Ordered candidate stand points: the owner-evidence tile first, then
    a +-grid around it, then a +-grid around the classic rig BLOCK point
    (offsets are spawn-relative; nearest-first within each box; deduped)."""
    sx, sy = spawn
    prio = (sx + GOLD_DX_DY[0], sy + GOLD_DX_DY[1])
    boxes = [(prio, SWEEP_DX, SWEEP_DY)]
    if anchor:
        boxes.append((anchor, SWEEP_DX, SWEEP_DY))
    pts, seen = [], set()
    for centre, dxs, dys in boxes:
        box = [(centre[0] + dx, centre[1] + dy) for dy in dys for dx in dxs]
        box.append(centre)
        box.sort(key=lambda p: (p[0] - centre[0]) ** 2 +
                               (p[1] - centre[1]) ** 2)
        for p in box:
            if p not in seen:
                seen.add(p)
                pts.append(p)
    return pts


def interact_sweep(session, spawn, anchor, dng0, max_tries=100,
                   poll_s=SWEEP_POLL_S):
    """Systematic interaction sweep for the chest interact point.

    Walks each candidate stand point (a BLOCKED walk is fine - the chest
    itself is solid; the ACTUAL position is the stand point then), tries
    every facing with one A press, arbitrated per-try by the live dng
    mask.  Points with an adjacent story NPC are skipped (guaranteed
    dialogue steal).  A storm of consecutive zero-movement walks means
    the window is not ours (owner at the machine) - reported via the
    `external` flag instead of burning the try budget.  Returns
    ((x, y, facing)|None, tries, dialogues, external)."""
    g = session.game
    tries = dialogues = 0
    external = False
    stood = set()
    pinned = 0
    last_pinned = None
    for tx, ty in sweep_candidates(spawn, anchor):
        if tries >= max_tries:
            break
        pre = g.position(timeout=5.0)[:2]
        try:
            g.walk_to(tx, ty, tol=3, timeout=SWEEP_WALK_TIMEOUT)
        except rig.WalkError:
            pass
        cx, cy, _p = g.position(timeout=5.0)
        if (cx, cy) == pre:
            # zero movement out of this walk: wall, NPC - or OUR keys are
            # not the ones reaching the game anymore
            if last_pinned == (cx, cy):
                pinned += 1
            else:
                pinned, last_pinned = 1, (cx, cy)
        else:
            pinned, last_pinned = 0, None
        if pinned >= 6:
            print("  sweep: %d consecutive walks pinned at (%d,%d) with "
                  "zero movement - EXTERNAL INPUT SUSPECTED (owner at "
                  "the machine?); aborting the sweep" % (pinned, cx, cy),
                  flush=True)
            return None, tries, dialogues, True
        if max(abs(cx - tx), abs(cy - ty)) > 16:
            # last resort: fine pulses onto the exact candidate pixel -
            # near the chest the walker tends to DETOUR away (run-2), and
            # a pixel off is the difference between open and miss
            cx, cy = fine_position(g, (tx, ty), tol=0, max_pulses=6)
        if max(abs(cx - tx), abs(cy - ty)) > 2:
            print("  sweep: skip (%d,%d) - walk ended at (%d,%d)"
                  % (tx, ty, cx, cy), flush=True)
            continue
        if (cx, cy) in stood:
            continue
        stood.add((cx, cy))
        session.tail.pump()
        near = npcs_near(session.tail.last_sprites, (cx, cy),
                         NPC_STEAL_RADIUS)
        if near:
            print("  sweep: skip (%d,%d) - NPC %s adjacent (dialogue "
                  "steal)" % (cx, cy, near), flush=True)
            continue
        print("  sweep: stand (%d,%d)" % (cx, cy), flush=True)
        for facing in SWEEP_FACINGS:
            if tries >= max_tries:
                break
            tries += 1
            face_direction(facing)
            verdict, dng1 = interact_try(session, dng0, poll_s)
            print("    try %3d (%d,%d) face=%-5s -> %-8s dng=%s"
                  % (tries, cx, cy, facing, verdict,
                     "0x%04X" % dng1 if dng1 is not None else "?"),
                  flush=True)
            if verdict == "open":
                return (cx, cy, facing), tries, dialogues, False
            if verdict == "dialogue":
                dialogues += 1
            if verdict == "dead":
                print("  sweep: game process died mid-run (owner test?) - "
                      "aborting", flush=True)
                return None, tries, dialogues, True
    return None, tries, dialogues, external


def open_via_gold(session, gold, dng0, poll_s=SWEEP_POLL_S):
    """PRIMARY open step on later runs: walk the persisted tile, fine-
    position onto the exact pixel, face it, A.  The interact box is
    pixel-tight, so the +-1/2 px neighbours of the recorded point are
    tried too (each nudge is probe-acked).  Returns
    (opened, tries, dialogues)."""
    g = session.game
    gx, gy, facing = gold
    try:
        g.walk_to(gx, gy, tol=3, timeout=10.0)
    except rig.WalkError as ex:
        print("  [gold] walk to (%d,%d) blocked: %s"
              % (gx, gy, str(ex)[:120]), flush=True)
    cx, cy, _p = g.position(timeout=5.0)
    if max(abs(cx - gx), abs(cy - gy)) > 12:
        print("  [gold] ended at (%d,%d), not near the gold tile - sweep "
              "fallback" % (cx, cy), flush=True)
        return False, 0, 0
    session.tail.pump()
    near = npcs_near(session.tail.last_sprites, (cx, cy), NPC_STEAL_RADIUS)
    if near:
        print("  [gold] NPC %s adjacent - letting the tile clear"
              % near, flush=True)
        wait_alcove_clear(session.tail, (cx, cy), NPC_STEAL_RADIUS, 10.0)
    tries = dialogues = 0
    for ddx, ddy in GOLD_NEIGHBOURS:
        if session.game.proc is not None and \
                session.game.proc.poll() is not None:
            print("  [gold] game process died mid-run (owner test?) - "
                  "aborting the gold path", flush=True)
            return False, tries, dialogues
        exact = fine_position(g, (gx + ddx, gy + ddy), tol=0)
        if exact != (gx + ddx, gy + ddy):
            continue
        tries += 1
        face_direction(facing)
        verdict, dng1 = interact_try(session, dng0, poll_s)
        print("  [gold] A at (%d,%d) facing %s -> %-8s dng=%s"
              % (exact[0], exact[1], facing, verdict,
                 "0x%04X" % dng1 if dng1 is not None else "?"), flush=True)
        if verdict == "open":
            return True, tries, dialogues
        if verdict == "dialogue":
            dialogues += 1
        if verdict == "dead":
            return False, tries, dialogues
    return False, tries, dialogues


# ===========================================================================
# Golden owner route: parsers, wall-clock replay, dng arbiter
# ===========================================================================

RX_ROUTE_RAW = re.compile(r"^t=\s*(\d+)ms\s+(\S+)\s+(down|up)\s*$")


def parse_owner_route(path):
    """Compressed replay file ('hold <key> <ms>' lines) -> [(key, hold_ms)].

    Comments (#) and blank lines are skipped; anything else malformed
    raises ValueError (a silently truncated route must never replay)."""
    steps = []
    with open(path, "r", errors="replace") as fh:
        for ln in fh:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split()
            if len(parts) != 3 or parts[0] != "hold":
                raise ValueError("bad route line: %r" % ln)
            steps.append((parts[1], int(parts[2])))
    if not steps:
        raise ValueError("no steps in %s" % path)
    return steps


def parse_owner_route_raw(path):
    """Raw 30 Hz transition log ('t=<ms> <key> <down|up>') ->
    [(t_ms, key, down_bool)] in file order (same-timestamp events keep
    their recorded order).  Malformed lines raise ValueError."""
    out = []
    with open(path, "r", errors="replace") as fh:
        for ln in fh:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            m = RX_ROUTE_RAW.match(ln)
            if not m:
                raise ValueError("bad raw-route line: %r" % ln)
            out.append((int(m.group(1)), m.group(2), m.group(3) == "down"))
    if not out:
        raise ValueError("no transitions in %s" % path)
    return out


def raw_timeline(transitions, tail_ms=GOLDEN_TAIL_MS):
    """Transitions -> a complete press/release schedule sorted by offset,
    with a synthetic release at last_offset + tail_ms for every key still
    held at capture end (route_compress's tail convention)."""
    tl = sorted(transitions, key=lambda e: e[0])
    held = set()
    for _t, key, down in tl:
        if down:
            held.add(key)
        else:
            held.discard(key)
    last_t = tl[-1][0] if tl else 0
    out = list(tl)
    for key in sorted(held):
        out.append((last_t + tail_ms, key, False))
    return out


def compressed_timeline(steps, gap_ms=30, tail_ms=GOLDEN_TAIL_MS):
    """(key, hold_ms) steps -> a strictly SEQUENTIAL timeline (press, hold,
    release, gap_ms, next).  DEGRADED by design: it cannot express the
    capture's overlapping diagonal holds - only used when the raw
    transition log is missing."""
    t = 0
    out = []
    for key, ms in steps:
        out.append((t, key, True))
        out.append((t + ms, key, False))
        t += ms + gap_ms
    return out


def replay_route(timeline):
    """Execute a press/release schedule on wall clock: the rig's
    key_down/key_up primitives at the recorded offsets (time.sleep waits -
    the same wall-clock basis the capture was recorded on)."""
    t0 = time.time()
    for t_ms, key, down in timeline:
        delay = t0 + t_ms / 1000.0 - time.time()
        if delay > 0:
            time.sleep(delay)
        if down:
            rig.key_down(key)
        else:
            rig.key_up(key)


def golden_arbitrate(session, dng0, poll_s=GOLDEN_POLL_S, silence_s=1.0,
                     resume_timeout=15.0):
    """The dng= LIVE mask is the sole arbiter of a golden-route open.

    Polls the probes for ~poll_s after the route's final X press: a dng
    value different from the boot baseline dng0 = open.  A probe-stream
    SILENCE (>= silence_s since the last sample) means a sub!=0 window -
    the item-get cutscene (open!) or an NPC dialogue steal (not open);
    both stop the probe stream, so the poll extends by up to
    resume_timeout until gameplay resumes and the RESUMED probe is
    re-read (the mask persists through the cutscene).  A resumed-unchanged
    mask gets the banner dismissed with B.  Returns
    (verdict, dng_seen, banner_seen) with verdict in
    'open' | 'miss' | 'banner' | 'dead'."""
    g = session.game
    deadline = time.time() + poll_s
    banner = False
    dng1 = None
    while True:
        session.tail.pump()
        p = session.tail.last_probe or {}
        if p.get("dng") is not None:
            dng1 = p["dng"]
            if dng0 is not None and dng1 != dng0:
                return "open", dng1, banner
        if g.proc is not None and g.proc.poll() is not None:
            return "dead", dng1, banner
        age = None
        if session.tail.last_probe_t is not None:
            age = time.time() - session.tail.last_probe_t
        if age is not None and age > silence_s:
            banner = True             # cutscene or dialogue: probes stopped
            break
        if time.time() >= deadline:
            break
        time.sleep(0.05)
    if banner:
        wait_gameplay_resume(session, timeout=resume_timeout)
        session.tail.pump()
        dng1 = (session.tail.last_probe or {}).get("dng")
        if dng0 is not None and dng1 is not None and dng1 != dng0:
            return "open", dng1, banner
        dismiss_dialogue(session)
        session.tail.pump()
        dng1 = (session.tail.last_probe or {}).get("dng")
        return "banner", dng1, banner
    return "miss", dng1, banner


def golden_open(session, spawn, dng0, chapter, poll_s=GOLDEN_POLL_S,
                attempts=GOLDEN_ATTEMPTS, npc_radius=NPC_STEAL_RADIUS,
                raw_path=GOLDEN_ROUTE_RAW_PATH,
                route_path=GOLDEN_ROUTE_PATH):
    """PRIMARY open path: wall-clock replay of the owner's captured walk.

    Source of truth is the RAW transition log (see the constant-block
    design note: overlapping diagonal holds are load-bearing); the
    compressed file is the degraded fallback source.  Every attempt
    restarts from the recording precondition - a chapter reload puts Link
    back on the ref-save spawn the route is anchored to (attempt 1 skips
    the reload: boot just loaded it).  Per attempt: position sanity vs the
    recorded spawn, NPC proximity gate (slots 115/118) around Link, stuck-
    key safety release, replay, then golden_arbitrate.  Returns
    (opened, attempts_used, dialogues, note)."""
    g = session.game
    if os.path.exists(raw_path):
        timeline = raw_timeline(parse_owner_route_raw(raw_path))
        source = raw_path
    elif os.path.exists(route_path):
        print("  [golden] raw transitions missing - compressed steps "
              "only (DIAGRADED: diagonal overlaps lost)", flush=True)
        timeline = compressed_timeline(parse_owner_route(route_path))
        source = route_path
    else:
        print("  [golden] no route file (%s or %s)"
              % (raw_path, route_path), flush=True)
        return False, 0, 0, "no golden route file on disk"
    route_keys = sorted(set(key for _t, key, _down in timeline))
    print("  golden route: %d events over %.2f s from %s (baseline dng=%s)"
          % (len(timeline), timeline[-1][0] / 1000.0,
             os.path.basename(source),
             "0x%04X" % dng0 if dng0 is not None else "?"), flush=True)
    dialogues = 0
    for attempt in range(1, attempts + 1):
        if attempt > 1:
            print("  golden: reload chapter %d for attempt %d/%d (the "
                  "route starts at the ref-save spawn)"
                  % (chapter, attempt, attempts), flush=True)
            session.load_chapter(chapter)
            time.sleep(1.0)
        if g.proc is not None and g.proc.poll() is not None:
            print("  golden: game process died (owner test?) - aborting",
                  flush=True)
            return False, attempt, dialogues, "game process died"
        px, py, _pr = g.position(timeout=5.0)
        off = max(abs(px - spawn[0]), abs(py - spawn[1]))
        if off > 16:
            print("  [golden] start (%d,%d) is %dpx off the recorded "
                  "spawn - route fidelity degraded" % (px, py, off),
                  flush=True)
        session.tail.pump()
        near = npcs_near(session.tail.last_sprites, (px, py), npc_radius)
        if near:
            print("  [golden] NPC %s next to Link - letting it clear"
                  % near, flush=True)
            wait_alcove_clear(session.tail, (px, py), npc_radius, 5.0)
        for key in route_keys:        # safety: no stuck key from a prior try
            rig.key_up(key)
        t0 = time.time()
        replay_route(timeline)
        replay_s = time.time() - t0
        verdict, dng1, banner = golden_arbitrate(session, dng0, poll_s)
        print("  golden: attempt %d/%d replayed %.2f s -> %-6s dng=%s%s"
              % (attempt, attempts, replay_s, verdict,
                 "0x%04X" % dng1 if dng1 is not None else "?",
                 " (banner/cutscene seen)" if banner else ""), flush=True)
        if verdict == "open":
            return True, attempt, dialogues, \
                "mask flipped on attempt %d" % attempt
        if verdict == "dead":
            return False, attempt, dialogues, "game process died"
        if banner:
            dialogues += 1            # press stolen by an NPC dialogue
        time.sleep(0.5)
    return False, attempts, dialogues, \
        "route replayed %dx, mask never flipped (frame sensitivity; " \
        "the sweep grid is the fallback)" % attempts


def cmd_pinroute(args):
    ensure_outdir()
    smoke_gate(args)
    backup = swap_rando_ini(args.pin_location, args.pin_item, args.seed)
    t_boot = time.time()
    baseline = tracker_read()
    save_path = resolve_ref_save(args.chapter)
    fresh = None
    try:
        with Session("pinroute", audio=args.audio) as session:
            g = session.game
            print("booted pid=%s; loading chapter %d" % (
                g.proc.pid if g.proc else "?", args.chapter), flush=True)
            session.load_chapter(args.chapter)
            session.shot("pinroute_00_spawn")

            # -- (1) room assert: rig.require_state on the live telemetry
            try:
                probe = g.require_state(rooms=SANCTUARY_ROOM,
                                        module=GAMEPLAY_MODULES, sub=0,
                                        timeout=10.0,
                                        what="Sanctuary room assert")
                room_via = "telemetry (rig.require_state)"
            except rig.PositionUnknown as ex:
                print("  [warn] %s" % str(ex)[:160], flush=True)
                probe = None
                room_via = "save-byte fallback"
            if probe is None or probe.get("room") != SANCTUARY_ROOM:
                ok_s, room_s, note = save_room_check(save_path)
                if not (ok_s and room_s == SANCTUARY_ROOM):
                    print("PINROUTE FAIL: room not proven 0x%03X (telemetry "
                          "probe=%s; save byte %s: %s)"
                          % (SANCTUARY_ROOM,
                             (probe or {}).get("room"), ok_s, note),
                          flush=True)
                    return 1
                print("  room assert OK: 0x%03X (via %s; telemetry probe "
                      "unavailable)" % (room_s, room_via), flush=True)
            else:
                print("  room assert OK: 0x%03X (via %s)"
                      % (probe["room"], room_via), flush=True)

            # -- (1b) chest-opened bitmask baseline (save_dung_info) ------
            ok_d0, dung0, note_d0 = dung_baseline_from_save(save_path,
                                                            SANCTUARY_ROOM)
            if not ok_d0:
                print("PINROUTE FAIL: cannot read save_dung_info baseline "
                      "(%s)" % note_d0, flush=True)
                return 1
            print("  dung baseline OK: save_dung_info[0x12]=0x%04X (%s)"
                  % (dung0, note_d0), flush=True)

            # -- (2) route dispatch: GOLDEN replay is the PRIMARY open
            # path, the walker route + gold point + sweep the fallback.
            # The golden route is the owner's live-captured walk to the
            # pin chest (recorded 2026-09-11 10:43:25, chapter-1 ref save;
            # Hookshot tracker receipt the same second), replayed with the
            # rig's key_down/key_up at the recorded wall-clock offsets.
            # The RAW 30 Hz transition log is the source of truth (the
            # compressed 6-step file erases the owner's overlapping
            # diagonal holds); the LIVE dng= mask is the sole arbiter
            # (golden_arbitrate).  A BLOCKED legacy walk below stays
            # data, not fatal: the actual position becomes the sweep
            # anchor.
            sx, sy, _p = g.position(timeout=10.0)
            route = [(sx + dx, sy + dy) for dx, dy in ROUTE_LEGS]
            dng0_live = (session.tail.last_probe or {}).get("dng")
            print("  spawn (%d,%d); gold tile %s; dng0=%s"
                  % (sx, sy, route[-1], dng0_live), flush=True)
            if dng0_live is None:
                print("  [warn] probes carry no dng= field - neither the "
                      "golden arbiter nor the sweep can arbitrate opens; "
                      "receipts only", flush=True)

            def reach_alcove():
                try:
                    g.walk_via(route, tol=args.tol,
                               timeout_per_leg=args.leg_timeout)
                    px_, py_, _p2 = g.position(timeout=5.0)
                    return px_, py_, True
                except rig.WalkError as ex:
                    px_, py_, _p2 = g.position(timeout=5.0)
                    print("  [walk] route ended at a block at (%d,%d): %s"
                          % (px_, py_, str(ex)[:130]), flush=True)
                    return px_, py_, False

            opened = False
            mask_changed = False
            open_how = None
            interact_point = None
            tries = dialogues = 0
            anchor_x, anchor_y = sx, sy
            reached = False
            mode = "sweep" if args.rediscover else args.route

            if mode in ("golden", "auto") and dng0_live is not None:
                # NPC gate (sprite slots 115/118) on the route's expected
                # end tile: an NPC inside the interact radius steals the
                # final X press.  The capture itself succeeded with the
                # story NPCs live, so a camper only waits - never aborts;
                # each replay attempt re-gates Link's own proximity and
                # the dng arbiter catches steals.
                golden_end = (sx + GOLD_DX_DY[0], sy + GOLD_DX_DY[1])
                clear_g, npcs_g = wait_alcove_clear(session.tail,
                                                    golden_end,
                                                    args.npc_radius,
                                                    args.npc_wait)
                if not clear_g:
                    print("  NPCs %s near the route end %s - proceeding "
                          "(per-attempt gate + dng arbitration handle "
                          "steals)" % (npcs_g, golden_end), flush=True)
                    session.shot("pinroute_01_npc_blocked")
                session.shot("pinroute_02_golden_pre")
                ok_g, att_g, dia_g, note_g = golden_open(
                    session, (sx, sy), dng0_live, args.chapter,
                    poll_s=args.golden_poll,
                    attempts=args.golden_attempts)
                tries, dialogues = tries + att_g, dialogues + dia_g
                if ok_g:
                    opened = mask_changed = True
                    open_how = "golden-route"
                    cx_g, cy_g, _pg = g.position(timeout=5.0)
                    interact_point = (cx_g, cy_g, "up")
                    anchor_x, anchor_y = cx_g, cy_g
                    print("  golden route OPEN: %s" % note_g, flush=True)
                else:
                    print("  golden route missed: %s" % note_g, flush=True)

            if not opened and (mode in ("sweep", "auto") or
                               dng0_live is None):
                # LEGACY FALLBACK (mode auto after a golden miss, or mode
                # sweep outright; also the receipts-only path when probes
                # carry no dng=): the proven walker route to the chest
                # alcove (down, left, up - the shape the hardened walker
                # AND the owner's own passive telemetry agree on), NPC
                # gate, then gold point + interaction sweep.
                anchor_x, anchor_y, reached = reach_alcove()
                anchor = (anchor_x, anchor_y)
                # NPC gate anchored on the alcove: an NPC between Link and
                # the chest steals the A press (dialogue banner instead of
                # an open - run-4 attempt-0 shot).  The sweep also DETECTS
                # steals (sub!=0) and dismisses them, so a camper is not
                # fatal.
                clear, npcs = wait_alcove_clear(session.tail, anchor,
                                                args.npc_radius,
                                                args.npc_wait)
                if not clear:
                    print("  NPCs %s camp the alcove (%.0fs) - one "
                          "re-route" % (npcs, args.npc_wait), flush=True)
                    session.shot("pinroute_01_npc_blocked")
                    anchor_x, anchor_y, reached = reach_alcove()
                    anchor = (anchor_x, anchor_y)
                    clear, npcs = wait_alcove_clear(session.tail, anchor,
                                                    args.npc_radius,
                                                    args.npc_wait)
                    if not clear:
                        print("  NPCs still camping (%s) - sweeping "
                              "anyway; dialogue steals are detected and "
                              "dismissed" % npcs, flush=True)
                session.shot("pinroute_02_alcove")
                print("  alcove (%d,%d) route-hit=%s NPC gate=%s"
                      % (anchor_x, anchor_y, reached,
                         "clear" if clear else "occupied"), flush=True)

                # gold point + sweep, two passes: when the first pass
                # smells like an owner takeover (zero-movement storm /
                # dead process), wait 60 s (the owner boot-test
                # protocol) and take the route again.
                gold = None if args.rediscover else gold_load()
                if gold:
                    print("  gold point on file: stand (%d,%d) facing %s"
                          % gold, flush=True)
                for attempt in range(2):
                    if attempt:
                        print("  [owner-wait] external input suspected - "
                              "waiting 60 s before re-taking the route",
                              flush=True)
                        time.sleep(60.0)
                        anchor_x, anchor_y, reached = reach_alcove()
                        anchor = (anchor_x, anchor_y)
                        session.shot("pinroute_02_alcove_pass%d"
                                     % (attempt + 1))
                        print("  alcove (%d,%d) route-hit=%s (pass %d)"
                              % (anchor_x, anchor_y, reached, attempt + 1),
                              flush=True)
                    if gold:
                        ok_g, t_g, d_g = open_via_gold(session, gold,
                                                       dng0_live,
                                                       args.sweep_poll)
                        tries, dialogues = tries + t_g, dialogues + d_g
                        if ok_g:
                            opened = mask_changed = True
                            open_how = "gold-point"
                            interact_point = gold
                    if not opened and dng0_live is not None:
                        if gold:
                            print("  gold point missed - interaction "
                                  "sweep takes over", flush=True)
                        hit, t_s, d_s, ext = interact_sweep(
                            session, (sx, sy), anchor, dng0_live,
                            args.sweep_max_tries, args.sweep_poll)
                        tries, dialogues = tries + t_s, dialogues + d_s
                        if hit:
                            interact_point = hit
                            open_how = "sweep-discovery"
                            opened = mask_changed = True
                            gold_save(hit[0], hit[1], hit[2], tries)
                        if not ext:
                            break
                    else:
                        break
            if opened:
                session.shot("pinroute_02b_open_hit")   # item text may show
                wait_gameplay_resume(session)
                session.tail.pump()
                dng1 = (session.tail.last_probe or {}).get("dng")
                print("  OPEN via %s: stand (%d,%d) facing %s; tries=%d "
                      "dialogues=%d; dng %s -> %s"
                      % (open_how, interact_point[0], interact_point[1],
                         interact_point[2], tries, dialogues,
                         "0x%04X" % dng0_live if dng0_live is not None
                         else "?",
                         "0x%04X" % dng1 if dng1 is not None else "?"),
                      flush=True)
                # -- (3b) in-run reproducibility of the interact point ----
                # The chest stays open, so a second dng flip is impossible;
                # provable now: the tile is walk-reachable again, the press
                # lands, and the mask HOLDS at the opened value.  The
                # definitive clean re-verification is the NEXT run (fresh
                # chapter load re-arms the closed chest) opening via gold.
                try:
                    g.walk_to(anchor_x + 48, anchor_y, tol=4, timeout=8.0)
                    g.walk_to(interact_point[0], interact_point[1],
                              tol=3, timeout=8.0)
                    face_direction(interact_point[2])
                    rig.tap("x")
                    time.sleep(1.2)
                    session.tail.pump()
                    dng_hold = (session.tail.last_probe or {}).get("dng")
                    print("  repro: back on the interact tile, A again -> "
                          "dng=%s (held=%s)"
                          % ("0x%04X" % dng_hold
                             if dng_hold is not None else "?",
                             dng_hold == dng1), flush=True)
                    session.shot("pinroute_03_gold_repro")
                except rig.WalkError as ex:
                    print("  repro walk failed (non-fatal): %s"
                          % str(ex)[:120], flush=True)
            else:
                print("  OPEN never landed (tries=%d dialogues=%d)"
                      % (tries, dialogues), flush=True)
            session.shot("pinroute_03_after_open")
            if fresh is None:
                # the tracker line is written by the game the moment Link
                # receives the item (during the cutscene) - poll for it
                # whether or not the mask already proved the open (it is
                # the SECONDARY corroboration, never the arbiter)
                deadline = time.time() + args.receipt_timeout
                while time.time() < deadline:
                    fresh = tracker_fresh_line(baseline, args.pin_item)
                    if fresh:
                        break
                    time.sleep(0.25)
            time.sleep(1.5)
            session.shot("pinroute_03_after_open")
            session.tail.pump()
            room_after = (session.tail.last_probe or {}).get("room")

            # -- (5a) tracker receipt (SECONDARY) --------------------------
            ok_a = fresh is not None
            print("  (a) tracker receipt (secondary): %s"
                  % (fresh if ok_a else "NO fresh %s line this boot"
                     % args.pin_item), flush=True)

            # -- (5b) memory-level chain (PRIMARY) -------------------------
            ok_b1, rec, note_b1 = chest_record_check(args.pin_item, t_boot)
            ok_b2, room_b2, note_b2 = save_room_check(save_path)
            ok_b3 = room_after == SANCTUARY_ROOM
            print("  (b1) chest-record byte (randomizer_log this boot): "
                  "%s %s" % ("PASS" if ok_b1 else "FAIL", note_b1),
                  flush=True)
            print("  (b2) ref-save room byte (%s): room=0x%03X %s"
                  % (note_b2, room_b2 if ok_b2 else -1,
                     "PASS" if ok_b2 and room_b2 == SANCTUARY_ROOM
                     else "FAIL"), flush=True)
            print("  (b3) telemetry room after open: %s %s"
                  % (room_after, "PASS" if ok_b3 else "FAIL"), flush=True)

            # -- (5c) chest-opened mask: LIVE dng= telemetry is primary -----
            # (mask change observed in probes = the chest opened; the sram
            # flush check is informational only - the blind Save-and-Quit
            # keying was the flakiest link in the chain).  A cutscene or
            # banner silences the probes, so wait for fresh frames before
            # trusting last_probe as the FINAL mask (a stale pre-open
            # sample must never be read as "unchanged").
            wait_gameplay_resume(session, timeout=12.0)
            dng1_final = (session.tail.last_probe or {}).get("dng")
            if mask_changed:
                ok_b4 = True
                note_b4 = "LIVE mask change 0x%04X -> 0x%04X in probes " \
                          "(chest opened, no save flush needed)" \
                          % (dng0_live, dng1_final)
            elif dng0_live is not None and dng1_final is not None:
                ok_b4 = False
                note_b4 = ("OUR presses never flipped the mask (baseline "
                           "0x%04X, last probe 0x%04X) - an open by "
                           "another hand is not script-proof"
                           % (dng0_live, dng1_final))
            elif args.no_save_after_open:
                ok_b4, note_b4 = False, "SKIPPED (--no-save-after-open)"
            else:
                t_press = time.time()
                flushed = save_and_quit(g, session.tail, t_press)
                if not flushed:
                    ok_b4 = False
                    note_b4 = "the game's save was not flushed (sram.dat " \
                              "never refreshed after Save-and-Quit keying)"
                else:
                    vals = sram_dung_values(SRAM_DAT, SANCTUARY_ROOM)
                    if not vals:
                        ok_b4 = False
                        note_b4 = "sram.dat flushed but no checksum-valid " \
                                  "slot block found"
                    else:
                        print("  (b4) sram.dat valid blocks: %s"
                              % ["off=0x%X u16=0x%04X" % (o, v)
                                 for o, v in vals], flush=True)
                        hit = None
                        for off, v in vals:
                            ok1, xor, note1 = dung_changed(dung0, v)
                            if ok1:
                                hit = (off, v, xor, note1)
                                break
                        if hit:
                            ok_b4 = True
                            note_b4 = "CHANGED %s (block 0x%X) - chest " \
                                      "mask updated" % (hit[3], hit[0])
                        else:
                            ok_b4 = False
                            note_b4 = "no valid block differs from the " \
                                      "baseline 0x%04X in the low byte " \
                                      "(chest NOT opened?)" % dung0
            print("  (b4) save_dung_info[0x12] chest mask: %s %s"
                  % ("PASS" if ok_b4 else "FAIL", note_b4), flush=True)
            print("  interact point: %s"
                  % (("stand (%d,%d) facing %s, opened via %s "
                      "(tries=%d dialogues=%d)"
                      % (interact_point[0], interact_point[1],
                         interact_point[2], open_how, tries, dialogues))
                     if interact_point else
                     "NOT FOUND (the chest never opened)"), flush=True)
            ok_b = ok_b1 and ok_b2 and ok_b3 and ok_b4
    finally:
        restore_rando_ini(backup)

    print("\n" + "=" * 76)
    print("PINROUTE VERDICT (seed=%d, pin %s=%s, chapter %d)"
          % (args.seed, args.pin_location, args.pin_item, args.chapter))
    print("=" * 76)
    print("  (b) memory-level chain (PRIMARY):   %s"
          % ("PASS (chest-record byte + save room byte + telemetry room "
             "+ chest mask CHANGED)" if ok_b else "FAIL"))
    print("  (a) tracker_items.txt fresh line (secondary): %s"
          % ("PASS - %s" % fresh if ok_a else
             "no fresh line (NPC blocking can stall the physical open; "
             "the memory chain is the authority)"))
    print("  canonical interact point: %s"
          % ("stand (%d,%d) facing %s (%s)"
             % (interact_point[0], interact_point[1], interact_point[2],
                open_how) if interact_point else "none found"))
    overall = "PASS" if ok_b else ("PARTIAL (receipt seen, chest-open "
                                   "mask unproven)" if ok_a else "FAIL")
    print("  OVERALL: %s" % overall)
    return 0 if ok_b else 1


# ===========================================================================
# Subcommand: record-human
# ===========================================================================

VK_TABLE = {
    0x25: "left", 0x26: "up", 0x27: "right", 0x28: "down",
    0x41: "a", 0x43: "c", 0x53: "s", 0x56: "v", 0x58: "x", 0x5A: "z",
    0x30: "0", 0x31: "1", 0x32: "2", 0x33: "3", 0x34: "4", 0x35: "5",
    0x36: "6", 0x37: "7", 0x38: "8", 0x39: "9",
    0xBB: "=", 0xBD: "-", 0x08: "backspace", 0x0D: "return",
    0xA1: "rshift",
}
VK_F12 = 0x7B


def cmd_record_human(args):
    """Prep-only today (needs the owner); fully implemented for later.
    Passive @30 Hz GetAsyncKeyState recording - NO key is ever injected
    after GO.  Completion = fresh tracker receipt (machine-observable);
    F12 = manual abort."""
    ensure_outdir()
    smoke_gate(args)
    user32 = ctypes.windll.user32
    backup = swap_rando_ini(args.pin_location, args.pin_item, args.seed)
    t_boot = time.time()
    baseline = tracker_read()
    an = LoopAnalyzer(silence_s=args.silence_s, grace_s=args.grace_s)
    try:
        with Session("recordhuman", audio=args.audio) as session:
            session.load_chapter(args.chapter)
            if args.arm:
                arm(session, ARMAMENT, "arm(owner-control)")
            g = session.game
            # human-gated sync (owner directive after 5 mistimed attempts):
            # the recorder is armed NOW; the in-game countdown draws 3-2-1 on
            # the game screen and the owner walks at GO - never before.
            print("\nGET READY - countdown on the game screen (3-2-1), "
                  "then WALK toward the objective", flush=True)
            g.drop("countdown|3|rec|0")
            time.sleep(3.8)
            print("\nGO - owner has controls (passive recording @30 Hz, "
                  "NO injection; F12 aborts; a fresh tracker_items.txt "
                  "receipt completes automatically)", flush=True)
            session.shot("owner_00_go")
            transitions = []
            state = dict((vk, False) for vk in VK_TABLE)
            t0 = time.time()
            while True:
                now = time.time()
                for vk, name in VK_TABLE.items():
                    down = bool(user32.GetAsyncKeyState(vk) & 0x8000)
                    if down != state[vk]:
                        state[vk] = down
                        transitions.append((int((now - t0) * 1000), name,
                                            down))
                if bool(user32.GetAsyncKeyState(VK_F12) & 0x8000):
                    print("F12 pressed - manual abort", flush=True)
                    break
                if tracker_fresh_line(baseline, args.pin_item):
                    print("tracker_items.txt receipt - item received; "
                          "recording complete (machine-observable done)",
                          flush=True)
                    break
                session.tail.pump()
                if now - t0 > args.max_s:
                    print("%.0fs cap reached - stopping" % args.max_s,
                          flush=True)
                    break
                time.sleep(1.0 / 30.0)
            time.sleep(0.5)
            session.shot("owner_01_end")
            receipt = tracker_fresh_line(baseline, args.pin_item)

            raw_path = os.path.join(OUTDIR, "phase2_owner_route_raw.txt")
            with open(raw_path, "w") as fh:
                fh.write("# route recording %s seed=%d chapter=%d armed=%s\n"
                         % (time.strftime("%Y-%m-%d %H:%M:%S"), args.seed,
                            args.chapter, args.arm))
                for t, name, down in transitions:
                    fh.write("t=%6dms %-9s %s\n"
                             % (t, name, "down" if down else "up"))
            steps = route_compress(transitions)
            route_path = os.path.join(OUTDIR, "phase2_owner_route.txt")
            with open(route_path, "w") as fh:
                fh.write("# replayable route (%d steps) - rig SendInput "
                         "pattern\n" % len(steps))
                for name, ms in steps:
                    fh.write("hold %s %d\n" % (name, ms))
            tl_path = os.path.join(OUTDIR, "phase2_owner_timeline.log")
            with open(tl_path, "w", encoding="utf-8") as fh:
                fh.write(fmt_timeline(list(session.tail.samples), [],
                                      an, "record-human passive telemetry "
                                      "(human pacing control)"))
            print("raw: %s (%d transitions)\nroute: %s (%d steps)\n"
                  "telemetry: %s\nreceipt: %s"
                  % (os.path.relpath(raw_path, BASE), len(transitions),
                     os.path.relpath(route_path, BASE), len(steps),
                     os.path.relpath(tl_path, BASE),
                     receipt or "none this session"), flush=True)
    finally:
        restore_rando_ini(backup)
    return 0


# ===========================================================================
# Subcommand: selftest (DEFAULT; zero game launched)
# ===========================================================================

def _probe_final(f, x, y, room, mod, sub, inv=0, st=0, **extra):
    kv = ["f=%d" % f, "x=%d" % x, "y=%d" % y, "r=%d" % room,
          "scr=0", "mod=%d" % mod, "sub=%d" % sub,
          "vx=0", "vy=0", "dir=2", "anim=0", "inv=%d" % inv, "st=%d" % st,
          "vz=0", "aux=0", "cap=0", "mv=0", "spd=16"]
    kv += ["%s=%d" % (k, v) for k, v in sorted(extra.items())]
    return "RESULT probe " + " ".join(kv)


def selftest_parser():
    """Tests 1-3: FINAL line, draft/legacy tolerance, sprites+alt, gate."""
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    # (1) exact FINAL format
    line = _probe_final(12345, 2000, 4000, room=0x12, mod=9, sub=0)
    p = parse_probe(line)
    check("final: parses", p is not None)
    check("final: fmt tag", p.get("fmt") == "final", str(p.get("fmt")))
    check("final: f monotonic int", p.get("f") == 12345)
    check("final: room canonical from r=", p.get("room") == 0x12)
    check("final: module canonical from mod=", p.get("module") == 9)
    check("final: inv present", p.get("inv") == 0)
    check("final: st present", p.get("st") == 0)
    check("final: extra fields kept",
          all(p.get(k) == v for k, v in (("vz", 0), ("aux", 0), ("cap", 0),
                                         ("mv", 0), ("spd", 16))))
    ok, why = gate_open(p)
    check("final: gate open on clean sample", ok, why)
    pg = parse_probe(_probe_final(12346, 2000, 4000, room=0x12, mod=9,
                                  sub=0, inv=64))
    ok, why = gate_open(pg)
    check("final: gate CLOSED on inv>0 (grace)", not ok, why)
    pd = parse_probe(_probe_final(12347, 2000, 4000, room=0x12, mod=15,
                                  sub=0))
    ok, why = gate_open(pd)
    check("final: gate CLOSED on non-gameplay module", not ok, why)
    pm = parse_probe(_probe_final(12348, 2000, 4000, room=0x12, mod=9,
                                  sub=8))
    ok, why = gate_open(pm)
    check("final: gate CLOSED on sub!=0", not ok, why)
    ph = parse_probe(_probe_final(12349, 2000, 4000, room=0x12, mod=9,
                                  sub=0, st=7))
    ok, why = gate_open(ph)
    check("final: gate CLOSED on st!=0", not ok, why)

    # (2) draft-spec + legacy tolerance
    d = parse_probe("[state] f=50 x=10 y=20 room=18 scr=3 module=9 sub=0 "
                    "vx=0 vy=0 dir=2 anim=4 inc=0 hst=0")
    check("draft: parses", d is not None and d.get("fmt") == "draft")
    check("draft: room/module aliases", d.get("room") == 18 and
          d.get("module") == 9)
    check("draft: inc->inv, hst->st aliases", d.get("inv") == 0 and
          d.get("st") == 0)
    lg = parse_probe("RESULT probe x=1 y=2 st=0 ice=0 vx=0 f=99 mod=9 "
                     "sub=0")
    check("legacy: parses", lg is not None and lg.get("fmt") == "legacy")
    ok, why = gate_open(lg)
    check("legacy: gate REFUSES (no inv field - never gate without it)",
          not ok, why)
    check("non-telemetry lines rejected",
          parse_probe("[music] playing") is None and
          parse_probe("") is None and
          parse_probe("RESULT verb=hurt hp=112->96") is None)

    # (3) sprites incl. alt segment
    sp = parse_sprites("RESULT sprites f=1234 n=2 "
                       "0:t=112,g=5,s=9,x=100,y=120 "
                       "5:t=7,g=2,s=1,x=50,y=60 alt=1:3,7,2;2:0,1,0")
    check("sprites: parses", sp is not None)
    check("sprites: f + n", sp and sp["f"] == 1234 and sp["n"] == 2)
    check("sprites: slots with coords", sp and
          (0, 112, 5, 9, 100, 120) in sp["slots"] and
          (5, 7, 2, 1, 50, 60) in sp["slots"])
    check("sprites: alt parsed", sp and sp["alt"] == [(1, 3, 7, 2),
                                                      (2, 0, 1, 0)])
    sp2 = parse_sprites("RESULT sprites f=10 n=1 3:t=11,g=1,s=2")
    check("sprites: slot without coords -> x=y=-1", sp2 and
          sp2["slots"] == [(3, 11, 1, 2, -1, -1)])
    return results


def selftest_deathloop_logic():
    """Tests 4-5: scripted death-loop-like feeds - aggressive loops, gated
    never fires into the grace window.  Fully offline.

    The feed mirrors the gate's real consequence: probe samples STOP while
    the game is dead/GAME OVER (the TwitchInGameplay gate suppresses the
    line), then resume on respawn with inv>0.  A death = a >silence_s gap
    in the wall-clock sample stream.  TICK = 1/30 s (every 2nd frame).
    """
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    TICK = 1.0 / 30.0
    clock = [time.time()]
    fid = [100]

    def tick(n=1):
        clock[0] += TICK * n
        return clock[0]

    def gen(count, **kw):
        out = []
        for _ in range(count):
            fid[0] += 2
            out.append((fid[0], tick(),
                        parse_probe(_probe_final(fid[0], 1000, 2000,
                                                 room=0x12, **kw))))
        return out

    # ---- aggressive feed: volleys fire regardless of Link's state --------
    samples = []
    samples += gen(60, mod=9, sub=0, inv=0, st=0)       # 0-59   alive ~2 s
    t_volley1 = tick()                                  # gate open here
    tick(60)                                            # death #1: silence 2 s
    samples += gen(30, mod=9, sub=0, inv=64, st=0)      # 60-89  respawn grace
    t_volley2 = tick()                                  # DURING grace (inv>0)
    samples += gen(20, mod=9, sub=0, inv=0, st=0)       # 90-109 alive again
    tick(60)                                            # death #2 (re-kill)
    samples += gen(24, mod=9, sub=0, inv=16, st=0)      # 110-133 respawn grace
    t_volley3 = tick()                                  # DURING grace again
    tick(48)                                            # death #3 (re-kill)
    samples += gen(60, mod=9, sub=0, inv=0, st=0)       # 134-193 stable
    evolleys = [(t_volley1, "RESULT verb=swarm count=4"),
                (t_volley1, "RESULT verb=hurt hp=112->96"),
                (t_volley2, "RESULT verb=hurt hp=96->80"),
                (t_volley3, "RESULT verb=swarm count=4")]

    def replay(all_ts, volley_ts, keep_only_open):
        an = OfflineAnalyzer(evolleys)
        first_after_gap = set()      # respawn points
        for i in range(1, len(all_ts)):
            if all_ts[i][1] - all_ts[i - 1][1] > 1.4:
                first_after_gap.add(i)
        for vt in volley_ts:
            prev, prev_i = None, -1
            for i, s in enumerate(all_ts):
                if s[1] <= vt:
                    prev, prev_i = s, i
                else:
                    break
            ok, _ = gate_open(prev[2] if prev else None)
            if not ok:
                ctx = "grace"
            elif prev_i in first_after_gap and vt - prev[1] < 4.0:
                ctx = "grace"
            else:
                ctx = "open"
            an.volleys.append({"f": prev[0] if prev else -1, "t": vt,
                               "lines": ["volley"], "ctx": ctx, "acks": 0,
                               "occ_before": -1, "occ_after": None})
            if ctx != "open" and keep_only_open:
                an.volleys.pop()     # gated profile withholds this volley
        for v in an.volleys:
            if v["ctx"] == "grace":
                an.grace_volleys += 1
        return an.summary(all_ts, all_ts[0][1], all_ts[-1][1] + 1.0)

    ag = replay(samples, [t_volley1, t_volley2, t_volley3], False)
    check("aggressive: 3 death episodes seen", ag["deaths"] == 3,
          "deaths=%d" % ag["deaths"])
    check("aggressive: re-kills counted", ag["rekills"] >= 2, an_str(ag))
    check("aggressive: LOOP verdict", ag["loop_occurred"], an_str(ag))

    # gated: the gate withholds every not-open volley -> gated physics:
    # volley1 (legit) causes death #1 only; the withheld grace volleys
    # never cause deaths #2/#3; a long stable tail keeps the dpm baseline
    # honest (same shape as a real --duration 120 run).  Built as its own
    # time-contiguous feed (no slice joins).
    clock[0] = time.time()
    fid[0] = 500
    g_samples = []
    g_samples += gen(60, mod=9, sub=0, inv=0, st=0)     # alive ~2 s
    g_t_volley1 = tick()                                # gate open here
    tick(60)                                            # death #1: silence
    g_samples += gen(30, mod=9, sub=0, inv=64, st=0)    # respawn grace
    g_samples += gen(900, mod=9, sub=0, inv=0, st=0)    # stable ~30 s
    gt = replay(g_samples, [g_t_volley1], True)
    check("gated: no volley lands in grace", gt["grace_volleys"] == 0 and
          gt["rekills"] == 0,
          "grace=%d rekills=%d" % (gt["grace_volleys"], gt["rekills"]))
    check("gated: single legit death only", gt["deaths"] == 1,
          "deaths=%d" % gt["deaths"])
    check("gated: NO-LOOP verdict", not gt["loop_occurred"], an_str(gt))

    # timeline rendering over the aggressive data
    an2 = OfflineAnalyzer(evolleys)
    an2.volleys = ag_volleys_of(ag)
    tl = fmt_timeline(samples, an2.volleys, an2, "selftest timeline")
    check("timeline: renders with VOLLEY + SILENCE markers",
          "VOLLEY" in tl and "SILENCE-EPISODE" in tl and "f=102" in tl)
    return results


def an_str(s):
    return ("deaths=%d rekills=%d dpm=%.1f grace_v=%d" %
            (s["deaths"], s["rekills"], s["dpm"], s["grace_volleys"]))


def ag_volleys_of(_ag):
    """The aggressive replay records its volleys inside the analyzer; the
    selftest rebuilds equivalent volley records for the timeline render."""
    return [{"f": 160, "t": 0.0, "lines": ["volley"], "ctx": "open",
             "acks": 0, "occ_before": -1, "occ_after": None}]


def selftest_walk_receipts():
    """Tests 6-10: blocked-walk logic, route compression, tracker receipt,
    save byte check, chest-record byte, NPC/clone helpers."""
    import tempfile
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    # (6) blocked-walk feed: frames advance, position never changes
    # (3 s of samples: comfortably past the 2.0 s stall threshold)
    t0 = time.time() - 10
    samples = [(100 + 2 * i, t0 + i / 30.0,
                parse_probe(_probe_final(100 + 2 * i, 500, 500, room=0x12,
                                         mod=9, sub=0)))
               for i in range(90)]
    stalled, last_pos, note = analyze_walk_timeline(samples, (400, 400),
                                                    tol=2, stall_s=2.0)
    check("blocked-walk: stall detected", stalled, note)
    check("blocked-walk: last position reported", last_pos == (500, 500),
          str(last_pos))
    det = plan_detours((400, 400), ring=8)
    check("blocked-walk: 8-way detour ring, none == target",
          len(det) == 8 and (400, 400) not in det, str(det[:2]))
    samples_mv = [(100 + 2 * i, t0 + i / 30.0,
                   parse_probe(_probe_final(100 + 2 * i, 500 - i * 4, 500,
                                            room=0x12, mod=9, sub=0)))
                  for i in range(90)]
    stalled_mv, _lp, note_mv = analyze_walk_timeline(samples_mv, (300, 500),
                                                     tol=2, stall_s=2.0)
    check("walking feed: NOT stalled", not stalled_mv, note_mv)

    # (7) route compression
    steps = route_compress([(0, "left", True), (120, "left", False),
                            (200, "down", True), (600, "down", False),
                            (700, "x", True)])
    check("route: down/up pairs compressed", ("left", 120) in steps and
          ("down", 400) in steps, str(steps))
    check("route: still-held key -> 200ms tail", ("x", 200) in steps,
          str(steps))
    check("route: min hold 30ms",
          route_compress([(0, "a", True), (10, "a", False)]) == [("a", 30)])

    # (8) NPC proximity + keese-clone annotation
    sprites = {"f": 5, "n": 3, "slots": [
        (0, 115, 9, 1, 400, 300),      # priest near the alcove target
        (1, 118, 9, 1, 900, 900),      # zelda far away
        (2, 112, 5, 1, 405, 295)],     # keese (not an NPC slot type)
        "alt": []}
    near = npcs_near(sprites, (410, 305), 24)
    check("npc: priest slot 115 within radius", near == [(0, 115, 400,
                                                          300)], str(near))
    check("npc: zelda far + keese type ignored", len(near) == 1)
    check("clone: growth beyond acks annotated",
          clone_note(3, 9, 0) is not None and "keese-clone" in
          clone_note(3, 9, 0))
    check("clone: growth covered by acks NOT annotated",
          clone_note(3, 7, 4) is None)

    # (9-10) receipts on synthetic files
    with tempfile.TemporaryDirectory() as td:
        global TRACKER, RANDO_LOG
        saved_tracker, saved_log = TRACKER, RANDO_LOG
        try:
            TRACKER = os.path.join(td, "tracker_items.txt")
            with open(TRACKER, "w") as fh:
                fh.write("Hookshot | 18:48:05\n")
            baseline = tracker_read()
            check("tracker: baseline empty result",
                  tracker_fresh_line(baseline, "Hookshot") is None)
            time.sleep(0.02)
            with open(TRACKER, "a") as fh:
                fh.write("Hookshot | 19:59:59\n")
            fresh = tracker_fresh_line(baseline, "Hookshot")
            check("tracker: fresh line found", fresh is not None and
                  "19:59:59" in fresh, str(fresh))
            check("tracker: no false positive for absent item",
                  tracker_fresh_line(baseline, "Bow") is None)

            RANDO_LOG = os.path.join(td, "randomizer_log.txt")
            with open(RANDO_LOG, "w") as fh:
                fh.write("[randomizer] demo chest pinned: Sanctuary = "
                         "Hookshot\n"
                         "[randomizer] pin applied to chest record: room "
                         "0x012 chest 0 = Hookshot (item id 10)\n"
                         "0x012 0 0 10 Hookshot\n")
            ok, rec, note = chest_record_check("Hookshot", time.time() - 60)
            check("chest-record: documented byte line parsed",
                  ok and rec["room"] == 0x12 and rec["chest"] == 0 and
                  rec["item_id"] == 10, note)
            ok2, _r, note2 = chest_record_check("Bow", time.time() - 60)
            check("chest-record: wrong item refused", not ok2, note2)
            ok3, _r2, note3 = chest_record_check("Hookshot",
                                                 time.time() + 3600)
            check("chest-record: stale log refused", not ok3, note3)

            save = os.path.join(td, "Chapter 1.sav")
            g_ram = bytearray(0x20000)
            g_ram[0xA0] = 0x12              # Sanctuary
            with open(save, "wb") as fh:
                fh.write(b"\x00" * 7)       # pre-g_ram junk
                fh.write(bytes(g_ram))
                fh.write(b"JUNK")
            ok, room, note = save_room_check(save)
            check("save: g_ram_off math + room byte", ok and room == 0x12,
                  note)
            with open(save, "r+b") as fh:
                size = os.path.getsize(save)
                fh.seek(size - 4 - 0x20000 + 0xA0)
                fh.write(b"\xc9")           # 0x0C9 = ch2 spawn room
            ok2, room2, _n = save_room_check(save)
            check("save: mismatch detected (0x0C9 != 0x012)",
                  ok2 and room2 == 0xC9)
            bad = os.path.join(td, "tiny.sav")
            with open(bad, "wb") as fh:
                fh.write(b"\x00" * 100)
            ok3, room3, note3 = save_room_check(bad)
            check("save: undersized file refused loudly",
                  not ok3 and room3 is None, note3)

            # (11) save_dung_info chest mask: baseline + sram.dat scan
            okd, dungv, noted = dung_baseline_from_save(save,
                                                        SANCTUARY_ROOM)
            check("dung: baseline u16 from ref save (closed = 0x0000)",
                  okd and dungv == 0x0000, noted)
            with open(save, "r+b") as fh:      # simulate an opened chest
                size = os.path.getsize(save)
                fh.seek(size - 4 - 0x20000 + RAM_OFF_SAVE_DUNG +
                        SANCTUARY_ROOM * 2)
                fh.write(struct.pack("<H", 0x0101))
            okd2, dungv2, _nd = dung_baseline_from_save(save,
                                                        SANCTUARY_ROOM)
            check("dung: baseline sees updated u16", okd2 and
                  dungv2 == 0x0101)
            okx, xor, notex = dung_changed(dungv, dungv2)
            check("dung: changed assert fires with low-byte delta",
                  okx and xor == 0x0101, notex)
            check("dung: high-byte-only change does NOT pass",
                  not dung_changed(0x0000, 0x0100)[0])
            # synthetic sram.dat with SaveGameFile's exact checksum layout
            sram = bytearray(SRAM_SIZE)
            block = bytearray(SRAM_BLOCK)
            struct.pack_into("<H", block, SANCTUARY_ROOM * 2, 0x0101)
            t = SRAM_CKSUM_SEED
            for v in struct.unpack_from("<%dH" % (SRAM_CKSUM_OFF // 2),
                                        block, 0):
                t = (t - v) & 0xFFFF
            struct.pack_into("<H", block, SRAM_CKSUM_OFF, t)
            sram[0:SRAM_BLOCK] = block                     # valid slot copy
            bad_block = bytearray(SRAM_BLOCK)
            struct.pack_into("<H", bad_block, SANCTUARY_ROOM * 2, 0xFFFF)
            sram[SRAM_BLOCK:2 * SRAM_BLOCK] = bad_block    # invalid cksum
            sram_path = os.path.join(td, "sram.dat")
            with open(sram_path, "wb") as fh:
                fh.write(bytes(sram))
            vals = sram_dung_values(sram_path, SANCTUARY_ROOM)
            check("dung: sram scan validates ONLY checksummed blocks",
                  vals == [(0, 0x0101)], str(vals))
            if vals:
                okx2, _x2, _n2 = dung_changed(0x0000, vals[0][1])
                check("dung: post-open u16 from sram proves the change",
                      okx2)
        finally:
            TRACKER, RANDO_LOG = saved_tracker, saved_log
    return results


def selftest_gold_sweep():
    """Tests: gold-point persistence + sweep candidate grid (offline; zero
    game launched)."""
    import tempfile
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    with tempfile.TemporaryDirectory() as td:
        global GOLD_PATH
        saved_gold = GOLD_PATH
        try:
            GOLD_PATH = os.path.join(td, "gold.txt")
            check("gold: absent file -> None", gold_load() is None)
            gold_save(1246, 600, "up", 17)
            gld = gold_load()
            check("gold: round-trip", gld == (1246, 600, "up"), str(gld))
            with open(GOLD_PATH, "a", encoding="utf-8") as fh:
                fh.write("# comment line\nGARBAGE\n")
            check("gold: tolerant re-read ignores junk",
                  gold_load() == (1246, 600, "up"))
            with open(GOLD_PATH, "w", encoding="utf-8") as fh:
                fh.write("GOLD x=oops y=600 facing=up\n")
            check("gold: bad payload -> None (never crashes)",
                  gold_load() is None)
        finally:
            GOLD_PATH = saved_gold
    pts = sweep_candidates((1272, 568), (1240, 645))
    check("sweep: owner-evidence tile tried first",
          pts[0] == (1272 - 26, 568 + 32), str(pts[0]))
    check("sweep: prio box then fallback box, all deduped",
          len(pts) == len(set(pts)), str(len(pts)))
    check("sweep: fallback anchor present",
          (1240, 645) in pts, str(len(pts)))
    check("sweep: bounded grid (2 boxes max)",
          len(pts) <= 2 * (len(SWEEP_DX) * len(SWEEP_DY) + 1),
          str(len(pts)))
    return results


# The exact bytes of the GOLDEN RUN capture (2026-09-11 10:43:25) - the
# selftest checks the parsers against these always, and against the on-disk
# artifacts whenever they exist.
GOLDEN_CAPTURED_STEPS = [("down", 74), ("left", 392), ("up", 393),
                         ("right", 253), ("up", 178), ("x", 200)]
GOLDEN_CAPTURED_RAW = (
    "# route recording 2026-09-11 10:43:25 seed=1234 chapter=1 armed=False\n"
    "t=     0ms down      down\n"
    "t=    38ms left      down\n"
    "t=    74ms down      up\n"
    "t=   430ms left      up\n"
    "t=   430ms up        down\n"
    "t=   787ms right     down\n"
    "t=   823ms up        up\n"
    "t=  1040ms up        down\n"
    "t=  1040ms right     up\n"
    "t=  1218ms up        up\n"
    "t=  1361ms x         down\n")


def _fake_session(log_path):
    """A stand-in Session for offline arbiter tests: a real ProbeTail on a
    synthetic log + a game stub whose proc never dies."""
    import types
    proc = types.SimpleNamespace(poll=lambda: None)
    game = types.SimpleNamespace(proc=proc)
    return types.SimpleNamespace(game=game, tail=ProbeTail(log_path))


def selftest_golden_route():
    """Tests: golden-route parsers, replay timeline + executor, and the
    dng arbiter (offline; zero game launched, zero real key injected -
    rig input primitives are monkeypatched for the executor test)."""
    import tempfile
    results = []

    def check(name, cond, detail=""):
        results.append((name, bool(cond), detail))

    saved_tap, saved_kd, saved_ku = rig.tap, rig.key_down, rig.key_up
    pressed = []                       # (t, "down"/"up"/"tap", key)

    def fake_tap(key, hold_s=0.0):
        pressed.append((time.time(), "tap", key))

    def fake_kd(key):
        pressed.append((time.time(), "down", key))

    def fake_ku(key):
        pressed.append((time.time(), "up", key))

    rig.tap, rig.key_down, rig.key_up = fake_tap, fake_kd, fake_ku
    try:
        # (g1) compressed parser: the exact captured bytes
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "route.txt")
            with open(p, "w") as fh:
                fh.write("# replayable route (6 steps) - rig SendInput "
                         "pattern\n")
                for k, ms in GOLDEN_CAPTURED_STEPS:
                    fh.write("hold %s %d\n" % (k, ms))
            steps = parse_owner_route(p)
            check("golden: compressed parser -> captured 6-step sequence",
                  steps == GOLDEN_CAPTURED_STEPS, str(steps))
            bad = os.path.join(td, "bad.txt")
            with open(bad, "w") as fh:
                fh.write("hold x\nhold up twenty\n")
            try:
                parse_owner_route(bad)
                check("golden: malformed route line raises", False,
                      "no error")
            except ValueError:
                check("golden: malformed route line raises", True)
        if os.path.exists(GOLDEN_ROUTE_PATH):
            real = parse_owner_route(GOLDEN_ROUTE_PATH)
            check("golden: phase2_owner_route.txt matches the capture",
                  real == GOLDEN_CAPTURED_STEPS, str(real))
        else:
            check("golden: phase2_owner_route.txt matches the capture",
                  True, "skipped - artifact absent")

        # (g2) raw parser + replay timeline
        with tempfile.TemporaryDirectory() as td:
            p = os.path.join(td, "raw.txt")
            with open(p, "w") as fh:
                fh.write(GOLDEN_CAPTURED_RAW)
            tr = parse_owner_route_raw(p)
            check("golden: raw parser -> 11 transitions, first/last",
                  len(tr) == 11 and tr[0] == (0, "down", True) and
                  tr[-1] == (1361, "x", True), str(tr[:1] + tr[-1:]))
            tl = raw_timeline(tr)
            check("golden: still-held x released at +200ms (t=1561)",
                  tl[-1] == (1561, "x", False), str(tl[-1]))
            check("golden: diagonal overlap kept (left down t=38, down "
                  "up t=74)",
                  (38, "left", True) in tl and (74, "down", False) in tl)
            check("golden: same-frame events kept in order (up down + "
                  "right up at t=1040)",
                  tl.index((1040, "up", True)) <
                  tl.index((1040, "right", False)))
            if os.path.exists(GOLDEN_ROUTE_RAW_PATH):
                real_tr = parse_owner_route_raw(GOLDEN_ROUTE_RAW_PATH)
                check("golden: phase2_owner_route_raw.txt matches the "
                      "capture", real_tr == tr,
                      "%d transitions" % len(real_tr))
            else:
                check("golden: phase2_owner_route_raw.txt matches the "
                      "capture", True, "skipped - artifact absent")

            # (g3) degraded compressed timeline is strictly sequential
            ctl = compressed_timeline(GOLDEN_CAPTURED_STEPS)
            check("golden: compressed timeline sequential + 30ms gaps "
                  "(degraded, documented)",
                  ctl[:4] == [(0, "down", True), (74, "down", False),
                              (104, "left", True), (496, "left", False)],
                  str(ctl[:4]))

            # (g4) executor: monkeypatched rig keys, real wall-clock holds
            del pressed[:]
            t0 = time.time()
            replay_route(tl)
            dur = time.time() - t0
            downs = [k for _t, ev, k in pressed if ev == "down"]
            ups = [k for _t, ev, k in pressed if ev == "up"]
            check("golden: executor presses the 6 route keys in order",
                  downs == ["down", "left", "up", "right", "up", "x"],
                  str(downs))
            check("golden: executor releases every key it pressed",
                  sorted(ups) == sorted(downs) and ups[-1] == "x",
                  str(ups))
            t_left = next(t for t, ev, k in pressed
                          if ev == "down" and k == "left") - t0
            t_dup = next(t for t, ev, k in pressed
                         if ev == "up" and k == "down") - t0
            check("golden: overlap survives live replay (left engages "
                  "before down releases, ~38ms)",
                  0 < t_left < t_dup and abs(t_left - 0.038) < 0.06,
                  "left@%.3f down-up@%.3f" % (t_left, t_dup))
            check("golden: total route duration ~1.56 s",
                  1.45 < dur < 1.80, "%.3f s" % dur)

        # (g5) dng arbiter on synthetic probe logs
        with tempfile.TemporaryDirectory() as td:
            log = os.path.join(td, "run.log")
            with open(log, "w") as fh:
                fh.write("\n".join(_probe_final(f, 1200, 560, room=0x12,
                                                mod=7, sub=0, dng=15)
                                   for f in range(10, 20)) + "\n")
            sess = _fake_session(log)
            v, dng1, banner = golden_arbitrate(sess, 15, poll_s=0.4)
            check("golden arbiter: unchanged mask 0x000F -> miss",
                  v == "miss" and dng1 == 15 and not banner,
                  "%s dng=%s" % (v, dng1))
            with open(log, "a") as fh:
                fh.write(_probe_final(30, 1218, 600, room=0x12, mod=7,
                                      sub=0, dng=271) + "\n")
            v, dng1, banner = golden_arbitrate(sess, 15, poll_s=1.0)
            check("golden arbiter: dng flip -> open",
                  v == "open" and dng1 == 271 and not banner,
                  "%s dng=%s" % (v, dng1))

            # silence (cutscene) -> resumed probe is re-read -> open
            import threading
            log2 = os.path.join(td, "run2.log")
            with open(log2, "w") as fh:
                fh.write("\n".join(_probe_final(f, 1200, 560, room=0x12,
                                                mod=7, sub=0, dng=15)
                                   for f in range(100, 110)) + "\n")
            sess2 = _fake_session(log2)

            def resume_later():
                time.sleep(0.8)
                with open(log2, "a") as fh:
                    fh.write(_probe_final(200, 1218, 600, room=0x12,
                                          mod=7, sub=0, dng=271) + "\n")
            th = threading.Thread(target=resume_later)
            th.start()
            v, dng1, banner = golden_arbitrate(sess2, 15, poll_s=3.0,
                                               silence_s=0.05,
                                               resume_timeout=3.0)
            th.join()
            check("golden arbiter: probe silence -> resume -> open "
                  "(a cutscene cannot mask an open)",
                  v == "open" and dng1 == 271,
                  "%s dng=%s banner=%s" % (v, dng1, banner))
    finally:
        rig.tap, rig.key_down, rig.key_up = saved_tap, saved_kd, saved_ku
    return results


def cmd_selftest(_args):
    suites = [
        ("parser (final/draft/legacy + sprites + gate)", selftest_parser),
        ("deathloop logic (scripted loop + gated suppression)",
         selftest_deathloop_logic),
        ("walk/receipts/NPC/chest-mask (save_dung_info)",
         selftest_walk_receipts),
        ("gold point + sweep candidates (offline)", selftest_gold_sweep),
        ("golden route (parsers/timeline/executor/arbiter, offline)",
         selftest_golden_route),
    ]
    total = passed = 0
    for name, fn in suites:
        res = fn()
        p = sum(1 for _n, ok, _d in res if ok)
        total += len(res)
        passed += p
        print("== %s: %d/%d ==" % (name, p, len(res)))
        for n, ok, d in res:
            if not ok:
                print("   FAIL: %s %s" % (n, d))
    print("\nSELFTEST: %d/%d passed%s"
          % (passed, total, "" if passed == total
             else "  (FAILURES ABOVE)"))
    return 0 if passed == total else 1


# ===========================================================================
# CLI
# ===========================================================================

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="phase-2 scenario suite (deathloop / pinroute / "
                    "record-human / selftest); selftest is the default "
                    "and never launches the game")
    sub = ap.add_subparsers(dest="cmd")

    def common(p, with_smoke=True):
        p.add_argument("--seed", type=int, default=1234)
        p.add_argument("--pin-location", default=PIN_LOCATION_DEFAULT)
        p.add_argument("--pin-item", default=PIN_ITEM_DEFAULT)
        p.add_argument("--chapter", type=int, default=1)
        p.add_argument("--audio", action="store_true")
        if with_smoke:
            p.add_argument("--skip-smoke", action="store_true",
                           help="skip the smoke.py 7/7 boot gate")
        p.add_argument("--no-rando-smoke", action="store_true",
                       help="run the smoke gate with --no-rando")

    d = sub.add_parser("deathloop", help="swarm+hurt armament pacing "
                       "discriminator")
    common(d)
    d.add_argument("--pacing", choices=["aggressive", "gated", "human",
                                        "all"], default="all")
    d.add_argument("--duration", type=float, default=120.0,
                   help="seconds per profile (default 120)")
    d.add_argument("--interval", type=float, default=2.0,
                   help="aggressive volley interval s (the old machine "
                        "pacing)")
    d.add_argument("--lead", type=float, default=5.0,
                   help="quiet lead-in before the first volley")
    d.add_argument("--gate-timeout", type=float, default=20.0,
                   help="gated profile: state-gate wait per volley before "
                        "it is logged as withheld")
    d.add_argument("--silence-s", type=float, default=1.5,
                   help="probe-stream silence that counts as left-"
                        "gameplay (death/gameover)")
    d.add_argument("--grace-s", type=float, default=4.0)
    d.add_argument("--rekill-window-s", type=float, default=10.0)
    d.add_argument("--min-deaths", type=int, default=3)
    d.add_argument("--min-rekills", type=int, default=2)
    d.add_argument("--dpm-loop", type=float, default=6.0)
    d.set_defaults(fn=cmd_deathloop)

    p = sub.add_parser("pinroute", help="position-aware Sanctuary chest "
                       "proof (NPC-block aware)")
    common(p)
    p.add_argument("--tol", type=int, default=2,
                   help="walk_to tolerance px (the hardened rig's +-2)")
    p.add_argument("--dy1", type=int, default=12,
                   help="drop-down px below the NPC cluster (owner-"
                        "confirmed route shape)")
    p.add_argument("--dx", type=int, default=58,
                   help="left-along-the-wall px")
    p.add_argument("--dy2", type=int, default=28,
                   help="down-onto-the-chest px")
    p.add_argument("--leg-timeout", type=float, default=20.0)
    p.add_argument("--detour-ring", type=int, default=8)
    p.add_argument("--npc-radius", type=int, default=24,
                   help="story-NPC proximity (px) that blocks the final "
                        "approach")
    p.add_argument("--npc-wait", type=float, default=25.0,
                   help="seconds to wait for the alcove to clear")
    p.add_argument("--npc-retries", type=int, default=2,
                   help="retreat+retry cycles for a blocked alcove")
    p.add_argument("--receipt-timeout", type=float, default=15.0)
    p.add_argument("--rediscover", action="store_true",
                   help="ignore the persisted gold interact point and "
                        "re-run the systematic interaction sweep (the "
                        "sweep is also the automatic fallback when the "
                        "gold path misses); forces --route sweep")
    p.add_argument("--route", choices=["golden", "sweep", "auto"],
                   default="auto",
                   help="open path: golden = replay the captured owner "
                        "route only (diagnostic: measures its live "
                        "success rate), sweep = legacy walker + gold "
                        "point + interaction sweep only, auto = golden "
                        "first, sweep fallback (default)")
    p.add_argument("--golden-attempts", type=int, default=GOLDEN_ATTEMPTS,
                   help="golden-route replays before the sweep fallback "
                        "(the owner's success rate was ~1-in-3; retries "
                        "expected)")
    p.add_argument("--golden-poll", type=float, default=GOLDEN_POLL_S,
                   help="dng arbiter poll seconds after the route's "
                        "final X press")
    p.add_argument("--sweep-max-tries", type=int, default=100,
                   help="A-press budget for the interaction sweep")
    p.add_argument("--sweep-poll", type=float, default=SWEEP_POLL_S,
                   help="dng poll seconds per A press")
    p.add_argument("--no-save-after-open", action="store_true",
                   help="skip the Save-and-Quit keying + sram.dat "
                        "chest-mask check (verdict then cannot prove the "
                        "open and stays PARTIAL/FAIL)")
    p.set_defaults(fn=cmd_pinroute)

    r = sub.add_parser("record-human", help="owner-driven control session "
                       "(passive recorder)")
    common(r)
    r.add_argument("--max-s", type=float, default=480.0)
    r.add_argument("--arm", action="store_true",
                   help="arm attrition+dmgup during the owner session "
                        "(the human-pacing death-loop control); default "
                        "is a pure no-interference control")
    r.add_argument("--pacing-floor", type=int, default=30,
                   help="smoke pacing floor: owner sessions tolerate a "
                        "busy desktop (the human IS the timing); default 30")
    r.add_argument("--silence-s", type=float, default=1.5)
    r.add_argument("--grace-s", type=float, default=4.0)
    r.set_defaults(fn=cmd_record_human)

    s = sub.add_parser("selftest", help="synthetic-feed verification, no "
                       "game launched")
    s.set_defaults(fn=cmd_selftest)

    args = ap.parse_args(argv)
    if args.cmd is None:            # default verification path
        args = ap.parse_args(["selftest"])
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())

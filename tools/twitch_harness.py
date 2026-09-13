#!/usr/bin/env python3
"""Automated regression harness for zelda3 Twitch verb handling.

Drives the game's drop-folder input (twitch_drop/, one "verb|arg|who|dur"
file per command, dur in FRAMES, 0 = use configured default) and asserts on
the RESULT log lines that twitch.c emits when debug=1. Runs the game in
test=1 mode so stat/flag verbs work at the title screen; world verbs
(smite/spawn/swarm) must still report DROPPED outside gameplay.

Known limitation: at the title screen link_health_capacity/link_magic_power
read 0, so heal clamps to 0 there; assertions are written against that
degenerate state and still catch clamp/logic regressions. Full in-gameplay
stat testing needs a loaded save (future: replay/automation).

Scoreboard (tracker.c): lifetime `deaths`/`commands` tallies persist in
scoreboard.txt (game CWD) and are reloaded on every boot. The harness
never zeroes the tally (the owner may want stream continuity); one full
run adds ~82 commands. Title-screen hurts down to 0 are NOT game-overs,
so `deaths` must never move during a harness run. To start a stream at
zero, delete scoreboard.txt (or pass --reset-scoreboard) beforehand.

Phases after the main suite (each boots its own short-lived game):
  scoreboard cross-check   commands delta == applied RESULTs observed
  ice_legacy=1             old flag-only ice must still START (coasting
                           distance itself is covered by
                           tools/gameplay_verify/probe_ice.py)
  modes reset on restart   fresh boot: bare toggles prove defaults OFF
  chat boss (v1)           CHAT BOSS state machine: auto-trigger (via the
                           bossrate rate-faking test verb), scripted damage,
                           forced VICTORY (counters + reward verbs), DEFEAT
                           window path (punishments + losses), auto-return
                           to DORMANT, and the boss_enabled=0 kill switch.
                           Runs in its own game with boss_enabled=1; the
                           main suite above runs boss_enabled=0 because a
                           live fight would inject its own RESULT lines.
  randomizer.ini           seed nickname appears on stdout and is
                           deterministic for the same seed

Usage:  python tools/twitch_harness.py [--reset-scoreboard]
Exit 0 = all checks passed. Requires zelda3.exe (build_msvc.cmd).
"""
import os
import re
import subprocess
import sys
import time

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # zelda3/
EXE = os.path.join(BASE, "zelda3.exe")
CFG = os.path.join(BASE, "twitch_config.txt")
DROP = os.path.join(BASE, "twitch_drop")
RUNLOG = os.path.join(BASE, "harness_run.log")
RUNLOG_ICE = os.path.join(BASE, "harness_ice.log")
RANDO_INI = os.path.join(BASE, "randomizer.ini")
RANDO_LOG = os.path.join(BASE, "randomizer_log.txt")
RANDO_SEED = 424242  # any fixed seed; 0 would derive from the clock
SCOREBOARD = os.path.join(BASE, "scoreboard.txt")
SCOREBOARD_LINE = os.path.join(BASE, "scoreboard_line.txt")
BOOT_SECS = 4  # game boot (title screen) before drops are meaningful

TEST_CFG = """\
token=
user=
channel=
enabled=0
drop=1
debug=1
test=1
cooldown=600
effect_secs=5
vs_mode=0
boss_enabled=0
"""
# boss_enabled=0 in the main suite: the CHAT BOSS auto-trigger fires on
# command-rate bursts (the rapid-fire section crosses any sane threshold),
# and a live fight injects its own attack RESULTs into the log, breaking
# the scoreboard cross-check. The boss gets a dedicated phase below with
# boss_enabled=1 (this is also the documented stream kill switch).


class Game:
    """One zelda3.exe process with its own config text and stdout log.

    The game reads twitch_config.txt/randomizer.ini/scoreboard.txt from its
    CWD, so only one Game may run at a time; start() publishes our config,
    stop() restores whatever was there before.
    """

    def __init__(self, cfg_text=TEST_CFG, log_path=RUNLOG):
        self.cfg_text = cfg_text
        self.log_path = log_path
        self.proc = None
        self.log = None
        self.log_pos = 0
        self.all_results = []  # every RESULT line ever seen on this log
        self.saved_cfg = None

    def start(self):
        os.makedirs(DROP, exist_ok=True)
        for f in os.listdir(DROP):
            os.remove(os.path.join(DROP, f))
        self.saved_cfg = open(CFG).read() if os.path.exists(CFG) else None
        with open(CFG, "w") as fh:
            fh.write(self.cfg_text)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([EXE], cwd=BASE, stdout=self.log,
                                     stderr=subprocess.STDOUT)
        time.sleep(BOOT_SECS)

    def drop(self, line):
        # Publish atomically: the game globs twitch_drop/*.txt and treats an
        # empty read as "delete and skip" (half-write tolerance), so a file
        # created empty and filled later can silently swallow a command.
        # .part never matches *.txt; os.replace is atomic on the same volume.
        final = os.path.join(DROP, "cmd_%d_%d.txt" % (time.time_ns(), len(line)))
        tmp = final + ".part"
        with open(tmp, "w") as fh:
            fh.write(line + "\n")
        os.replace(tmp, final)

    def _read_new(self):
        self.log.flush()
        with open(self.log_path, "r", errors="replace") as fh:
            fh.seek(self.log_pos)
            new = fh.read()
            self.log_pos = fh.tell()
        lines = [l for l in new.splitlines() if l.startswith("RESULT")]
        self.all_results += lines
        return lines

    def results(self, timeout=10.0):
        """Collect RESULT lines for up to timeout; return when any appear."""
        deadline = time.time() + timeout
        lines = self._read_new()
        while not lines and time.time() < deadline:
            time.sleep(0.2)
            lines = self._read_new()
        return lines

    def wait_for(self, pattern, timeout=20.0):
        """Keep collecting until pattern is seen or timeout; returns all lines."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        seen = []
        while time.time() < deadline:
            seen += self._read_new()
            for l in seen:
                if rx.search(l):
                    return seen
            time.sleep(0.2)
        return seen

    def wait_for_count(self, pattern, want, timeout=20.0):
        """Like wait_for but only returns once the pattern matched >= want
        times (all lines land in one flushed batch, so this is exact)."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        seen = []
        while time.time() < deadline:
            seen += self._read_new()
            if sum(1 for l in seen if rx.search(l)) >= want:
                break
            time.sleep(0.2)
        return seen

    def wait_in_log(self, pattern, timeout=20.0):
        """Poll the whole boot log for an arbitrary (non-RESULT) line,
        e.g. the [randomizer] banner; returns the match or None."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(self.log_path, "r", errors="replace") as fh:
                    m = rx.search(fh.read())
            except OSError:
                m = None
            if m:
                return m
            time.sleep(0.2)
        return None

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                rc = self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                rc = 'killed'
            print('  [game exit code: %s]' % rc)
        if self.log:
            self.log.close()
        for f in os.listdir(DROP):
            os.remove(os.path.join(DROP, f))
        if self.saved_cfg is not None:
            with open(CFG, "w") as fh:
                fh.write(self.saved_cfg)


PASS, FAIL = [], []


def check(name, ok, detail=""):
    if ok:
        PASS.append(name)
        print("  ok   %s   %s" % (name, detail))
    else:
        FAIL.append(name)
        print("  FAIL %s   %s" % (name, detail))


def expect(name, lines, pattern):
    rx = re.compile(pattern)
    for l in lines:
        if rx.search(l):
            check(name, True, l)
            return
    check(name, False, "wanted /%s/; got %s" % (pattern, lines))


def read_scoreboard():
    """Dict of the four lifetime tallies from scoreboard.txt; missing
    file/keys count as 0 (Scoreboard_Init would recreate them with the same
    values on next boot)."""
    vals = {"deaths": 0, "commands": 0, "bosses_won": 0, "losses": 0}
    if os.path.exists(SCOREBOARD):
        with open(SCOREBOARD, "r", errors="replace") as fh:
            for line in fh:
                m = re.match(r"^(deaths|commands|bosses_won|losses)=(\d+)", line.strip())
                if m:
                    vals[m.group(1)] = int(m.group(2))
    return vals


def reset_scoreboard():
    """Zero the persisted tally (opt-in only: the owner may want continuity).
    Mirrors tracker.c Scoreboard_Write's two output files."""
    with open(SCOREBOARD, "w") as fh:
        fh.write("deaths=0\ncommands=0\n")
    with open(SCOREBOARD_LINE, "w", encoding="utf-8") as fh:
        # \u00b7 = the UTF-8 middle dot the C code writes
        fh.write("deaths: 0 \u00b7 commands: 0\n")
    print("scoreboard.txt zeroed (--reset-scoreboard)")


def rando_boot(log_path):
    """Boot one short game with randomizer.ini enabled; return the nickname
    from the [randomizer] stdout banner, or None."""
    rg = Game(log_path=log_path)
    rg.start()
    m = rg.wait_in_log(r'\[randomizer\] enabled, seed=%d - "([^"]+)"' % RANDO_SEED)
    rg.stop()
    return m.group(1) if m else None


# What the scoreboard tallies (twitch.c TwitchApply): every command that
# passes the vs-mode and gameplay gates increments `commands` BEFORE verb
# dispatch. So a known verb whose effect then no-ops still counts even
# though its RESULT line says DROPPED (steal with an empty pool prints
# reason=nothing_to_steal; unhandled verbs print reason=unhandled). Only
# DROPPED reason=vs_mode / not_in_gameplay / unknown return before the
# tally. Excluded from the count: "effect=END" expiry lines and the extra
# "nice=1" line (it accompanies a command that also prints its own RESULT).
def counts_for_scoreboard(line):
    if "effect=END" in line or "nice=1" in line:
        return False
    # attrition's "cap=N->M" container-loss telemetry accompanies a command
    # (hurt) or an engine event, never consumes a drop of its own
    if "verb=attrition cap=" in line:
        return False
    # rupeesteal's "taken=N total=M" per-hit telemetry is the same deal (it
    # rides along inside modprobe probes and real hits, never its own drop)
    if "verb=rupeesteal taken=" in line:
        return False
    # CHAT BOSS v1 internals: boss state/attack/crit/query telemetry and the
    # boss-sourced reward/punishment verbs (refill/fairy/curse/swarm/deny
    # applied by "CHAT-BOSS"/"THE BOSS") are not chat traffic - twitch.c
    # skips Scoreboard_LogCommand for them (g_tw_boss_source). Excluded here
    # too so the cross-check stays honest even if a future phase enables the
    # boss inside the main suite. (bosshp/bossset/bossrate RESULTs are
    # covered by the verb=boss* match.)
    if re.match(r"RESULT (verb=boss|verb=bosshp|verb=bossstart|verb=bossset|verb=bossrate)", line):
        return False
    if "DROPPED" not in line:
        return True
    return ("reason=nothing_to_steal" in line or "reason=unhandled" in line)


def another_game_running():
    """True if a zelda3.exe we did not spawn is already running. A second
    instance consumes twitch_drop/ files (commands vanish without a RESULT
    in our log) and writes competing scoreboard.txt updates, so any run
    started alongside it produces garbage - fail fast instead."""
    try:
        if os.name == "nt":
            out = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq zelda3.exe", "/NH"],
                capture_output=True, text=True, timeout=10).stdout
        else:
            out = subprocess.run(["pgrep", "-x", "zelda3"],
                                 capture_output=True, text=True,
                                 timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return False  # cannot tell - run anyway, the checks will judge
    return "zelda3" in out.lower()


def main():
    if "--reset-scoreboard" in sys.argv[1:]:
        reset_scoreboard()
    if not os.path.exists(EXE):
        print("zelda3.exe missing - run build_msvc.cmd first")
        return 1
    if another_game_running():
        print("another zelda3.exe is already running (stream rig?) - it"
              " would steal twitch_drop/ files and corrupt the scoreboard.")
        print("close it first, then rerun the harness.")
        return 1
    g = Game()
    print("== zelda3 twitch harness ==")
    # scoreboard baseline BEFORE the first boot (file may not exist = all 0)
    sb0 = read_scoreboard()
    g.start()
    rapid_got = 0
    try:
        # ---- instant stat verbs (title screen: capacity/magic read 0) ----
        g.drop("heal|999|tester|0")
        lines = g.results()
        m = re.search(r"verb=heal hp=\d+->(\d+)$", " ".join(lines))
        cap = int(m.group(1)) if m else -1
        check("heal clamps to capacity", cap == 0,
              "capacity at title = %d [%s]" % (cap, lines))

        g.drop("hurt|999|tester|0");  expect("hurt floors at 0", g.results(), r"verb=hurt hp=\d+->0$")
        g.drop("heal|10|tester|0");   expect("heal clamped by cap=0", g.results(), r"verb=heal hp=0->0$")
        g.drop("drain||tester|0");    expect("drain floors at 0", g.results(), r"verb=drain magic=0->0$")
        g.drop("mp||tester|0");       expect("mp fills to 128", g.results(), r"verb=mp magic=0->128$")
        g.drop("drain||tester|0");    expect("drain 32", g.results(), r"verb=drain magic=128->96$")
        g.drop("refill||tester|0");   expect("refill", g.results(), r"verb=refill hp=\d+ magic=128$")
        g.drop("rupees|50|tester|0"); expect("rupees 50", g.results(), r"verb=rupees rupees=\d+->50$")
        g.drop("tax|20|tester|0");    expect("tax 20", g.results(), r"verb=tax rupees=50->30$")
        g.drop("tax|999|tester|0");   expect("tax floors at 0", g.results(), r"verb=tax rupees=\d+->0$")
        g.drop("bombs|7|tester|0");   expect("bombs 7", g.results(), r"verb=bombs bombs=\d+->7$")
        g.drop("arrows|25|tester|0"); expect("arrows 25", g.results(), r"verb=arrows arrows=\d+->25$")

        # ---- timed effects: field 4 is FRAMES, 0/absent = default ----
        # confuse is SCREEN-based (lane design): arg = room transitions
        g.drop("confuse|3|tester|0")
        expect("confuse 3 screens", g.results(), r"verb=confuse effect=START screens=3$")
        g.drop("confuse||tester|0")
        expect("confuse default 2 screens", g.results(), r"verb=confuse effect=START screens=2$")
        g.drop("flip||tester|60")
        expect("flip explicit 60f", g.results(), r"verb=flip effect=START frames=60$")
        g.drop("root||tester|0")
        expect("root default 300f", g.results(), r"verb=root effect=START frames=300$")
        g.drop("curse||tester|0")
        expect("curse default 4x=1200f", g.results(), r"verb=curse effect=START frames=1200$")
        g.drop("freeze||tester|90")
        expect("freeze explicit 90f", g.results(), r"verb=freeze effect=START frames=90$")

        # ---- lane's verbs: speed/slow/ice/bunny/fairy/deny ----
        g.drop("speed||tester|120")
        expect("speed explicit 120f", g.results(), r"verb=speed effect=START frames=120$")
        g.drop("slow||tester|0")
        expect("slow default 300f", g.results(), r"verb=slow effect=START frames=300$")
        g.drop("ice||tester|90")
        expect("ice explicit 90f", g.results(), r"verb=ice effect=START frames=90$")
        g.drop("illusion||tester|0")
        expect("bunny poof 300f", g.results(), r"verb=bunny effect=START frames=300$")
        g.drop("fairy||tester|0")
        expect("fairy needs gameplay", g.results(), r"verb=fairy DROPPED reason=not_in_gameplay")
        g.drop("arise||tester|0")
        expect("arise needs gameplay", g.results(), r"verb=arise DROPPED reason=not_in_gameplay")
        g.drop("chicken||tester|0")
        expect("chicken alias known", g.results(), r"verb=chicken DROPPED reason=not_in_gameplay")
        g.drop("deny||tester|90")
        expect("deny 90f", g.results(), r"verb=deny effect=START frames=90$")
        g.drop("denyboots||tester|0")
        expect("denyboots default", g.results(), r"verb=denyboots effect=START frames=300$")

        # ---- empty inventory at title: nothing to steal ----
        g.drop("steal||tester|0")
        expect("steal empty pool", g.results(), r"verb=steal DROPPED reason=nothing_to_steal")

        # ---- guards ----
        g.drop("spawn|octorok 2|tester|0"); expect("spawn needs gameplay", g.results(), r"verb=spawn DROPPED reason=not_in_gameplay")
        g.drop("smite||tester|0");          expect("smite needs gameplay", g.results(), r"verb=smite DROPPED reason=not_in_gameplay")
        g.drop("xyzzy||tester|0");          expect("unknown verb", g.results(), r"verb=xyzzy DROPPED reason=unknown")

        # ---- malformed input must not wedge anything ----
        g.drop("|||");                      expect("all-empty fields", g.results(), r"DROPPED reason=unknown")
        g.drop("not a command at all");     expect("prose file", g.results(), r"DROPPED reason=unknown")
        g.drop("heal|999|tester|0|extra|junk")
        expect("extra fields ignored", g.results(), r"verb=heal hp=\d+->\d+$")
        g.drop("heal|" + "9" * 300 + "|t|0")
        expect("huge arg safe", g.results(), r"verb=heal")
        g.drop("HEAL|0|Tester|0");          expect("case-insensitive verb", g.results(), r"verb=heal hp=\d+->\d+$")
        g.drop("heal|-5|t|0");              expect("negative arg", g.results(), r"verb=heal hp=\d+->\d+$")

        # ---- drop path bypasses IRC cooldown: rapid fire, all applied ----
        for i in range(12):
            g.drop("hurt|1|rush%d|0" % i)
        lines = g.wait_for_count(r"verb=hurt hp=\d+->\d+$", 12, 20)
        rapid_got = sum(1 for l in lines if re.search(r"verb=hurt hp=\d+->\d+$", l))
        check("cooldown bypass", rapid_got >= 10, "%d rapid hurts applied" % rapid_got)

        remaining = os.listdir(DROP)
        check("drop folder drained", not remaining, str(remaining))

        # ---- persistent modes: attrition (default off, in-session only) ----
        # Modes bypass the gameplay gate: usable at the title screen. Bare
        # verb (or junk arg) toggles; "on"/"off" wins explicitly; "hardmode"
        # is an alias whose RESULT resolves to the canonical verb.
        g.drop("attrition|on|tester|0")
        expect("attrition on", g.results(), r"verb=attrition mode=on$")
        g.drop("attrition||tester|0")
        expect("bare attrition toggles off", g.results(), r"verb=attrition mode=off$")
        g.drop("hardmode|on|tester|0")
        expect("hardmode alias turns on", g.results(), r"verb=attrition mode=on$")
        g.drop("attrition|junk|tester|0")
        expect("junk arg toggles off", g.results(), r"verb=attrition mode=off$")
        g.drop("hardmode||tester|0")
        expect("bare alias toggles on", g.results(), r"verb=attrition mode=on$")

        # capacity reads 0 at the title: an armed mode has nothing to lose
        g.drop("hurt|8|tester|0")
        seen = g.wait_for(r"verb=hurt hp=\d+->\d+$")
        expect("hurt applies while armed", seen, r"verb=hurt hp=\d+->\d+$")
        check("cap=0 skip: nothing to lose",
              not any("verb=attrition cap=" in l for l in seen), str(seen))
        g.drop("attrition|off|tester|0")
        expect("attrition off", g.results(), r"verb=attrition mode=off$")

        # ---- container loss + 3-heart floor (needs cap>0: setcap, test=1) ----
        # setcap is a harness-only verb (twitch.c gates it on test_mode) that
        # seeds capacity so the loss path is observable without a save.
        g.drop("setcap|40|tester|0")
        expect("setcap 40", g.results(), r"verb=setcap cap=\d+->40$")
        g.drop("attrition|on|tester|0")
        expect("attrition armed again", g.results(), r"verb=attrition mode=on$")
        g.drop("hurt|8|tester|0")
        expect("hit costs one container", g.wait_for(r"verb=attrition cap=40->32$"),
               r"verb=attrition cap=40->32$")
        g.drop("setcap|24|tester|0")
        expect("setcap to the floor", g.results(), r"verb=setcap cap=\d+->24$")
        g.drop("hurt|8|tester|0")
        seen = g.wait_for(r"verb=hurt hp=\d+->\d+$")
        expect("hurt at the floor", seen, r"verb=hurt hp=\d+->\d+$")
        check("floor: capacity never below 24",
              not any("verb=attrition cap=" in l for l in seen), str(seen))

        # off stops further loss (losses are permanent - nothing is restored)
        g.drop("attrition|off|tester|0")
        expect("attrition disarmed", g.results(), r"verb=attrition mode=off$")
        g.drop("setcap|40|tester|0")
        expect("setcap 40 again", g.results(), r"verb=setcap cap=\d+->40$")
        g.drop("hurt|8|tester|0")
        seen = g.wait_for(r"verb=hurt hp=\d+->\d+$")
        expect("hurt while off", seen, r"verb=hurt hp=\d+->\d+$")
        check("off: no further loss", not any("verb=attrition cap=" in l for l in seen),
              str(seen))
        g.drop("setcap|0|tester|0")          # hygiene: title-screen default
        expect("setcap reset to 0", g.results(), r"verb=setcap cap=\d+->0$")

        # ---- enemy modifiers: dmgup / mpsteal / rupeesteal / hardmods ----
        # Same persistent-mode contract as attrition: default off, bypass the
        # gameplay gate, on/off explicit, bare verb (or junk arg) toggles.
        # The damage-time effects can't fire on their own at the title screen
        # (nothing damages Link there), so the harness probes them with
        # "modprobe" (test=1 verb, like setcap): it pushes a synthetic hit
        # through Twitch_ModifyDamage and RESULT-prints the modified outputs,
        # seeded to full health/magic/300 rupees so the deltas are
        # deterministic. Each probe re-seeds from scratch (state is saved and
        # restored), so consecutive rupeesteal probes all read 300->280; the
        # accumulating session tally (stolen_total) is the theft witness.
        g.drop("dmgup|on|tester|0")
        expect("dmgup on", g.results(), r"verb=dmgup mode=on$")
        g.drop("modprobe|8|tester|0")
        expect("dmgup doubles damage", g.results(), r"verb=modprobe dmg=8 hp=160->144 magic=128->128 rupees=300->300 ")
        g.drop("dmgup|off|tester|0")
        expect("dmgup off", g.results(), r"verb=dmgup mode=off$")
        g.drop("modprobe|8|tester|0")
        expect("dmgup off: vanilla damage", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->128 rupees=300->300 ")
        g.drop("dmgup||tester|0")
        expect("bare dmgup toggles on", g.results(), r"verb=dmgup mode=on$")
        g.drop("dmgup|junk|tester|0")
        expect("junk arg toggles off", g.results(), r"verb=dmgup mode=off$")

        g.drop("mpsteal|on|tester|0")
        expect("mpsteal on", g.results(), r"verb=mpsteal mode=on$")
        g.drop("modprobe|8|tester|0")
        expect("mpsteal drains 16 magic", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->112 rupees=300->300 ")
        g.drop("mpsteal|off|tester|0")
        expect("mpsteal off", g.results(), r"verb=mpsteal mode=off$")

        g.drop("rupeesteal|on|tester|0")
        expect("rupeesteal on", g.results(), r"verb=rupeesteal mode=on$")
        g.drop("modprobe|8|tester|0")
        expect("rupeesteal takes 20", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->128 rupees=300->280 stolen_total=20$")
        g.drop("modprobe|8|tester|0")
        expect("rupeesteal total accumulates", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->128 rupees=300->280 stolen_total=40$")
        g.drop("rupeesteal|off|tester|0")
        expect("rupeesteal off", g.results(), r"verb=rupeesteal mode=off$")
        g.drop("modprobe|8|tester|0")
        expect("stolen total survives mode off", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->128 rupees=300->300 stolen_total=40$")

        g.drop("dmgup|on|tester|0")
        expect("dmgup armed for kill probe", g.results(), r"verb=dmgup mode=on$")
        g.drop("modprobe|90|tester|0")
        expect("double damage turns 90 into a kill", g.results(), r"verb=modprobe dmg=90 hp=160->0 ")
        g.drop("dmgup|off|tester|0")
        expect("dmgup disarmed after kill probe", g.results(), r"verb=dmgup mode=off$")

        # hardmods: GROUP toggle over the three enemy-hit modifiers (attrition
        # is deliberately outside the group). on = all on, off = all off, bare
        # = collapse (none on -> all on, any on -> all off); individual
        # toggles keep working inside an armed group.
        g.drop("hardmods|on|tester|0")
        expect("hardmods on arms the group", g.results(), r"verb=hardmods mode=on$")
        g.drop("modprobe|8|tester|0")
        expect("hardmods applies all three at once", g.results(), r"verb=modprobe dmg=8 hp=160->144 magic=128->112 rupees=300->280 stolen_total=60$")
        g.drop("dmgup|off|tester|0")
        expect("individual toggle beats the group", g.results(), r"verb=dmgup mode=off$")
        g.drop("modprobe|8|tester|0")
        expect("dmgup off, rest of group alive", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->112 rupees=300->280 stolen_total=80$")
        g.drop("hardmods|off|tester|0")
        expect("hardmods off sweeps all", g.results(), r"verb=hardmods mode=off$")
        g.drop("modprobe|8|tester|0")
        expect("group off: vanilla pass-through", g.results(), r"verb=modprobe dmg=8 hp=160->152 magic=128->128 rupees=300->300 stolen_total=80$")
        g.drop("hardmods||tester|0")
        expect("bare hardmods: all off -> all on", g.results(), r"verb=hardmods mode=on$")
        g.drop("hardmods||tester|0")
        expect("bare hardmods: any on -> all off", g.results(), r"verb=hardmods mode=off$")
    finally:
        g.stop()

    # ---- scoreboard: tracker.c lifetime tally, cross-checked here ----
    # Every command past the vs-mode/gameplay gates bumps `commands` right
    # after Tracker_LogActivity; the IRC-connect tagline shares the activity
    # path but NOT Scoreboard_LogCommand, so it can never be counted. Hurts
    # to 0 at the title screen are not game-overs: `deaths` must not move.
    print("\n== scoreboard (scoreboard.txt) ==")
    sb1 = read_scoreboard()
    counted = [l for l in g.all_results if counts_for_scoreboard(l)]
    delta = sb1["commands"] - sb0["commands"]
    check("scoreboard counts every tallied command", delta == len(counted),
          "commands %d -> %d (+%d), %d tallied RESULTs seen"
          % (sb0["commands"], sb1["commands"], delta, len(counted)))
    # 70 drops in the suite above tally by design:
    #   12 stat + 6 timed + 6 lane + 4 malformed-but-valid + 1 steal (its
    #   empty-pool no-op still counts) + 16 attrition/mode/setcap checks
    #   (the "verb=attrition cap=" telemetry lines are excluded above)
    #   + 25 enemy-modifier checks (toggles + modprobe probes; the
    #   "verb=rupeesteal taken=" telemetry lines are excluded above too);
    #   plus the 12 rapid-fire hurts.
    fixed_tallied = 70
    check("scoreboard tally complete (no silently lost drops)",
          delta == fixed_tallied + rapid_got,
          "expected +%d (%d suite + %d rapid), got +%d"
          % (fixed_tallied + rapid_got, fixed_tallied, rapid_got, delta))
    check("scoreboard deaths untouched by harness", sb1["deaths"] == sb0["deaths"],
          "deaths %d -> %d (title hurts are not game-overs)"
          % (sb0["deaths"], sb1["deaths"]))
    line_txt = ""
    if os.path.exists(SCOREBOARD_LINE):
        with open(SCOREBOARD_LINE, encoding="utf-8", errors="replace") as fh:
            line_txt = fh.read().strip()
    check("scoreboard_line.txt mirrors tally",
          ("deaths: %d" % sb1["deaths"]) in line_txt
          and ("commands: %d" % sb1["commands"]) in line_txt
          and ("bosses: %d-%d" % (sb1["bosses_won"], sb1["losses"])) in line_txt,
          repr(line_txt))

    # ---- ice_legacy=1 kill switch: old flag-only ice behavior ----
    # Only the RESULT/log contract is asserted here (START, no crash); ice
    # COASTING DISTANCE is measured in-game by
    # tools/gameplay_verify/probe_ice.py, which can't be done at a title
    # screen with no input automation.
    print("\n== ice_legacy kill switch ==")
    g2 = Game(cfg_text=TEST_CFG + "ice_legacy=1\n", log_path=RUNLOG_ICE)
    g2.start()
    try:
        g2.drop("ice||tester|90")
        expect("ice legacy flag still STARTs ice", g2.results(),
               r"verb=ice effect=START frames=90$")
        check("ice legacy: game stays up", g2.proc.poll() is None,
              "no crash with ice_legacy=1")
    finally:
        g2.stop()

    # ---- modes reset on restart (v1 modes are in-session only) ----
    # A bare "!<mode>" TOGGLES, so seeing mode=on from a bare toggle on a
    # fresh boot proves that mode started OFF (a default-on mode would have
    # toggled to off). Nothing is persisted to twitch_config.txt. The final
    # hardmods drop proves the group sweep saw the three armed modifiers.
    print("\n== modes reset on restart ==")
    g3 = Game(log_path=os.path.join(BASE, "harness_attrition.log"))
    g3.start()
    try:
        g3.drop("attrition||restart-tester|0")
        expect("fresh boot: bare toggle -> on proves default off", g3.results(),
               r"verb=attrition mode=on$")
        g3.drop("dmgup||restart-tester|0")
        expect("fresh boot: dmgup default off", g3.results(), r"verb=dmgup mode=on$")
        g3.drop("mpsteal||restart-tester|0")
        expect("fresh boot: mpsteal default off", g3.results(), r"verb=mpsteal mode=on$")
        g3.drop("rupeesteal||restart-tester|0")
        expect("fresh boot: rupeesteal default off", g3.results(), r"verb=rupeesteal mode=on$")
        g3.drop("hardmods||restart-tester|0")
        expect("fresh boot: hardmods sweeps the armed group off", g3.results(),
               r"verb=hardmods mode=off$")
        check("fresh boot: game stays up", g3.proc.poll() is None)
    finally:
        g3.stop()

    # ---- CHAT BOSS v1 (twitch.c boss state machine) ----
    # Dedicated game with boss_enabled=1 (the main suite runs the documented
    # kill switch so rate bursts can't auto-start a fight mid-suite). Covers,
    # per the v1 verify list: bosshp query -> auto-trigger (unit-checked via
    # the bossrate rate-faking verb, then the real tick path) -> scripted
    # accepted commands reducing HP -> force-defeat of the boss (victory
    # counters + reward verbs) -> auto-return to DORMANT -> DEFEAT window
    # path (punishments + losses counter) -> boss_enabled=0 kill switch.
    print("\n== chat boss ==")
    BOSS_FEED = os.path.join(BASE, "boss_feed.txt")
    if os.path.exists(BOSS_FEED):
        os.remove(BOSS_FEED)               # stale result line from an old run

    def read_feed():
        with open(BOSS_FEED, "r", errors="replace") as fh:
            return fh.read()

    bb0 = read_scoreboard()
    g4 = Game(cfg_text=TEST_CFG.replace("boss_enabled=0", "boss_enabled=1") +
              "boss_min_rate=6\nboss_secs=120\n",
              log_path=os.path.join(BASE, "harness_boss.log"))
    g4.start()
    try:
        # query verb at DORMANT (fresh boot: maxhp is 0 until a fight starts)
        g4.drop("bosshp||tester|0")
        expect("bosshp answers at DORMANT", g4.results(),
               r"verb=bosshp state=DORMANT hp=0/0 ")

        # auto-trigger logic, unit-checked by faking the rate counter:
        # bossrate (test=1) injects N synthetic accepted-command timestamps
        # and prints the trigger DECISION against the live conditions.
        g4.drop("bossrate|0|tester|0")
        expect("rate 0/min: would not start", g4.results(),
               r"verb=bossrate rate=0 threshold=6 cooldown_ok=1 gameplay=0 would_start=0$")
        g4.drop("bossrate|30|tester|0")
        expect("rate 6/min: would start", g4.results(),
               r"verb=bossrate rate=6 threshold=6 cooldown_ok=1 gameplay=0 would_start=1$")
        # ...and the real auto path (test=1 bypasses ONLY the gameplay gate,
        # so the tick can fire at the title screen; production streams never
        # do this). HP is deterministic here: rate 6 -> 0.65*6*120*15/60 =
        # 117 -> rounds 120 -> clamped to the 150 floor. The auto-start can
        # land in the SAME flushed batch as the bossrate RESULT above (same
        # game frame), which results() then already consumed - so poll
        # all_results (every RESULT this game has logged) instead.
        auto_rx = r"verb=bossstart state=ACTIVE hp=150/150 rate=6 reason=auto$"
        deadline = time.time() + 5
        while time.time() < deadline and \
                not any(re.search(auto_rx, l) for l in g4.all_results):
            time.sleep(0.2)
            g4._read_new()
        check("auto-trigger starts the fight",
              any(re.search(auto_rx, l) for l in g4.all_results),
              [l for l in g4.all_results if "bossstart" in l])

        # boss_feed.txt appeared with the HP line the overlay box parses
        deadline = time.time() + 5
        while not os.path.exists(BOSS_FEED) and time.time() < deadline:
            time.sleep(0.2)
        feed_txt = read_feed()
        check("boss_feed.txt has the HP line",
              "HP: 150/150" in feed_txt and "phase: 1" in feed_txt, feed_txt.strip())

        # scripted accepted commands reduce HP (15 each; a crit rolls 50 and
        # is accepted as a valid alternative all through this phase)
        g4.drop("heal|1|alice|0")
        g4.drop("hurt|1|bob|0")
        g4.wait_for_count(r"verb=hurt hp=\d+->\d+$", 1, 10)
        time.sleep(2.5)                    # the 2 s pulse settles damage
        g4.drop("bosshp||tester|0")
        m = re.search(r"verb=bosshp state=ACTIVE hp=(\d+)/150",
                      " ".join(g4.results()))
        hp_now = int(m.group(1)) if m else -1
        check("accepted commands damage the boss", 50 <= hp_now <= 120,
              "hp now %d (150 - 2x15, crits allowed)" % hp_now)

        # exact single-command delta: normalize with bossset (clears pending)
        g4.drop("bossset|150|tester|0")
        expect("bossset normalizes HP", g4.results(), r"verb=bossset hp=150/150$")
        g4.drop("hurt|1|carol|0")
        g4.wait_for(r"verb=hurt hp=\d+->\d+$")
        time.sleep(2.5)
        g4.drop("bosshp||tester|0")
        m = re.search(r"verb=bosshp state=ACTIVE hp=(\d+)/150",
                      " ".join(g4.results()))
        hp_now = int(m.group(1)) if m else -1
        check("one accepted command deals 15 (or 50 crit)", hp_now in (135, 100),
              "delta %d" % (150 - hp_now if hp_now > 0 else -1))

        # attack scheduler alive: the first P1 pulse (15 s into the fight)
        # skips at the title screen - deterministic, the window is 120 s and
        # the politeness rule says non-gameplay ticks never throw effects
        expect("attacks skip outside gameplay", g4.wait_for(
                   r"verb=boss attack=skip reason=not_in_gameplay$", 25),
               r"verb=boss attack=skip reason=not_in_gameplay$")

        # force-defeat of the BOSS = chat VICTORY: reward verbs + counter
        g4.drop("bossset|10|tester|0")
        expect("bossset to lethal range", g4.results(), r"verb=bossset hp=10/150$")
        g4.drop("hurt|1|dave|0")
        seen = g4.wait_for(r"RESULT verb=boss state=VICTORY mvp=", 15)
        expect("victory resolves", seen, r"verb=boss state=VICTORY mvp=")
        expect("victory reward: refill", seen, r"verb=refill hp=\d+ magic=128$")
        # fairy is a WORLD verb: attempted by the payoff table, dropped by
        # the standard gameplay gate at the title screen (fires on stream)
        expect("victory reward: fairy attempted", seen,
               r"verb=fairy DROPPED reason=not_in_gameplay")
        feed_txt = read_feed()
        check("victory feed line",
              "VICTORY" in feed_txt and "CHAT SLAYS" in feed_txt, feed_txt.strip())

        # auto-return to DORMANT after the 5 s resolve hold
        time.sleep(6)
        g4.drop("bosshp||tester|0")
        expect("returns to DORMANT", g4.results(),
               r"verb=bosshp state=DORMANT hp=0/150 ")

        # DEFEAT path: manual bossstart (test verb: skips rate+cooldown but
        # not the kill switch), then force the window to ~5 frames
        g4.drop("bossstart||tester|0")
        expect("manual bossstart", g4.results(),
               r"verb=bossstart state=ACTIVE hp=\d+/\d+ rate=\d+ reason=manual$")
        g4.drop("bossset|-5|tester|0")
        seen = g4.wait_for(r"RESULT verb=boss state=DEFEAT hp=", 10)
        expect("window expiry = defeat", seen, r"verb=boss state=DEFEAT hp=")
        expect("punishment: curse 30s (4x internal)", seen,
               r"verb=curse effect=START frames=1800$")
        expect("punishment: deny 15s", seen, r"verb=deny effect=START frames=900$")
        check("punishment: swarm 4 attempted (world verb, title = dropped)",
              any("verb=swarm DROPPED reason=not_in_gameplay" in l for l in seen),
              str(seen))
        feed_txt = read_feed()
        check("defeat shame line in feed", "ONE job" in feed_txt, feed_txt.strip())
    finally:
        g4.stop()

    bb1 = read_scoreboard()
    check("scoreboard bosses_won +1",
          bb1["bosses_won"] == bb0["bosses_won"] + 1,
          "bosses_won %d -> %d" % (bb0["bosses_won"], bb1["bosses_won"]))
    check("scoreboard losses +1", bb1["losses"] == bb0["losses"] + 1,
          "losses %d -> %d" % (bb0["losses"], bb1["losses"]))
    if os.path.exists(BOSS_FEED):
        os.remove(BOSS_FEED)   # the defeat line must not leak onto a later
                               # stream's overlay (the box hides when absent)

    # kill switch: boss_enabled=0 AND test=0 (production shape) - the boss
    # query still answers (OFF) and !bossstart is not even a verb
    print("\n== boss kill switch ==")
    g5 = Game(cfg_text=TEST_CFG.replace("test=1", "test=0"),
              log_path=os.path.join(BASE, "harness_boss_off.log"))
    g5.start()
    try:
        g5.drop("bosshp||tester|0")
        expect("bosshp answers OFF when disabled", g5.results(),
               r"verb=bosshp state=OFF hp=0/0 ")
        g5.drop("bossstart||tester|0")
        expect("bossstart not admitted without test=1", g5.results(),
               r"verb=bossstart DROPPED reason=unknown$")
        check("kill switch: game stays up", g5.proc.poll() is None)
    finally:
        g5.stop()

    # ---- randomizer: seed nickname (randomizer.ini, CWD) ----
    # The main game must be fully stopped first: the game reads
    # randomizer.ini from its CWD, and both phases share this directory.
    print("\n== randomizer seed nickname ==")
    ini_saved = open(RANDO_INI, "rb").read() if os.path.exists(RANDO_INI) else None
    rlog_saved = open(RANDO_LOG, "rb").read() if os.path.exists(RANDO_LOG) else None
    try:
        with open(RANDO_INI, "w") as fh:
            fh.write("enabled=1\nseed=%d\nlog=1\n" % RANDO_SEED)
        nick1 = rando_boot(os.path.join(BASE, "harness_rando1.log"))
        check("randomizer names the seed", bool(nick1),
              nick1 or ("no [randomizer] banner for seed %d" % RANDO_SEED))
        nick2 = rando_boot(os.path.join(BASE, "harness_rando2.log"))
        check("seed nickname deterministic",
              bool(nick1) and nick1 == nick2, "%r vs %r" % (nick1, nick2))
    finally:
        # restore whatever the repo had (content preserved byte-for-byte);
        # never leave the randomizer enabled behind the harness
        if ini_saved is not None:
            with open(RANDO_INI, "wb") as fh:
                fh.write(ini_saved)
        elif os.path.exists(RANDO_INI):
            os.remove(RANDO_INI)
        if rlog_saved is not None:
            with open(RANDO_LOG, "wb") as fh:
                fh.write(rlog_saved)
        elif os.path.exists(RANDO_LOG):
            os.remove(RANDO_LOG)

    print("\n== %d passed, %d failed ==" % (len(PASS), len(FAIL)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())

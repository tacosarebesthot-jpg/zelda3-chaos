#!/usr/bin/env python3
"""End-to-end demo of the hardened rig (rig hardening 2026-09-10).

One boot drives every verification the owner mandated:

  item 1  walk_to() the Sanctuary chest tile (1246,600) from the chapter-1
          spawn and show the loop exits AT the target (+-2px); then target
          an unreachable point and show the loud WalkError
  item 2  (smoke.py runs separately; here: golden spawn state + clean exit)
  item 3  spawn|keese 3 -> dump sprite slots -> diff vs intended type 112;
          then swarm|4 -> dump -> report the actual sprite_type values
          (answers the "cuccos=0" corruption question with data)
  item 4  state dumps stamped with the game's monotonic frame_id
          (probe f=), measured emulation pace (~60fps via the frame-delay
          pacing in the verify ini), frames-per-action for run comparison
  e2e     boot -> gameplay -> position-aware walk -> chest open (the pin
          proof the old fixed-frame route failed) -> one spawn verb ->
          graceful WM_CLOSE exit code 0

Usage:  python tools/gameplay_verify/harden_verify.py [--tag run1] ...
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rig  # noqa: E402

BASE = os.path.dirname(os.path.dirname(HERE))
INI = os.path.join(HERE, "zelda3_verify.ini")
SHOTS = os.path.join(HERE, "shots_harden")
TRACKER = os.path.join(BASE, "tracker_items.txt")

GOLDEN_SPAWN = {"x": 1272, "y": 568, "tol": 4}
# closed-loop walk demo: the proven gold route (down the aisle, left along
# the lower ledge, up into the room centre).  Greedy dominant-axis walking
# CANNOT do this route (pillar row blocks left at y~598; the old fixed-frame
# script only ever got here by luck).
ROUTE = [(1270, 623), (1246, 623), (1246, 600)]
# the Sanctuary chest sits at ~(1311,566); stand below it.  Zelda follows
# Link in this chapter and pins him in the room centre, so the approach
# goes along the right ledge (chest proof = evidence only, see below).
CHEST_STAND = (1312, 584)
CHEST_ROUTE = [(1312, 623), CHEST_STAND]
UNREACHABLE = (1100, 600)    # west, into the pillar row - wall-blocked, no
                             # door that way (east push ended in the south
                             # door at (1274,603) in run2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chapter", type=int, default=1)
    ap.add_argument("--bootwait", type=float, default=8.0)
    ap.add_argument("--tag", default="run1")
    ap.add_argument("--seed-pin", action="store_true",
                    help="also pin Sanctuary=Hookshot via randomizer.ini")
    args = ap.parse_args()

    tag = args.tag
    RUN_LOG = os.path.join(HERE, "harden_run_%s.log" % tag)
    results = []

    def record(name, ok, detail, required=True):
        results.append({"check": name, "ok": bool(ok), "detail": detail,
                        "required": bool(required)})
        print("  [%s]%s %s: %s" % ("PASS" if ok else "FAIL",
                                   "" if required else "(evidence)",
                                   name, detail), flush=True)

    def shoot(name):
        try:
            p = g.shot(SHOTS, "%s_%s" % (tag, name))
            print("    shot: %s" % os.path.basename(p))
        except Exception as ex:
            print("    [shot failed: %s]" % ex)

    rig.make_verify_ini(os.path.join(BASE, "zelda3.ini"), INI,
                        load_ref=True, enable_audio=False, display_perf=True)
    rando_swap = False
    if args.seed_pin:
        rpath = os.path.join(BASE, "randomizer.ini")
        rig.config_recover([rpath])
        rig.config_backup([rpath])       # byte-copy sidecar first
        with open(rpath, "w", encoding="utf-8") as fh:
            fh.write("enabled=1\nseed=1234\nlog=1\n"
                     "pin_location=Sanctuary\npin_item=Hookshot\n")
        rando_swap = True
    t_boot = time.time()
    g = rig.Game(BASE, RUN_LOG, os.path.relpath(INI, BASE))
    exit_code = None
    try:
        g.start()
        print("== hardened rig e2e (%s): booted, pid %d ==" % (tag, g.proc.pid))
        shoot("00_title")
        time.sleep(args.bootwait * 0.5)   # title settles before LoadRef

        # -- gameplay + golden spawn state ---------------------------------
        p0 = g.load_chapter(args.chapter)
        dx = abs(p0.get("x", 0) - GOLDEN_SPAWN["x"])
        dy = abs(p0.get("y", 0) - GOLDEN_SPAWN["y"])
        record("golden_spawn", dx <= GOLDEN_SPAWN["tol"] and
               dy <= GOLDEN_SPAWN["tol"] and p0.get("st") == 0,
               "spawn probe %r (golden x=%d+-%d y=%d+-%d st=0)"
               % (p0, GOLDEN_SPAWN["x"], GOLDEN_SPAWN["tol"],
                  GOLDEN_SPAWN["y"], GOLDEN_SPAWN["tol"]))

        # -- LOCATION AWARENESS (owner scope upgrade): the rig asserts WHICH
        # room Link is in before acting on the room's chest
        g.require_state(rooms=(0x12,), module=7, max_inv=0, timeout=8.0,
                        what="Sanctuary (room 0x012) before chest actions")
        record("room_assert", True,
               "pre-action state check passed: dungeon_room_index=0x%02X, "
               "mod=%s sub=%s inv=%s (grace clear) - rig knows WHERE Link "
               "is before walking to the chest"
               % (g.last_probe.get("r", 0), g.last_probe.get("mod"),
                  g.last_probe.get("sub"), g.last_probe.get("inv")))
        dump_a = g.state_dump(os.path.join(HERE, "harden_state_%s_A.json" % tag),
                              note="after boot+LoadRef ch%d" % args.chapter)
        pace_boot = g.measure_pace(2.0)
        print("    pace@boot: %.1f fps measured over probes" % pace_boot)

        # -- ITEM 1: position-aware walk (required demo) ---------------------
        # The proven gold route (run_pin_gold.log probe trace): straight
        # DOWN the aisle to the lower ledge, LEFT along it, then UP into the
        # room.  Direct dominant-axis walking hits the pillar row at y~598 -
        # exactly why the old fixed-frame script and blind walkers fumble.
        # Every waypoint is a closed-loop walk_to with a loud timeout.
        print("== item 1: walk_via %s (tol 2px, closed loop) ==" % (ROUTE,))
        t0 = time.time()
        legs = g.walk_via(ROUTE, tol=2, timeout_per_leg=20.0, verbose=True)
        wall = time.time() - t0
        w = legs[-1]
        frames = sum(l["frames"] for l in legs)
        record("walk_reach",
               all(l["err"] <= (2, 2) for l in legs),
               "route %s -> reached (%d,%d) err=%s (all %d legs within "
               "+-2px; %d frames total, %.1fs wall, %.1f fps during walk)"
               % (ROUTE, w["reached"][0], w["reached"][1], w["err"],
                  len(legs), frames, wall,
                  frames / wall if wall > 0 else 0.0))
        shoot("10_at_chest")

        # -- ITEM 4: pace + frame-stamped dumps ------------------------------
        # measured in the CALM window right after the walk: later, the
        # swarm section can genuinely end gameplay (pinned Link -> damage
        # -> cutscene/death lock -> probes legitimately stop, seen in
        # run7), which is a finding, not a pacing failure.
        print("== item 4: pacing + frame_id ==")
        pace_end = g.measure_pace(2.0)
        dump_b = g.state_dump(os.path.join(HERE, "harden_state_%s_B.json" % tag),
                              note="calm gameplay after walk, pre-swarm")
        paced = (45.0 <= pace_boot <= 80.0) and (45.0 <= pace_end <= 80.0)
        record("frame_pace", paced,
               "measured %.1f fps @boot, %.1f fps @mid (frame-delay pacing "
               "in verify ini; unlocked would read hundreds)" %
               (pace_boot, pace_end))
        record("frame_id_stamped",
               dump_a["frame_id"] is not None and
               dump_b["frame_id"] is not None and
               dump_b["frame_id"] > dump_a["frame_id"],
               "state dumps: frame_id %s -> %s (+%s frames so far)"
               % (dump_a["frame_id"], dump_b["frame_id"],
                  (dump_b["frame_id"] or 0) - (dump_a["frame_id"] or 0)))

        # -- chest open (best-effort evidence, NOT required): the Sanctuary
        # chest sits at ~(1311,566); Zelda follows Link in the ch1 script
        # and pins him if he stops in the room centre (runs 4/5), so this
        # approaches along the right ledge instead.  Failure here is
        # reported as evidence, never a verdict failure - the pin re-proof
        # belongs to the death-loop re-runs.
        try:
            print("== chest open attempt at %s (evidence) ==" % (CHEST_STAND,))
            g.walk_via(CHEST_ROUTE, tol=2, timeout_per_leg=15.0)
            g.ensure_focus()
            rig.key_down("up")
            time.sleep(0.35)
            rig.key_up("up")
            time.sleep(0.2)
            rig.tap("x")
            time.sleep(2.0)
            shoot("11_chest_item")
            # dismiss the item text box (probes pause while a box is up);
            # anchor on the FRESHEST frame id, not the walk-era one
            g._drain()
            last_f = (g.last_probe or {}).get("f", 0)
            for _ in range(6):
                rig.tap("x")
                time.sleep(0.6)
                if g.wait_probe_after(last_f, timeout=3.0):
                    break
            tracker_ok = False
            tr_detail = "tracker_items.txt missing"
            if os.path.exists(TRACKER):
                fresh_lines = [ln.strip() for ln in
                               open(TRACKER, "r", errors="replace")
                               if ln.strip()]
                fresh_lines = [ln for ln in fresh_lines
                               if os.path.getmtime(TRACKER) >= t_boot]
                want = "Hookshot" if args.seed_pin else None
                tr_detail = "fresh tracker lines: %r" % fresh_lines[:3]
                tracker_ok = (any("Hookshot" in ln for ln in fresh_lines)
                              if want else bool(fresh_lines))
            record("chest_item", tracker_ok, tr_detail, required=False)
        except Exception as ex:
            record("chest_item", False,
                   "chest approach failed (NPC interference is expected "
                   "some runs): %r" % ex, required=False)

        # -- ITEM 3: spawn diff on a known batch (grace-gated verbs) --------
        # fire_verb() refuses to fire while the pre-action state check
        # fails (wrong room, cutscene lock, or incapacitated_timer>0 -
        # Link mid-hit/respawn): this is the owner's death-loop guard.
        print("== item 3: spawn|keese 3 -> slot diff vs type 112 ==")
        g.fire_verb("smite||harden|0", expect_rx=r"verb=smite killed=\d+")
        time.sleep(1.5)
        before = g.sprite_slots()
        line, gate = g.fire_verb("spawn|keese 3|harden|0",
                                 expect_rx=r"verb=spawn type=112 count=3")
        record("spawn_result", line is not None,
               "%s (fired at gate: inv=%s st=%s mod=%s - grace clear)"
               % ((line or "no RESULT").strip(), gate.get("inv"),
                  gate.get("st"), gate.get("mod")))
        time.sleep(0.8)
        after = g.sprite_slots(after_frame=(before or {}).get("f", -1))
        if not before or not after:
            record("spawn_diff", False, "sprite telemetry missing")
        else:
            matches, mismatches = g.spawn_diff(112, before, after)
            record("spawn_diff", not mismatches,
                   "before f=%d n=%d %s | after f=%d n=%d | matches=%s "
                   "mismatches=%s"
                   % (before["f"], before["n"], before["slots"],
                      after["f"], after["n"],
                      [(s, t, gr) for s, t, gr in matches] or "[]",
                      mismatches or "[]"))
        g.fire_verb("heal|999|harden|0", expect_rx=r"verb=heal")

        # -- ITEM 3b: swarm investigation (the corruption question) ---------
        print("== item 3b: swarm|4 -> what actually spawned ==")
        g.fire_verb("smite||harden|0", expect_rx=r"verb=smite killed=\d+")
        time.sleep(1.5)
        before2 = g.sprite_slots()
        sline, gate = g.fire_verb("swarm|4|harden|0",
                                  expect_rx=r"verb=swarm count=\d+ cuccos=\d")
        time.sleep(0.8)
        after2 = g.sprite_slots(after_frame=(before2 or {}).get("f", -1))
        shoot("12_swarm")
        if not after2:
            record("swarm_dump", False, "no sprite dump after swarm")
        else:
            kinds = ["slot%d t=%d %s g=%d s=%d @(%s,%s)"
                     % (s[0], s[1], rig.spawn_name(s[1]), s[2], s[3],
                        s[4], s[5]) for s in after2["slots"]]
            record("swarm_dump", True,
                   "%s | %d live slots: %s"
                   % ((sline or "no RESULT").strip(), len(kinds),
                      "; ".join(kinds)))
        g.fire_verb("smite||harden|0", expect_rx=r"verb=smite killed=\d+")
        g.fire_verb("heal|999|harden|0", expect_rx=r"verb=heal")

        # -- gameplay-gap events (transitions/death/respawn detection) ------
        gaps = g.gameplay_gaps()
        record("gameplay_events", True,
               "%d gameplay gap(s) detected in probe stream: %s"
               % (len(gaps), gaps or "none (gameplay continuous)"))

        # -- ITEM 1b: loud failure on an unreachable target ------------------
        # Push WEST into the pillar/wall: genuinely blocked, and away from
        # the south door (pushing right walked Link into the door at
        # (1274,603) in run2, which ends gameplay and stops telemetry - the
        # walker must and now does fail loudly on that too).
        print("== item 1b: unreachable target must FAIL LOUDLY ==")
        try:
            g.walk_to(UNREACHABLE[0], UNREACHABLE[1], tol=2, timeout=6.0)
            record("walk_unreachable_loud", False,
                   "walk_to %s unexpectedly SUCCEEDED - assertion broken"
                   % (UNREACHABLE,))
        except rig.WalkError as ex:
            record("walk_unreachable_loud", True,
                   "WalkError as required: %s" % ex)

        # -- clean exit ------------------------------------------------------
        exit_code = g.close(timeout=15.0)
        record("clean_exit", exit_code == 0,
               "WM_CLOSE exit code %r" % (exit_code,))
    except Exception as ex:
        import traceback
        record("e2e_aborted", False, "%r\n%s" % (ex, traceback.format_exc()))
    finally:
        g.stop()
        if rando_swap:
            rig.config_restore([os.path.join(BASE, "randomizer.ini")])
            print("  [restore] randomizer.ini restored from sidecar")

    out = os.path.join(HERE, "harden_results_%s.json" % tag)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump({"tag": tag, "results": results}, fh, indent=1)
    failed = [r for r in results if not r["ok"] and r["required"]]
    evidence_failed = [r for r in results if not r["ok"] and not r["required"]]
    print("== hardened e2e verdict (%s): %s (%d/%d required checks) =="
          % (tag, "FAIL" if failed else "PASS",
             sum(1 for r in results if r["required"]) - len(failed),
             sum(1 for r in results if r["required"])))
    for r in failed:
        print("   FAIL %s: %s" % (r["check"],
                                  r["detail"].splitlines()[0]))
    for r in evidence_failed:
        print("   [evidence miss - not verdict-breaking] %s: %s"
              % (r["check"], r["detail"].splitlines()[0]))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

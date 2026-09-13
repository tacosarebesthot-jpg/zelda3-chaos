#!/usr/bin/env python3
"""text_season.py -- DECORATIVE-ONLY dialogue seasoning pipeline for zelda3.

Pipeline (extract -> edit -> recompress -> repack -> verify):
  1. The English dialogue lives in `assets/dialogue.txt` (one `N: text` line per
     game message, 1-based N; the engine's message index is N-1). It was
     extracted from the US ROM with `python assets/restool.py --extract-dialogue`.
  2. `python assets/restool.py` recompresses every line with the game's
     dictionary/alphabet scheme (assets/text_compression.py), packs all 397
     messages into the `kDialogue` asset (asset #94) with auto-generated
     offsets, and rebuilds `zelda3_assets.dat` (everything else comes from the
     US ROM). The build is byte-reproducible: with dialogue.txt untouched it
     reproduces the original dat exactly.
  3. The engine reads messages size-delimited (no end-byte inside the packed
     data; src/messaging.c Text_LoadCharacterBuffer appends 0x7f itself), so
     message LENGTH changes are fully handled by the recompressor. Nothing
     else in the dat depends on message lengths.

GUARDRAIL: only decorative text (signs / filler NPC one-liners) may be edited.
Every edit must be listed in tools/seasonings.json with the message index, the
exact expected original line, and a written justification. Any index not in
that file is refused. Hint dialogue, item descriptions, quest-critical text,
menus and anything load-bearing must never be added there.

Usage:
  python tools/text_season.py show              # show live dat text for each placement
  python tools/text_season.py apply             # backup, edit dialogue.txt, rebuild, verify
  python tools/text_season.py verify            # re-verify an already-applied dat
  python tools/text_season.py restore           # restore zelda3_assets.dat from the backup
"""

import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")
DIALOGUE_TXT = os.path.join(ASSETS, "dialogue.txt")
DAT_PATH = os.path.join(ROOT, "zelda3_assets.dat")
BACKUP_PATH = os.path.join(HERE, "zelda3_assets.dat.orig")
PLACEMENTS_PATH = os.path.join(HERE, "seasonings.json")

sys.path.insert(0, ASSETS)
import text_compression as tc  # noqa: E402

SIG = bytes([90, 101, 108, 100, 97, 51, 95, 118, 48, 32, 32, 32, 32, 32, 10, 0,
             27, 174, 233, 45, 74, 174, 252, 50, 49, 27, 153, 197, 27, 43, 216,
             197, 132, 101, 173, 169, 36, 108, 15, 155, 176, 169, 57, 131, 174,
             101, 51, 207])
NUM_ASSETS = 165
K_DIALOGUE = 94  # src/assets.h: #define kDialogue(idx) FindInAssetArray(94, idx)

MAX_PLAIN_LEN = 60  # one short message line; keep it decorative-sized


def fail(msg):
    print("ERROR: %s" % msg)
    sys.exit(1)


def load_placements():
    with open(PLACEMENTS_PATH, encoding="utf8") as f:
        data = json.load(f)
    placements = {}
    for p in data["placements"]:
        msg = p["msg"]
        if msg in placements:
            fail("duplicate placement for message index %s" % msg)
        placements[msg] = p
    return data, placements


# ---------------------------------------------------------------- dat parsing

def parse_dat(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 88 + NUM_ASSETS * 4 or data[:48] != SIG:
        fail("%s does not look like a zelda3 assets file" % path)
    extra = struct.unpack_from("<I", data, 84)[0]
    sizes = struct.unpack_from("<%dI" % NUM_ASSETS, data, 88)
    off = 88 + NUM_ASSETS * 4 + extra
    assets = []
    for i in range(NUM_ASSETS):
        off = (off + 3) & ~3
        assets.append(data[off:off + sizes[i]])
        off += sizes[i]
    return data, assets


def unpack_packed(blk):
    """Python port of src/util.c FindIndexInMemblk, all elements at once."""
    if len(blk) < 2:
        fail("packed block too small")
    end = len(blk) - 2
    mx = struct.unpack_from("<H", blk, end)[0]
    parts = []
    if mx < 8192:
        if mx * 2 > end:
            fail("packed block corrupt (16-bit)")
        for i in range(mx + 1):
            left = mx * 2 if i == 0 else mx * 2 + struct.unpack_from("<H", blk, i * 2 - 2)[0]
            right = end if i == mx else mx * 2 + struct.unpack_from("<H", blk, i * 2)[0]
            parts.append(blk[left:right])
    else:
        mx -= 8192
        if mx * 4 > end:
            fail("packed block corrupt (32-bit)")
        for i in range(mx + 1):
            left = mx * 4 if i == 0 else mx * 4 + struct.unpack_from("<I", blk, i * 4 - 4)[0]
            right = end if i == mx else mx * 4 + struct.unpack_from("<I", blk, i * 4)[0]
            parts.append(blk[left:right])
    return parts


def get_dialogue_msgs(assets):
    """Returns (dict_blks, [compressed_msg_bytes]) for language 0 (us)."""
    langs = unpack_packed(assets[K_DIALOGUE])
    if len(langs) < 1:
        fail("kDialogue has no languages")
    dict_blk, dialog_blk = unpack_packed(langs[0])[:2]
    return unpack_packed(dict_blk), unpack_packed(dialog_blk)


def decode_message(buf, dict_blks):
    """Decode a compressed message back to dialogue.txt text syntax."""
    info = tc.kLanguages["us"]
    s, i = "", 0
    while i < len(buf):
        c = buf[i]
        i += 1
        if c >= info.DICT_BASE_DEC:
            s += decode_message(dict_blks[c - info.DICT_BASE_DEC], [])
            continue
        if c == 0x7F:  # EndMessage
            break
        l = info.command_lengths[c - info.COMMAND_START] \
            if info.COMMAND_START <= c < info.SWITCH_BANK else 1
        if c < info.COMMAND_START:
            s += info.alphabet[c]
        elif l == 2:
            s += "[%s %.2d]" % (info.command_names[c - info.COMMAND_START], buf[i])
            i += 1
        else:
            s += "[%s]" % info.command_names[c - info.COMMAND_START]
    return s


def load_dialogue_lines():
    with open(DIALOGUE_TXT, encoding="utf8") as f:
        lines = f.read().splitlines()
    parsed = []
    for n, line in enumerate(lines, 1):
        a, b = line.split(": ", 1)
        assert int(a) == n, "dialogue.txt numbering broken at line %d" % n
        parsed.append(b)
    return parsed


# ---------------------------------------------------------------- guardrails

def validate_new_text(msg, p):
    t = p["new_text"]
    if not t:
        fail("msg %s: empty replacement" % msg)
    if len(t) > MAX_PLAIN_LEN:
        fail("msg %s: replacement too long (%d > %d)" % (msg, len(t), MAX_PLAIN_LEN))
    if "[" in t or "]" in t:
        fail("msg %s: control codes ([...]) are not allowed in replacements" % msg)
    allowed = set(tc.kTextAlphabet_US) | {" "}
    bad = sorted(set(t) - allowed)
    if bad:
        fail("msg %s: characters not in the US text alphabet: %r" % (msg, "".join(bad)))
    if not p.get("why_decorative"):
        fail("msg %s: missing why_decorative justification" % msg)


# ---------------------------------------------------------------- commands

def cmd_show():
    _, placements = load_placements()
    _, assets = parse_dat(DAT_PATH)
    dict_blks, msgs = get_dialogue_msgs(assets)
    for msg in sorted(placements):
        if not 0 <= msg < len(msgs):
            fail("placement msg index %s out of range" % msg)
        print("--- msg %d (dialogue.txt line %d)" % (msg, msg + 1))
        print("  now: %s" % decode_message(msgs[msg], dict_blks))
        print("  new: %s" % placements[msg]["new_text"])


def cmd_apply():
    doc, placements = load_placements()
    for msg, p in placements.items():
        validate_new_text(msg, p)

    if not os.path.exists(DAT_PATH):
        fail("%s not found" % DAT_PATH)

    # 1. Backup the original dat (first run only; never overwrite a backup).
    if os.path.exists(BACKUP_PATH):
        print("backup already exists: %s (keeping it)" % BACKUP_PATH)
    else:
        shutil.copy2(DAT_PATH, BACKUP_PATH)
        print("backup written: %s" % BACKUP_PATH)

    # 2. Edit assets/dialogue.txt in place.
    lines = load_dialogue_lines()
    changed, already = [], []
    for msg, p in sorted(placements.items()):
        if not 0 <= msg < len(lines):
            fail("placement msg index %s out of range" % msg)
        cur = lines[msg]
        if cur == p["expected_original"]:
            lines[msg] = p["new_text"]
            changed.append(msg)
        elif cur == p["new_text"]:
            already.append(msg)
        else:
            fail("msg %s: dialogue.txt drifted from expected original:\n"
                 "  expected: %r\n  actual:   %r" % (msg, p["expected_original"], cur))
    print("edits: %d applied, %d already in place" % (len(changed), len(already)))
    with open(DIALOGUE_TXT, "w", encoding="utf8", newline="") as f:
        f.write("\n".join("%d: %s" % (n, t) for n, t in enumerate(lines, 1)) + "\n")

    # 3. Rebuild zelda3_assets.dat (restool recompresses + repacks kDialogue).
    print("rebuilding zelda3_assets.dat ...")
    r = subprocess.run([sys.executable, os.path.join(ASSETS, "restool.py")],
                       cwd=ASSETS, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        fail("restool.py failed; restore with: python tools/text_season.py restore")
    if r.stdout:
        print(r.stdout.strip().splitlines()[-1])

    # 4. Verify the new dat.
    verify_dat(placements)


def verify_dat(placements):
    _, old_assets = parse_dat(BACKUP_PATH)
    _, new_assets = parse_dat(DAT_PATH)
    _, old_msgs = get_dialogue_msgs(old_assets)
    new_dict, new_msgs = get_dialogue_msgs(new_assets)
    if len(old_msgs) != len(new_msgs):
        fail("message count changed: %d -> %d" % (len(old_msgs), len(new_msgs)))
    problems = 0
    touched = 0
    for i, (a, b) in enumerate(zip(old_msgs, new_msgs)):
        if i in placements:
            touched += 1
            want = placements[i]["new_text"]
            got = decode_message(b, new_dict)
            # byte-exact round trip through the stock recompressor
            recomp = tc.compress_strings([got], "us")[0]
            if got != want:
                print("FAIL msg %d decoded %r != intended %r" % (i, got, want))
                problems += 1
            elif bytes(recomp) != bytes(b):
                print("FAIL msg %d does not round-trip through the recompressor" % i)
                problems += 1
        elif bytes(a) != bytes(b):
            print("FAIL msg %d is untouched but its bytes changed" % i)
            problems += 1
    diff_regions = [i for i in range(NUM_ASSETS)
                    if bytes(old_assets[i]) != bytes(new_assets[i])]
    if diff_regions != [K_DIALOGUE]:
        print("FAIL assets other than kDialogue changed: %s" % diff_regions)
        problems += 1
    if problems:
        fail("%d verification problem(s); restore with: python tools/text_season.py restore"
             % problems)
    print("OK: %d/%d messages byte-identical, %d seasoned messages decode + round-trip "
          "cleanly, only asset %d (kDialogue) differs"
          % (len(old_msgs) - touched, len(old_msgs), touched, K_DIALOGUE))


def cmd_verify():
    _, placements = load_placements()
    for msg, p in placements.items():
        validate_new_text(msg, p)
    if not os.path.exists(BACKUP_PATH):
        fail("no backup at %s; nothing to compare against" % BACKUP_PATH)
    lines = load_dialogue_lines()
    for msg, p in sorted(placements.items()):
        if lines[msg] != p["new_text"]:
            fail("msg %d: dialogue.txt does not contain the seasoned text" % msg)
    verify_dat(placements)


def cmd_restore():
    if not os.path.exists(BACKUP_PATH):
        fail("no backup at %s" % BACKUP_PATH)
    shutil.copy2(BACKUP_PATH, DAT_PATH)
    print("restored %s from %s" % (DAT_PATH, BACKUP_PATH))
    print("NOTE: assets/dialogue.txt still contains the seasoned lines; run "
          "`git checkout` / re-extract (`python assets/restool.py --extract-dialogue`) "
          "to revert it too.")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
    if cmd == "show":
        cmd_show()
    elif cmd == "apply":
        cmd_apply()
    elif cmd == "verify":
        cmd_verify()
    elif cmd == "restore":
        cmd_restore()
    else:
        fail("unknown command %r (use show|apply|verify|restore)" % cmd)


if __name__ == "__main__":
    main()

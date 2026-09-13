#!/usr/bin/env python3
"""Offline lint for dialogue.txt: the same rules as the game's tokenizer.
Flags unknown [codes], characters the 8x16 dialogue font lacks, and counts
alternates.  Usage: python tools/dialogue_lint.py [path]"""
import re, sys, os
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dialogue.txt")
OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!?-.,>()\"' <:;_/`|")
CODES = {"Scroll", "Waitkey", "1", "2", "3", "Name", "Wait", "Color", "Number", "Speed", "Sound",
         "Choose", "Choose2", "Choose3", "Selchg", "Item", "NextPic", "Window", "Position",
         "...", "Ankh", "Waves", "Snake", "LinkL", "LinkR", "Up", "Down", "Left", "Right",
         "1HeartL", "1HeartR", "2HeartL", "3HeartL", "3HeartR", "4HeartL", "4HeartR", "A", "B", "X", "Y"}
n = 0; alts = {}; problems = 0; dirty = 0
DIRTY = re.compile(r"\b(fuck\w*|shit\w*|bullshit|ass|asses|badass|bitch\w*|damn|dammit|goddamn|cunt|dick|cock|pussy|whore\w*|tits|nowyafuckedup|fucterbud)\b", re.I)
for ln in open(path, encoding="utf-8"):
    m = re.match(r"(\d+)([hxrHXR]*): (.*)", ln.rstrip("\r\n"))
    if not m:
        continue
    n += 1; alts[m.group(1)] = alts.get(m.group(1), 0) + 1
    t = m.group(3)
    if "x" not in m.group(2).lower() and DIRTY.search(t):
        print("untagged cursing in %s (the game treats it as dirty anyway)" % m.group(1))
    if "x" in m.group(2).lower(): dirty += 1
    for c in re.findall(r"\[([^\]]*)\]", t):
        if c.split(" ")[0] not in CODES:
            print("unknown code in %s: [%s]" % (m.group(1), c)); problems += 1
    body = re.sub(r"\[[^\]]*\]|\{chatter\}|\{boss\}", "", t)
    bad = sorted(set(ch for ch in body if ch not in OK))
    if bad:
        print("font lacks %r in %s" % ("".join(bad), m.group(1))); problems += 1
    if len(t.encode("utf-8")) > 1900:   # the engine buffer is 2048 bytes
        print("too long (%d bytes) in %s" % (len(t), m.group(1))); problems += 1
print("%d lines (%d dirty), %d numbers, %d with alternates (max %d per number), %d problems" %
      (n, dirty, len(alts), sum(1 for v in alts.values() if v > 1), max(alts.values()) if alts else 0, problems))
sys.exit(1 if problems else 0)

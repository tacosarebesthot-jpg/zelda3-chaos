#!/usr/bin/env python3
"""Farm the stream chat for the joke text: chatter counts + quotable lines.

Inputs: VOD chat JSONs (list of {off,user,msg}) from the Z2 project's
get_chat.py (a folder of *_chat.json files).
Outputs: briefs/CHAT_FARM_<date>.md (ranked quotes) and merges new chatter
names (>= MIN_MSGS messages) into chatters.txt without reordering it.

Usage: python tools/chat_farm.py [--vods DIR] [--min 5]
New VOD: python get_chat.py <video_id> > vodN_lane_<date>_chat.json first.
"""
import argparse, collections, datetime, glob, json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__)); BASE = os.path.dirname(HERE)
PROJ = os.path.dirname(os.path.dirname(BASE))
BOTS = {"streamelements", "nightbot", "moobot", "fossabot", "wizebot"}
KEYS = r"\b(lol|lmao|omg|chicken|cucco|kill|die|died|dead|chat|cheat|boss|princess|zelda|link|dave|hookshot|bomb|rupee|key|chest|slow|heal|hurt|swarm|freeze|arise|meatwad|carl|ricky|bubbles|snake|madden|duck|phone|mod|sub)\b"
ap = argparse.ArgumentParser()
ap.add_argument("--vods", default=os.path.join(PROJ, "_tools", "vods"))
ap.add_argument("--min", type=int, default=5)
ap.add_argument("--streamer", default="streamer")
a = ap.parse_args()
msgs = []
for f in sorted(glob.glob(os.path.join(a.vods, "*_chat.json"))):
    for m in json.load(open(f, encoding="utf-8")):
        msgs.append((os.path.basename(f), m.get("user") or "", m.get("msg") or ""))
for f in glob.glob(os.path.join(PROJ, "_tools", "discord", "full_*.json")):
    try:
        for m in json.load(open(f, encoding="utf-8")):
            if isinstance(m, dict) and not m.get("bot"):
                msgs.append((os.path.basename(f), str(m.get("author") or ""), m.get("content") or ""))
    except Exception:
        pass
skip = BOTS | {a.streamer.lower()}
count = collections.Counter(u for _, u, _ in msgs if u and u.lower() not in skip)
seen = collections.Counter()
for _, u, t in msgs:
    t = t.strip()
    if u and u.lower() not in skip and not t.startswith("!") and "http" not in t and 18 <= len(t) <= 110:
        seen[t.lower()] += 1
def score(t):
    s = min(seen[t.lower()], 3) * 2 + sum(ch.isupper() for ch in t) / max(1, len(t)) * 4
    s += t.count("!") + t.count("?") * 0.5 + (2 if re.search(KEYS, t.lower()) else 0)
    return s
uniq = {}
for _, u, t in msgs:
    t = t.strip()
    if not u or u.lower() in skip or t.startswith("!") or "http" in t or not (18 <= len(t) <= 110):
        continue
    if t.lower() not in uniq or score(t) > score(uniq[t.lower()][1]):
        uniq[t.lower()] = (u, t)
ranked = sorted(uniq.values(), key=lambda x: -score(x[1]))[:150]
day = datetime.date.today().isoformat()
out = os.path.join(PROJ, "briefs", "CHAT_FARM_%s.md" % day)
with open(out, "w", encoding="utf-8") as fh:
    fh.write("# Chat farm %s (%d messages)\n\n## Chatters (excl. streamer + bots)\n" % (day, len(msgs)))
    fh.write("\n".join("- %s (%d)" % (u, n) for u, n in count.most_common(60)))
    fh.write("\n\n## Quote candidates (ranked)\n" + "\n".join("- **%s**: %s" % (u, t) for u, t in ranked) + "\n")
# merge chatters.txt (case-insensitive, append only)
ch = os.path.join(BASE, "chatters.txt")
lines = open(ch, encoding="utf-8").read().splitlines() if os.path.exists(ch) else ["# chatters seen in your streams"]
have = {l.strip().lower() for l in lines if l.strip() and not l.startswith("#")}
new = [u for u, n in count.most_common() if n >= a.min and u.lower() not in have]
if new:
    with open(ch, "a", encoding="utf-8") as fh:
        fh.write("\n".join(new) + "\n")
print("wrote", out, "|", len(ranked), "quotes |", len(new), "new chatters:", new)

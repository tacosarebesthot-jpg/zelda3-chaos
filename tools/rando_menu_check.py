#!/usr/bin/env python3
"""Static check for the RANDOMIZER page (player select > new file > RANDOMIZER):
kRandoOpts in src/randomizer.c against src/select_file.c's real printf widths,
against the reference randomizer's own resources/app/cli/args.json, and
against what randomizer_ref/dump_logic.py actually writes to disk. Pure
static analysis - no game launch, no python randomizer_ref import.

Checks:
  row_count          #define kRandoOptCount equals len(kRandoOpts)
  row_order          BOSSES is the last row, ENTRANCES second-to-last, and
                      kBossOptIndex/kEntranceOptIndex agree
  row_arity          each row's n equals len(vals) and len(names)
  label_len          label <= the real snprintf width in select_file.c
  name_len           every entry of names <= the real snprintf width
  desc_len           d1 and d2 <= the real snprintf width
  refkey_exists      every refkey is in args.json (documented exceptions only)
  refkey_boolflag    boolflag==1 iff args.json marks the refkey store_true
                      (checked only for rows RulesBuild actually emits - a
                      presentation row's boolflag is never read at dump time)
  refkey_choices     every val is one of args.json's choices for that refkey
                      (rows without an args.json refkey are skipped here)
  rulesbuild_skips_presentation
                      RulesBuild's emission loop is gated on
                      "opt[o] && !kRandoOpts[o].presentation"
  rulefiles_match_dump
                      kRuleFiles == the files dump_world()/run_seed() in
                      dump_logic.py write unconditionally
  logic_folder_coverage
                      each randomizer_ref/out/logic_* folder either has every
                      kRuleFiles entry or is reported as "rebuild on demand"
                      (informational - an incomplete folder is a known, open
                      audit finding, not a failure of this check)

Run: python_embed\\python.exe tools\\rando_menu_check.py
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RANDOMIZER_C = os.path.join(ROOT, "src", "randomizer.c")
SELECT_FILE_C = os.path.join(ROOT, "src", "select_file.c")
ARGS_JSON = os.path.join(ROOT, "randomizer_ref", "ALttPDoorRandomizer",
                          "resources", "app", "cli", "args.json")
DUMP_LOGIC_PY = os.path.join(ROOT, "randomizer_ref", "dump_logic.py")
OUT_DIR = os.path.join(ROOT, "randomizer_ref", "out")

FAILURES = []
INFO = []


def fail(msg):
    FAILURES.append(msg)


def info(msg):
    INFO.append(msg)


def read(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# src/randomizer.c
# ---------------------------------------------------------------------------
def parse_randomizer_c(src):
    m = re.search(r"#define\s+kRandoOptCount\s+(\d+)", src)
    if not m:
        fail("could not find #define kRandoOptCount in src/randomizer.c")
        return None
    opt_count = int(m.group(1))

    m = re.search(r"#define\s+kBossOptIndex\s+\(kRandoOptCount\s*-\s*(\d+)\)", src)
    boss_offset = int(m.group(1)) if m else None
    if boss_offset is None:
        fail("could not find #define kBossOptIndex (kRandoOptCount - N) in src/randomizer.c")

    m = re.search(r"#define\s+kEntranceOptIndex\s+\(kRandoOptCount\s*-\s*(\d+)\)", src)
    entrance_offset = int(m.group(1)) if m else None
    if entrance_offset is None:
        fail("could not find #define kEntranceOptIndex (kRandoOptCount - N) in src/randomizer.c")

    # value/name arrays: static const char *const kV_xxx[] = {...};
    arrays = {}
    for am in re.finditer(
            r'static const char \*const (k[VN]_\w+)\[\]\s*=\s*\{([^}]*)\};',
            src):
        name = am.group(1)
        body = am.group(2)
        vals = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
        arrays[name] = vals

    # the kRandoOpts table itself
    tm = re.search(
        r'static const RandoOpt kRandoOpts\[kRandoOptCount\]\s*=\s*\{(.*?)\n\};',
        src, re.S)
    if not tm:
        fail("could not find the kRandoOpts table in src/randomizer.c")
        return None
    table_body = tm.group(1)

    row_re = re.compile(
        r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*'
        r'(k[VN]_\w+)\s*,\s*(k[VN]_\w+)\s*,\s*(\d+)\s*,\s*'
        r'"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"'
        r'(?:\s*,\s*(\d+))?(?:\s*,\s*(\d+))?\s*\}')
    rows = []
    for rm in row_re.finditer(table_body):
        (inikey, refkey, label, vals_name, names_name, n, d1, d2,
         boolflag, presentation) = rm.groups()
        rows.append({
            "inikey": inikey, "refkey": refkey, "label": label,
            "vals_name": vals_name, "names_name": names_name, "n": int(n),
            "d1": d1, "d2": d2,
            "boolflag": int(boolflag) if boolflag is not None else 0,
            "presentation": int(presentation) if presentation is not None else 0,
        })

    # RulesBuild's emission loop + the presentation gate
    bm = re.search(r'static int RulesBuild\(.*?\n\}\n', src, re.S)
    rulesbuild_body = bm.group(0) if bm else ""

    # kRuleFiles
    fm = re.search(r'static const char \*const kRuleFiles\[\]\s*=\s*\{([^}]*)\};', src)
    rule_files = re.findall(r'"([^"]+)"', fm.group(1)) if fm else []
    if not fm:
        fail("could not find kRuleFiles in src/randomizer.c")

    return {
        "opt_count": opt_count, "boss_offset": boss_offset,
        "entrance_offset": entrance_offset, "arrays": arrays, "rows": rows,
        "rulesbuild_body": rulesbuild_body, "rule_files": rule_files,
    }


# ---------------------------------------------------------------------------
# src/select_file.c: the real printf widths for the RANDOMIZER page
# ---------------------------------------------------------------------------
def parse_select_file_widths(src):
    widths = {}
    m = re.search(r'%-(\d+)\.\d+s"\s*,\s*kind == kRPKind_Begin', src)
    if m:
        widths["label"] = int(m.group(1))
    else:
        fail("select_file.c: could not find the label snprintf width (kind == kRPKind_Begin ? ... : label)")
    m = re.search(r'%-(\d+)\.\d+s"\s*,\s*val\)', src)
    if m:
        widths["name"] = int(m.group(1))
    else:
        fail("select_file.c: could not find the value/name snprintf width (\"%-N.Ns\", val)")
    m1 = re.search(r'%-(\d+)\.\d+s"\s*,\s*d1\)', src)
    m2 = re.search(r'%-(\d+)\.\d+s"\s*,\s*d2\)', src)
    if m1 and m2:
        if m1.group(1) != m2.group(1):
            fail("select_file.c: d1 and d2 snprintf widths disagree (%s vs %s)" % (m1.group(1), m2.group(1)))
        widths["desc"] = int(m1.group(1))
    else:
        fail("select_file.c: could not find the d1/d2 snprintf widths")
    return widths


# ---------------------------------------------------------------------------
# randomizer_ref/dump_logic.py: the files it writes unconditionally
# ---------------------------------------------------------------------------
def parse_dump_logic_files(src):
    files = set()

    # dump_world()'s six-file loop
    m = re.search(r'def dump_world\(.*?for fname, data in \[(.*?)\]:', src, re.S)
    if m:
        files.update(re.findall(r'"([\w.]+\.json)"', m.group(1)))
    else:
        fail("dump_logic.py: could not find dump_world()'s fname/data list")

    # the five standalone dump_* helpers, each unconditionally writing one
    # file at their function's own indent level (never inside an `if`)
    for helper in ("dump_bosses", "dump_drops", "dump_pots", "dump_bonks", "dump_entrances"):
        fm = re.search(r'\ndef %s\(.*?(?=\ndef |\Z)' % re.escape(helper), src, re.S)
        if not fm:
            fail("dump_logic.py: could not find def %s(...)" % helper)
            continue
        body = fm.group(0)
        wm = re.search(r'\n( *)with open\(os\.path\.join\(outdir, "([\w.]+\.json)"\)', body)
        if not wm:
            fail("dump_logic.py: %s() does not write a single unconditional <name>.json" % helper)
            continue
        indent = wm.group(1)
        if len(indent) != 4:
            fail("dump_logic.py: %s()'s json write is indented %d spaces (conditional?), expected 4"
                 % (helper, len(indent)))
        files.add(wm.group(2))

    # run_seed() must call all five unconditionally (base body indent, no
    # surrounding `if`), same as dump_world()
    rm = re.search(r'\ndef run_seed\(.*?(?=\ndef |\Z)', src, re.S)
    if rm:
        body = rm.group(0)
        for helper, var in (("dump_bosses", "bosses"), ("dump_drops", "drops"),
                            ("dump_pots", "pots"), ("dump_entrances", "entrances"),
                            ("dump_bonks", "bonks")):
            cm = re.search(r'\n( *)%s = %s\(' % (var, helper), body)
            if not cm:
                fail("dump_logic.py: run_seed() does not call %s() unconditionally" % helper)
            elif len(cm.group(1)) != 4:
                fail("dump_logic.py: run_seed()'s call to %s() is indented %d spaces (conditional?), expected 4"
                     % (helper, len(cm.group(1))))
    else:
        fail("dump_logic.py: could not find def run_seed(...)")

    return files


# ---------------------------------------------------------------------------
# checks
# ---------------------------------------------------------------------------
# ENEMY COLOR (refkey enemy_palette) has no reference setting at all - the
# reference's ow_palettes/uw_palettes are BACKGROUND palettes, never enemies
# (see the ENEMIES/ENEMY COLOR comment block above kRandoOpts in
# src/randomizer.c). It is presentation-only, so RulesBuild never sends it -
# this is the one documented exception to "every refkey exists in args.json".
ALLOWED_MISSING_REFKEY = {"enemy_palette"}


def check_rows(parsed, args_json):
    rows = parsed["rows"]
    opt_count = parsed["opt_count"]
    arrays = parsed["arrays"]

    if len(rows) != opt_count:
        fail("row_count: kRandoOptCount=%d but parsed %d rows out of kRandoOpts" % (opt_count, len(rows)))
    else:
        info("row_count: kRandoOptCount=%d matches %d parsed rows" % (opt_count, len(rows)))

    if not rows:
        return

    if rows[-1]["inikey"] != "bosses":
        fail("row_order: last row is %r, expected 'bosses'" % rows[-1]["inikey"])
    if rows[-2]["inikey"] != "entrances":
        fail("row_order: second-to-last row is %r, expected 'entrances'" % rows[-2]["inikey"])
    if parsed["boss_offset"] != 1:
        fail("row_order: kBossOptIndex is (kRandoOptCount - %s), expected (kRandoOptCount - 1)" % parsed["boss_offset"])
    if parsed["entrance_offset"] != 2:
        fail("row_order: kEntranceOptIndex is (kRandoOptCount - %s), expected (kRandoOptCount - 2)" % parsed["entrance_offset"])
    if not FAILURES or all("row_order" not in f for f in FAILURES):
        info("row_order: BOSSES last, ENTRANCES second-to-last, kBossOptIndex/kEntranceOptIndex agree")

    for r in rows:
        vals = arrays.get(r["vals_name"])
        names = arrays.get(r["names_name"])
        if vals is None:
            fail("row_arity[%s]: %s is not a known kV_/kN_ array" % (r["inikey"], r["vals_name"]))
            continue
        if names is None:
            fail("row_arity[%s]: %s is not a known kV_/kN_ array" % (r["inikey"], r["names_name"]))
            continue
        if len(vals) != r["n"]:
            fail("row_arity[%s]: n=%d but %s has %d entries" % (r["inikey"], r["n"], r["vals_name"], len(vals)))
        if len(names) != r["n"]:
            fail("row_arity[%s]: n=%d but %s has %d entries" % (r["inikey"], r["n"], r["names_name"], len(names)))


def check_lengths(parsed, widths):
    if not widths:
        return
    label_max = widths.get("label")
    name_max = widths.get("name")
    desc_max = widths.get("desc")
    for r in parsed["rows"]:
        if label_max is not None and len(r["label"]) > label_max:
            fail("label_len[%s]: label %r is %d chars, page draws it at %-d.%ds"
                 % (r["inikey"], r["label"], len(r["label"]), label_max, label_max))
        if desc_max is not None:
            if len(r["d1"]) > desc_max:
                fail("desc_len[%s]: d1 %r is %d chars, page draws it at %d.%ds"
                     % (r["inikey"], r["d1"], len(r["d1"]), desc_max, desc_max))
            if len(r["d2"]) > desc_max:
                fail("desc_len[%s]: d2 %r is %d chars, page draws it at %d.%ds"
                     % (r["inikey"], r["d2"], len(r["d2"]), desc_max, desc_max))
        if name_max is not None:
            names = parsed["arrays"].get(r["names_name"], [])
            for nm in names:
                if len(nm) > name_max:
                    fail("name_len[%s]: %s entry %r is %d chars, page draws it at %d.%ds"
                         % (r["inikey"], r["names_name"], nm, len(nm), name_max, name_max))
    if not any(f.startswith(("label_len", "desc_len", "name_len")) for f in FAILURES):
        info("lengths: every label<=%s, name<=%s, d1/d2<=%s (widths read from select_file.c)"
             % (label_max, name_max, desc_max))


def check_refkeys(parsed, args_json):
    for r in parsed["rows"]:
        refkey = r["refkey"]
        entry = args_json.get(refkey)
        if entry is None:
            if refkey not in ALLOWED_MISSING_REFKEY:
                fail("refkey_exists[%s]: refkey %r is not in args.json and is not a documented exception"
                     % (r["inikey"], refkey))
            continue
        is_store_true = entry.get("action") == "store_true"
        # RulesBuild only ever reads boolflag for a row it actually emits
        # (opt[o] && !presentation); a presentation row's boolflag is inert,
        # so only enforce the store_true<->boolflag equivalence for rows the
        # dump command line can actually receive.
        if not r["presentation"]:
            if is_store_true and not r["boolflag"]:
                fail("refkey_boolflag[%s]: args.json marks %r store_true but boolflag=0"
                     % (r["inikey"], refkey))
            if r["boolflag"] and not is_store_true:
                fail("refkey_boolflag[%s]: boolflag=1 but args.json does not mark %r store_true"
                     % (r["inikey"], refkey))
        if is_store_true:
            continue  # value strings only ever reach the ini files
        choices = entry.get("choices")
        if choices is None:
            fail("refkey_choices[%s]: args.json entry for %r has neither store_true nor choices"
                 % (r["inikey"], refkey))
            continue
        vals = parsed["arrays"].get(r["vals_name"], [])
        bad = [v for v in vals if v not in choices]
        if bad:
            fail("refkey_choices[%s]: %s has values not in args.json %s choices: %s"
                 % (r["inikey"], r["vals_name"], refkey, bad))
    if not any(f.startswith(("refkey_exists", "refkey_boolflag", "refkey_choices")) for f in FAILURES):
        info("refkeys: every row's refkey/boolflag/choices check out against args.json (enemy_palette exempted)")


def check_rulesbuild_skips_presentation(parsed):
    body = parsed["rulesbuild_body"]
    if not body:
        fail("rulesbuild_skips_presentation: could not extract RulesBuild()'s body")
        return
    if re.search(r'if\s*\(\s*opt\[o\]\s*&&\s*!kRandoOpts\[o\]\.presentation\s*\)', body):
        info("rulesbuild_skips_presentation: RulesBuild's emission loop is gated on !presentation")
    else:
        fail("rulesbuild_skips_presentation: RulesBuild() no longer guards emission with "
             "'opt[o] && !kRandoOpts[o].presentation'")


def check_rulefiles(parsed, dump_files):
    rule_files = set(parsed["rule_files"])
    if not rule_files:
        return
    if rule_files != dump_files:
        only_c = sorted(rule_files - dump_files)
        only_py = sorted(dump_files - rule_files)
        fail("rulefiles_match_dump: kRuleFiles vs dump_logic.py's unconditional writes differ"
             + (" (only in kRuleFiles: %s)" % only_c if only_c else "")
             + (" (only in dump_logic.py: %s)" % only_py if only_py else ""))
    else:
        info("rulefiles_match_dump: kRuleFiles (%d files) matches dump_logic.py's unconditional writes"
             % len(rule_files))


def check_logic_folders(parsed):
    rule_files = parsed["rule_files"]
    if not rule_files or not os.path.isdir(OUT_DIR):
        info("logic_folder_coverage: randomizer_ref/out not found, skipped")
        return
    complete, rebuild = [], []
    for name in sorted(os.listdir(OUT_DIR)):
        path = os.path.join(OUT_DIR, name)
        if not name.startswith("logic_") or not os.path.isdir(path):
            continue
        missing = [f for f in rule_files if not os.path.isfile(os.path.join(path, f))]
        if missing:
            rebuild.append("%s (missing %s)" % (name, ", ".join(missing)))
        else:
            complete.append(name)
    if not complete and not rebuild:
        info("logic_folder_coverage: no randomizer_ref/out/logic_* folders found")
        return
    info("logic_folder_coverage: %d complete (%s)%s"
         % (len(complete), ", ".join(complete) if complete else "none",
            "; rebuild on demand: %s" % "; ".join(rebuild) if rebuild else ""))


def main():
    if not os.path.isfile(RANDOMIZER_C):
        print("FAIL: cannot find %s" % RANDOMIZER_C)
        return 1
    if not os.path.isfile(SELECT_FILE_C):
        print("FAIL: cannot find %s" % SELECT_FILE_C)
        return 1
    if not os.path.isfile(ARGS_JSON):
        print("FAIL: cannot find %s" % ARGS_JSON)
        return 1
    if not os.path.isfile(DUMP_LOGIC_PY):
        print("FAIL: cannot find %s" % DUMP_LOGIC_PY)
        return 1

    parsed = parse_randomizer_c(read(RANDOMIZER_C))
    widths = parse_select_file_widths(read(SELECT_FILE_C))
    with open(ARGS_JSON, encoding="utf-8") as fh:
        args_json = json.load(fh)
    dump_files = parse_dump_logic_files(read(DUMP_LOGIC_PY))

    if parsed is not None:
        check_rows(parsed, args_json)
        check_lengths(parsed, widths)
        check_refkeys(parsed, args_json)
        check_rulesbuild_skips_presentation(parsed)
        check_rulefiles(parsed, dump_files)
        check_logic_folders(parsed)

    for line in INFO:
        print("  ok: %s" % line)
    if FAILURES:
        print()
        for line in FAILURES:
            print("FAIL: %s" % line)
        print()
        print("rando_menu_check: %d failure(s)" % len(FAILURES))
        return 1

    n = len(parsed["rows"]) if parsed else 0
    print("rando_menu_check: PASS - %d rows, order/arity/lengths/refkeys/"
          "RulesBuild-skip/kRuleFiles/logic-folders all OK" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())

# randomizer_ref — Phase 0 logic dump for the zelda3 C engine

This directory contains a **read-only extraction tool** for the
[ALttPDoorRandomizer](https://github.com/astralogic/ALttPDoorRandomizer) fork
(the Python entrance/door randomizer for ALTTPR). It runs the fork's full
generation pipeline and dumps the cleanly-separable **logic layer** — world
graph, item pool, and access rules — as JSON that the zelda3 C engine can
consume instead of re-implementing the Python world-building.

Nothing inside the ALttPDoorRandomizer clone is modified. The only shim lives
in `stubs/bps/` (an inert stub for the pip `bps` patch library that `Rom.py`
imports unconditionally; never called because ROM patching is suppressed).

Contents:

| File            | Purpose                                                |
|-----------------|--------------------------------------------------------|
| `dump_logic.py` | the extraction tool                                    |
| `stubs/bps/`    | import stub for the `bps` pip module                   |
| `out/seed_<N>/` | one JSON dump set per seed (generated)                 |
| `SCHEMA_NOTES.md` | recommendations for the C-side representation        |
Reference repo used for the committed dumps:
`<path to your ALttPDoorRandomizer clone>`,
git `7e14fdda b00b847d6eccf0931b365a5774c5476a` (2026-08-23),
generator `1.5.6-u`, overworld randomizer `0.7.1.5`.

---

## How to run

From this directory (`zelda3/randomizer_ref`), with any Python 3.9+ on PATH
(developed and run on 3.14):

```bash
# default: repo = ../ALttPDoorRandomizer, out = ./out, seeds 1234 and 5678
python dump_logic.py

# fully explicit — this is the command that produced out/seed_1234 + out/seed_5678
python dump_logic.py \
    --repo "<path to your ALttPDoorRandomizer clone>" \
    --out out \
    --seed 1234 5678 \
    --players 1 \
    --logic noglitches \
    --mode open \
    --goal ganon \
    --door_shuffle vanilla \
    --shuffle vanilla \
    --loglevel error
```

Extra options:

* `--setting key=value` passes any fork setting through to its own CLI parser,
  e.g. `--setting pottery=keys` or a bare flag `--setting skip_playthrough`.
* One process can dump many seeds (`--seed 1 2 3`); each seed re-runs the whole
  generation (~5 s per seed) and writes `out/seed_<N>/*.json`.
* `--with-fill-data` additionally dumps the five phase-1 schema-gap files
  (`keydoors.json`, `shops.json`, `barriers.json`, `vanilla_locations.json`,
  `groups.json`) — see the section below. Without the flag the output set is
  exactly the original six files, byte-for-byte.

Fixed profile used by the committed dumps: `players=1, logic=noglitches,
mode=open, goal=ganon, door_shuffle=vanilla, shuffle=vanilla` (entrances
vanilla), everything else at fork defaults (key/map/compass shuffle `none`,
`pottery=none`, `prizeshuffle=none`, `shopsanity=off`, weapons `random`,
crystals 7/7). All actually-applied settings are recorded per seed in
`meta.json`.

Generation failures are reported as-is: a few seeds fail the fork's own
`Cannot beat game!` validation under heavier profiles (e.g. some
`--door_shuffle basic` seeds — a known door-shuffle bug class in the fork;
the repo itself ships `debug_*.py` scripts tracking it). The vanilla profile
used here generates reliably.

---

## Output files (per seed, ~890 KB total per seed)

| File            | Size (seed 1234) | Contents                                        |
|-----------------|------------------|-------------------------------------------------|
| `regions.json`  | 186 KB           | 933 regions                                     |
| `edges.json`    | 412 KB           | 2217 entrances/exits (the traversable graph)    |
| `locations.json`| 115 KB           | 367 locations (222 of them event locations)     |
| `items.json`    |   4 KB           | 153-item pool, 44 distinct item names           |
| `rules.json`    | 170 KB           | deduped rule node registry + stats + sources    |
| `meta.json`     |   1 KB           | profile, seed, versions, counts, rule stats     |

All rule bodies live in `rules.json`; edges/locations reference them by id so
identical rules (most are shared) are stored once.

### `regions.json` — array of region objects

```json
{"id": 0, "name": "Agahnims Tower", "type": "Dungeon",
 "dungeon": "Agahnims Tower", "hint": "...",
 "light_world": false, "dark_world": false, "indoors": true}
```

* `id` — stable integer: index into the **alphabetically sorted** region-name
  list. Identical across seeds (the vanilla world graph is seed-independent).
* `type` — `RegionType` name: `Menu | LightWorld | DarkWorld | Cave | Dungeon`
  (`Cave` also covers houses).
* `dungeon` — dungeon object name or `null`; `hint` — the fork's hint text.

### `edges.json` — array of directed connections

```json
{"id": 116, "from": 50, "to": 36,
 "entrance": "C Whirlpool Cliff Ledge Drop", "spot_type": "Ledge",
 "rule": "rb59b2228ee",
 "door": "C Whirlpool Cliff Ledge Drop", "door_type": "Logical",
 "vanilla_target": "..."}
```

* One entry per `Entrance` whose `connected_region` is set: `from`/`to` are
  region ids; traversing requires evaluating `rule` **and** being in `from`.
* `spot_type`: `Entrance, Ledge, Mirror, Flute, OWEdge, OWTerrain, OpenTerrain,
  Portal, Whirlpool` (entrance-shuffle categories).
* `door`/`door_type` present when the edge is door-backed (1275 of 2217 edges
  here); `door_type` values: `Logical, Interior, Normal, SpiralStairs, Open,
  Hole, Warp, StraightStairs, ...`.
* `vanilla_target` — vanilla destination region name when the fork records it.

### `locations.json` — array of location objects

```json
{"id": 24, "name": "Castle Tower - Circle of Pots Key Drop", "region": 892,
 "type": "Drop", "event": true, "real": true, "locked": false,
 "address": "0x9e688",
 "rule": "rb59b2228ee", "typed_ir": "rb59b2228ee", "dnf": [[]],
 "forced_item": "Small Key (Agahnims Tower)",
 "placed_item": {"name": "Small Key (Agahnims Tower)", "player": 1,
                 "progression": false}}
```

* `type` — `LocationType`: `Normal, Prize, Logical, Shop, Pot, Drop, Bonk`.
* `event` — event location (its item is a logical event, e.g. `Beat Agahnim 2`,
  the pendant/crystal pickups); 222 of 367 locations here are events. The event
  items flow through the same item counters as normal items.
* `rule` — access rule (see below). `typed_ir`/`dnf` — the fork's own typed
  rule IR and its DNF item-requirements form, present on the 24 spots where
  the fork builds `Rule` objects (enemy kill rooms / key-drop logic).
* `forced_item` — event item hard-placed here; `placed_item` — the item the
  seed's fill actually placed (`null` on unreachable/never-filled spots).
  NOTE: the fork has **no table of vanilla (unrandomized) placements**, so
  there is no `vanilla_item` field; vanilla placement is implicit in the ROM
  data, not in the Python logic layer.
* `address` — ROM pladdr when applicable (`LocationType.Normal` etc.),
  otherwise `null`.

### `items.json` — the item pool

```json
{"item": "Blue Boomerang", "count": 1, "progression": true, "type": null}
{"item": "Small Key (Agahnims Tower)", "count": 5, "progression": false,
 "type": "SmallKey", "precollected": 0}
```

* Aggregated from `world.itempool` after generation (153 items here).
* `progression` — the fork's `advancement` flag. `type` — dungeon-item class
  (`SmallKey | BigKey | Map | Compass | Prize | ...`) or `null`.
* `precollected` — items granted at start (0 for this profile).

### `rules.json` — the rule IR

```json
{"version": 1,
 "profile": {...},
 "nodes":  {"r03b1887097": {"op": "small_key_door", ...}, ...},
 "sources": {"r03b1887097": "spot.access_rule = lambda state: ..."},
 "stats":  {...}, "untranslated": [...]}
```

* `nodes` — id (`"r" + 10 hex of sha1(canonical json)`) → one rule tree each
  (340 unique rules for 2584 spots — heavy dedup, most rules are shared).
* `sources` — original python source snippet per rule id (traced back to the
  exact `set_rule(...)` line).
* See `SCHEMA_NOTES.md` for the node schema and evaluation model.

### `meta.json`

Profile actually applied, seed, `generator_version` (`1.5.6-u`),
`overworld_randomizer_version` (`0.7.1.5`), repo `git_hash`, counts and the
rule-flattening stats shown below.

---

## Rule flattening results (the headline numbers)

For both committed seeds:

| Metric                    | Value                          |
|---------------------------|--------------------------------|
| rule spots (edges+locations) | 2584 (seed 1234) / 2589 (seed 5678) |
| trivial always-true rules | 1518 / 1520                    |
| non-trivial rules         | 1066 / 1069                    |
| **flattened to typed IR** | **1066 (100.0%) / 1069 (100.0%)** |
| fallback (`opaque`) rules | **0**                          |
| unique rule nodes         | 340 / 341                      |
| spots carrying the fork's typed IR (`Rule` objects) | 24    |

Every single access rule in this profile decompiled into the typed node tree —
zero opaque fallbacks. (Fallback machinery exists and is documented below for
heavier profiles; `can_extend_magic`-style bottle arithmetic and the
universal-key branch of `has_sm_key` are the known cases that would produce
opaque markers under other settings.)

Known untranslated sources (none occur in the committed profile):
`bottle_count()` state arithmetic inside `can_extend_magic`,
`has_sm_key()` under `keyshuffle=universal`, comprehensions, and statements
outside the supported return/assign/if-return subset.

---

## Fill-data dumps (`--with-fill-data`) — the phase-1 schema gaps

The core dump leaves four things implicit that the fill-algorithm port needs:
the small-key door semantics behind `small_key_door` rule nodes, shop stock
behind `unlimited`/`extend_magic`, the crystal-barrier flow behind `barrier`
nodes, and the vanilla item placements. `--with-fill-data` closes all four
(plus the semantic item groups) by reading the finished World's
`world.key_logic` / `world.shops` / door objects and the fork's own static
tables. All five files are **profile data, not seed data** — they came out
byte-identical for seeds 1234 and 5678 under the committed profile.

| File                      | Size  | Contents                                            |
|---------------------------|-------|-----------------------------------------------------|
| `keydoors.json`           | 32 KB | 82 key doors (per-dungeon small-key logic)          |
| `shops.json`              | 14 KB | 11 shops, stock slots + `unlimited` map             |
| `barriers.json`           | 489 KB| 1288 doors with crystal state + flow semantics      |
| `vanilla_locations.json`  | 42 KB | 226 vanilla item slots (217 resolved, 9 ambiguous)  |
| `groups.json`             |  2 KB | progressive chains, bottle family, profile limits   |

### `keydoors.json` — small-key door logic (the REAL table)

One `doors[]` entry per `world.key_logic[1][dungeon].door_rules` key: `door`
(the name the `small_key_door` rule nodes reference = the entrance name),
`dungeon`, `small_key_num`, `worstcase`, `partial_threshold`
(`min(worstcase, small_key_num)`), `new_rules` (the raw variant numbers:
`worstcase` / `allow_small` / `lock:<BigKey item>` / `crystal_alternative`),
`allow_small` + `small_location`, `alternate_small_key` +
`alternate_big_key_loc`, `is_valid`. `dungeons[]` carries each dungeon's small
key item, big key item and `max_chests`; `partial_eval` documents the exact
`eval_small_key_door_partial` formula. 72 doors use the partial algorithm; the
10 Misery Mire blue-barrier doors carry `crystal_alternative: 2`.

Cross-check vs the C scaffold's baked `kKeyDoors[]`
(`src/rando/rando_state.c`): **82 doors on both sides, identical door sets,
identical numbers on every semantic field** (worstcase, small_key_num,
crystal_alternative, AllowSmall flag + location, Lock item, lock threshold,
lock location sets). The only textual differences: the dump has
`worstcase: null` where the C table encoded "absent" as 0 (10 Mire barrier
doors), and 4 GT doors' `alternate_big_key_loc` differ only in list order (the
fork stores a Python set; order is meaningless — the test is "any listed
location holds the big key"). So both tables agree; the dump is now the
authoritative source and the baked table can be retired. Note the C scaffold's
docs called the table "90 doors" — it is 82 rows (72 key doors + 10 barrier
doors); the count in `RANDO_PHASE1.md` was a miscount.

### `shops.json` — shop stock

`shops[]`: one entry per `world.shops[1]` shop — `region` (the shop's Region;
in the region graph these are ordinary `Cave`-type regions whose name is the
shop name and which contain the shop's item locations), `shop_type`,
`room_id`, `inventory[]` slots (`item`, `price`, `max`, `replacement`,
`unlimited`), the `locations[]` served (from the fork's
`shop_to_location_table`) and each location's `stock` forced item (with
shopsanity off the stock is placed as `forced_item`, matching
`locations.json`). `unlimited`: item -> selling regions, the exact input of
`can_buy_unlimited(item)` (true iff a reachable shop sells the item as a slot
item or as its after-limit replacement). Cross-check: the C scaffold's baked
`kUnlimited[]` (`Bombs (10)` x8 regions, Green/Blue Potion -> Potion Shop) is
a correct subset of this table.

### `barriers.json` — crystal barrier flow

`doors[]`: one entry per distinct Door in the world — `crystal`
(`Null | Blue | Orange | Either`) as set by the fork's static
`door.barrier(...)` / `door.c_switch()` calls in `Doors.py`, plus `blocked`,
`trapped`, `stonewall`, `small_key_door`, `big_key_door`,
`alternative_crystal_rule`, `req_event` and the door's from/to regions.
`rule_probes[]` lists the (region, color) pairs the `barrier` rule nodes test
(52 unique pairs here = the 96 BARRIER nodes deduped). `semantics` documents
the exact flow, and YES it is fully dumpable/dumpable-exact: overworld
regions carry state Orange; dungeon regions OR-accumulate Blue/Orange bits
over paths; a door with crystal state C requires the carried state to contain
C (`valid_crystal`, also satisfied by `Either` or by
`alternative_crystal_rule` = the Mire barriers' 2-small-key alternative) and
sets the carried state to C afterwards; `c_switch` doors are crystal-switch
touch points (state `Either`). The C scaffold's current approximation
("reachable ⇒ both crystal states") is strictly more permissive than this;
replacing it needs a 2-bit state per region plus the per-door table — no
other data is required. `blocked` trap doors are seed-dependent and propagate
no crystal flow.

### `vanilla_locations.json` — vanilla placement table

Method: **inverted from the fork's own static tables** in
`source/item/FillUtil.py` (`vanilla_mapping` plus the `keydrop_`/`potkeys_`/
`shop_`/`retro_` variants when the profile activates them — none are active
under the committed profile except the base table). No generation involved;
the table is seed-independent. Each `locations[]` entry: `vanilla` = every
fork-table candidate, `picked` = the single profile-resolved item (basic vs
progressive variants resolved via `world.progressive`; Ocarina via flute
mode) or `null` for the 9 slots the fork itself leaves ambiguous (the
pendant/crystal prize slots — prizeshuffle=none still shuffles prize items
per seed in this fork). Key-drop/pot-key/shop-slot vanilla items are the
`forced_item`s already visible in `locations.json`/`shops.json`. Limitation:
generic `Bottle` slots stand for any bottle of the difficulty's bottle list.

### `groups.json` — semantic item groups

The groups `CollectionState.collect()` consumes: the five progressive chains
(sword/shield/glove/bow/armor with their tier items and the shield's
`Shield Level` counter), the bottle family (prefix rule + member names), and
the profile limits (`progressive_*_limit` from the fork's difficulty table:
sword 4, shield 3, armor 2, bow 2, **bottle 4** under `normal`). The C
scaffold bakes these chains with a bottle cap of 8 — the fork's cap is
`progressive_bottle_limit = 4` (inert in the committed dumps, whose pool has
exactly 4 distinct bottle names, but wrong for richer pools); `groups.json`
is the authoritative source.

---

## How it works (implementation notes)

1. `parse_cli()` from the fork builds the argument namespace; `Main.main()`
   runs the full pipeline (regions → entrances → doors → item pool → rules →
   fill → playthrough validation) and returns the `World`.
2. Most access rules in this fork are **raw lambdas** on
   `spot.access_rule` — the fork's typed `Rule` IR
   (`source/logic/Rule.py`) is only attached at ~24 spots
   (`spot.verbose_rule`). `dump_logic.py` therefore decompiles every lambda:
   AST parse → translate to the node IR, with
   * recursive inlining of `CollectionState`/`Region`/`Boss` helper methods
     (`can_lift_rocks`, boss `defeat_rule`s, ...) with parameters bound,
   * constant folding of world data (settings, tile swaps, difficulty limits)
     — sound because a dump is for one fixed profile; folded values appear as
     `static` nodes,
   * direct interception of the fork's own special-rule lambdas
     (`eval_*_small_key_door*`, `eval_location_main`),
   * the fork's typed `Rule` trees serialized directly where they exist,
     including their DNF (`get_requirements()`).
3. Untranslatable subtrees become `{"op":"opaque", "reason", "python"}`
   markers and are counted in `rules.json.stats.untranslated`.

The dumps are deterministic per seed except where the fork itself randomizes
world construction: the region list was byte-identical across the two seeds,
and so was the edge structure (from/to/entrance/spot_type/door — only the
rule-id references move, since a few rules fold against per-seed world state).
The location set can differ by a few seed-dependent event spots (e.g. five
`* Area Crab Drop` events present in seed 5678 only); of the 340 unique rule
nodes in seed 1234, all but one reappear in seed 5678.

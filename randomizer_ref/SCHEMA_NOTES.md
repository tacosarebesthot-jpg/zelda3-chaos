# SCHEMA_NOTES — consuming rules.json from C (zelda3)

Recommendations for the C-side representation: how a bytecode / tagged-union
rule interpreter should consume the dump, and how CollectionState's item
counters map. Everything below refers to the JSON produced by `dump_logic.py`.

## 1. Rule trees as a tagged union

Each rule is a JSON tree of nodes. Map 1:1 to a tagged union:

```c
typedef enum {
    R_STATIC, R_AND, R_OR, R_NOT,
    R_ITEM, R_ITEM_COUNT_SUM, R_BOTTLES,
    R_CRYSTALS, R_PENDANTS, R_BOSSES, R_HEARTS,
    R_REACHABLE, R_UNLIMITED, R_EXTEND_MAGIC,
    R_BARRIER, R_NOT_BUNNY, R_REACH_LW, R_REACH_DW,
    R_EVERYTHING, R_DOOR_OPEN, R_LOCATION_CHECK,
    R_SMALL_KEY_DOOR, R_OPAQUE
} RuleOp;

typedef struct RuleNode {
    RuleOp op;
    union {
        bool value;                              /* R_STATIC */
        struct { struct RuleNode **n; uint16_t count; } kids;  /* AND/OR/NOT */
        struct { const char *item; uint16_t count; bool is_key; } item;
        struct { const char **items; uint16_t n; uint16_t count; } sum;
        uint16_t count;                          /* BOTTLES/CRYSTALS/...   */
        struct { const char *spot; const char *type; } reach;
        struct { const char *item; } shop;
        struct { uint16_t magic; bool fullrefill; } magic;
        struct { const char *region; uint8_t barrier; } barrier; /* 0=orange 1=blue */
        struct { const char *region; } bunny;
        struct { const char *door; } door_open;
        struct { const char *item; const char *loc; } loc_check;
        struct { const char *door; const char *dungeon; } skd;
    };
} RuleNode;
```

* `and`/`or` have N children (already flattened — no nested same-op wrappers,
  no constant children, no duplicate siblings; the dumper simplified them).
  An empty `and` is not emitted; the dumper would have made it `static`.
* Evaluation is the obvious short-circuit recursion over the union. Depth is
  small (≤ ~12 in practice); the decompiler capped at 48.
* Intern item/spot/region names once into string tables at load time and
  reference by index — rule trees are heavily shared (340 nodes cover 2584
  spots), so consider deduping on the C side too and storing just a rule id
  per edge/location.
* `R_OPAQUE` must never occur in the committed dumps (0 of 1066 non-trivial
  rules); if it does (other profiles), fail loudly — it means the rule could
  not be flattened and the original python is in `rules.json.sources`.

### Special node semantics (exact fork behavior)

| Node | Fork semantics to replicate |
|------|------------------------------|
| `item` (`key:true`) | `state.has_sm_key` — plain counter under non-universal keyshuffle; the universal-key branch was never emitted here |
| `item_count_sum` | sum of counters of `items[]` >= `count` (used by triforce-hunt goals) |
| `bottles` | count of **distinct** pool items whose name starts with `"Bottle"` (Bottle, Bottle (Red), Bottle (Green), Bottle (Blue), Bottle (Fairy), Bottle (Gold Bee)...) |
| `crystals` | count of distinct `Crystal 1..7` event items held >= `count` |
| `pendants` | same over `Green/Red/Blue Pendant` |
| `bosses` | count of reachable `<Dungeon> - Boss Kill` regions (optionally filtered by prize name) >= `count` |
| `hearts` | `min(BossHeartContainer,limit) + SanctuaryHeartContainer + min(PieceOfHeart,limit)//4 + 3 >= count` (limits from difficulty requirements) |
| `unlimited` | any shop whose region is reachable sells `item` with unlimited stock — needs the shop tables; only occurs for retro/bomb-farm paths in this profile |
| `extend_magic` | `basemagic(8→16→32 with upgrades)` plus bottle refills from purchasable potions >= `magic`; consult `shop_regions` on the typed-IR variant when present |
| `barrier` | crystal-switch state of `region` — the world maintains an orange/blue `CrystalBarrier` per region pair; `blue` = blue barrier down |
| `not_bunny` | has Moon Pearl OR region is not bunny-forced (region's light/dark flags + mode) |
| `reach_light_world` / `reach_dark_world` | any currently-reachable region has the flag |
| `door_open` / `small_key_door` | door-state / dungeon key logic — see §4 |
| `static` | profile-constant (settings folds); do NOT evaluate against state |
| `location_check` | specific item already placed at a location (placement rules; also expressible as a post-fill assertion) |

## 2. Reachability loop (the interpreter's driver)

The rules interleave with graph reachability exactly like the fork:

1. Start with region `Menu` reachable; `static` rules don't gate anything.
2. Worklist over `edges.json`: edge `from`→`to` opens when `from`'s region is
   reachable **and** its rule evaluates true in the current CollectionState.
3. Region reachability feeds back into rules (`reachable`, `bosses`,
   `unlimited`, `reach_*`), so iterate to fixpoint (BFS with re-evaluation on
   item pickup — the same "sweep" the Python code does).

## 3. CollectionState → C

Python `CollectionState` is a bag of counters plus world-sweep bookkeeping.
The part rules actually touch maps to:

```c
typedef struct CollectionState {
    /* prog_items: Counter[(item_name, player)] — key by interned item id */
    uint16_t *item_count;          /* per player: item_id -> count */
    /* forced_keys: small keys reserved by the key-flooder (has_sm_key_strict) */
    uint16_t *forced_keys;
    /* reachable_regions: region_id -> CrystalBarrier state (orange/blue) */
    uint8_t  *region_barrier;
    bool     *region_reachable;
    /* door state for R_DOOR_OPEN / small-key doors */
    bool     *door_open;
    uint16_t *dungeon_key_count;   /* per dungeon small-key counter */
} CollectionState;
```

* `state.has(item, player, count)` ⇒ `item_count[item_id] >= count`. Event
  items (`Beat Agahnim 2`, `Crystal 5`, `Pick Up Kiki`, `Return Smith`, ...)
  use the same counter — no special casing needed; just allocate ids for every
  name in `items.json` plus every event name appearing in rules.
* Counters **count distinct item names**, not instances: holding two different
  Bottle variants is `bottle_count()==2`; `has()` with count>1 means the same
  name collected multiple times (only upgrade items like
  `Bomb Upgrade (+5)` ever do that).
* `static` nodes encode profile constants — if the C engine wants
  profile-independent rules, re-run `dump_logic.py` per profile instead of
  parametrizing these.

## 4. Gaps / follow-ups for the C port — ALL CLOSED (phase 1.5)

The four open items below were closed by re-running `dump_logic.py
--with-fill-data`, which emits five extra files per seed: `keydoors.json`,
`shops.json`, `barriers.json`, `vanilla_locations.json`, `groups.json`.
Schemas in §5; this section states what each gap actually was and its answer.

* **Small-key doors — CLOSED (`keydoors.json`).** The dump's
  `small_key_door` nodes carry only `(door, dungeon)`; the numbers live in
  `world.key_logic[player][dungeon].door_rules` (class `DoorRules` in
  `KeyDoorShuffle.py`) and are now dumped verbatim. Under this profile the
  fork attaches the **partial** evaluator everywhere
  (`key_logic_algorithm=partial`): `open = state.is_door_open(door) OR
  min(WorstCase, small_key_num) keys OR (AllowSmall: small_location holds a
  dungeon small key AND >= AllowSmall_n keys) OR (Lock: any
  alternate_big_key_loc holds the big key AND >= min(Lock_n,
  alternate_small_key) keys)`. The 10 Misery Mire blue-barrier doors instead
  carry `CrystalAlternative: 2` (evaluated by `eval_alternative_crystal`, ORed
  onto the barrier rule). `partial_threshold` = `min(worstcase,
  small_key_num)` is precomputed per door.
  **Cross-check vs the C scaffold's baked `kKeyDoors[]`**
  (`src/rando/rando_state.c`): the table has **82 rows** (not 90 as
  `RANDO_PHASE1.md` says — a doc miscount); the dump has exactly the same 82
  doors and matches on every semantic field (worstcase, small_key_num,
  crystal_alternative, AllowSmall flag/location, Lock item, lock threshold,
  lock location sets). Diffs are representational only: dump `worstcase:null`
  vs C `0` for the 10 Mire barrier doors, and 4 GT doors'
  `alternate_big_key_loc` differ in list order only (fork stores a Python
  set; only membership matters). **Verdict: the baked table was right; the
  dump is now authoritative and seed-independent** (byte-identical for seeds
  1234 and 5678). The C side should load `keydoors.json` instead of baking.
* **Shops — CLOSED (`shops.json`).** Shop stock comes from
  `world.shops[1]` (built by `create_shops` in `Regions.py` from the static
  `shop_table`); the shop→location mapping is the static
  `shop_to_location_table`. A "shop region" is an ordinary region (type
  `Cave`) named after the shop that contains the shop's item locations;
  `can_buy_unlimited(item)` = ∃ shop whose region is reachable and that sells
  `item` as a slot item (or as the slot's after-limit `replacement`). The
  baked `kUnlimited[]` was verified correct (a subset: Bombs (10) x8 regions,
  Green/Blue Potion -> Potion Shop); `shops.json` extends it with all 11
  shops' full stock and the retro/take-any shops for other profiles.
* **Crystal barriers — CLOSED (`barriers.json`); exact semantics WERE
  recoverable.** `door.crystal` is set statically in `Doors.py`
  (`door.barrier(Blue/Orange)`, `door.c_switch()` → `Either`) for 179 of 1288
  doors (64 Blue, 52 Orange, 63 Either); 10 Mire doors also set
  `alternative_crystal_rule`. The exact flow (CollectionState.traverse_world,
  BaseClasses.py): the engine carries a 2-bit crystal state
  (`Blue=1|Orange=2`, `Either=3`); non-dungeon regions are always entered
  with Orange; dungeon regions OR-accumulate the state over all paths
  (re-entering with a new bit extends a region's accumulated state); passing
  a door requires `valid_crystal` — `door.crystal ∈ {Null, Either}` or the
  carried state contains `door.crystal` or `door.alternative_crystal_rule` —
  and afterwards the carried state **becomes** `door.crystal` (unchanged if
  Null). `can_reach_blue/orange(region)` (the `barrier` rule nodes) = region
  reachable AND carried state has the bit. Caveats: `door.blocked` (trap
  doors, 39 here) propagate no crystal flow and are seed/runtime-dependent;
  the `can_hit_crystal_through_barrier` ranged-switch rules are already
  present in the dumped rules as plain item nodes. The C scaffold's
  approximation ("reachable ⇒ both crystal states") is strictly MORE
  permissive than the truth; exact evaluation needs only a 2-bit state per
  dungeon region + the per-door `crystal` column now in `barriers.json`.
* **Vanilla placements — CLOSED (`vanilla_locations.json`).** The fork's
  vanilla table is static data after all: `vanilla_mapping` (+ settings-gated
  `keydrop_`/`potkeys_`/`shop_`/`retro_` variants) in
  `source/item/FillUtil.py`, inverted to location→item by the dumper. Method
  and limitations are documented in the file itself: 226 slots, 217 resolved
  under this profile (basic/progressive variants resolved via
  `world.progressive=on`), 9 ambiguous by fork design (pendant/crystal prize
  slots; prizeshuffle=none still shuffles prizes per seed). No `--plando`
  generation was needed. Note `shuffle=vanilla` does NOT mean vanilla
  placement (the fill shuffles anyway), and `algorithm=vanilla_fill` does not
  force it either (`config.static_placement` is populated but never
  consumed in this fork version).
* **Groups (`groups.json`).** The fork has no named item-group table; the
  semantics live in `CollectionState.collect()`: five progressive chains and
  the bottle family, gated by the difficulty limits — under `normal`:
  sword 4, shield 3, armor 2, bow 2, **bottle 4**
  (`progressive_bottle_limit`, ItemList.py `difficulties`). FINDING: the C
  scaffold bakes a bottle cap of 8; the fork's cap is 4. Inert for the
  committed dumps (the pool holds exactly 4 distinct bottle names) but wrong
  for richer pools — bind `groups.json` limits instead.
* **Dungeon/door metadata**: `edges.json` already carries `door_type`;
  `barriers.json` now adds per-door crystal/blocked/smallKey/bigKey flags.
  Room/door ROM flag data (from `RoomData.py`/`Tables.py`) remains the next
  dump for actually patching doors in-engine, but is not needed for logic
  evaluation.

## 5. The `--with-fill-data` files (schemas)

All five are **profile data**: identical for seeds 1234 and 5678 under the
committed profile (unlike `locations.json.placed_item`, which is per-seed).
Emit per seed dir alongside the core files; the core six are byte-identical
with or without the flag.

### `keydoors.json`

```json
{"version": 1,
 "algorithm": "partial", "keyshuffle": "none",
 "partial_eval": "...exact formula text...",
 "dungeons": [{"dungeon": "Eastern Palace",
               "small_key_item": "Small Key (Eastern Palace)",
               "big_key_item": "Big Key (Eastern Palace)", "max_chests": 2}],
 "doors": [
   {"door": "Eastern Dark Square Key Door WN",     // == rules.json node arg == entrance name
    "dungeon": "Eastern Palace",
    "small_key_num": 2, "worstcase": 2,
    "partial_threshold": 2,                        // min(worstcase, small_key_num)
    "new_rules": {"worstcase": 2},                 // + "allow_small", "lock:<BigKey item>",
                                                   //   "crystal_alternative" when present
    "allow_small": false, "small_location": null,
    "alternate_small_key": null,
    "alternate_big_key_loc": [],                   // sorted location names (a SET; order irrelevant)
    "is_valid": true}]}
```

82 entries (72 key doors + the 10 Misery Mire barrier doors whose only rule
is `crystal_alternative: 2`). Evaluation (partial, exact): door is open iff
`is_door_open(door)` or ANY `new_rules` entry fires —
`worstcase n` → `keys >= min(n, small_key_num)`;
`allow_small n` → `small_location` currently holds the dungeon's small key
AND `keys >= n` (n is always worstcase-1);
`lock:<bk> n` → some location in `alternate_big_key_loc` holds `<bk>` AND
`keys >= min(n, alternate_small_key)`;
`crystal_alternative n` → `keys >= n`.

### `shops.json`

```json
{"version": 1, "shopsanity": false, "note": "...",
 "shops": [{"region": "Kakariko Shop", "region_type": "Cave", "shop_type": "Shop",
            "room_id": 76, "locked": false, "custom": false,
            "inventory": [{"slot": 0, "item": "Red Potion", "price": 120,
                           "max": 0, "replacement": null,
                           "replacement_price": 0, "unlimited": true}],
            "locations": ["Kakariko Shop - Left", ...],     // shop_to_location_table
            "stock": [{"location": "Kakariko Shop - Left", "forced_item": "Red Potion"}]}],
 "unlimited": {"Bombs (10)": ["Dark Death Mountain Shop", ...],
               "Green Potion": ["Potion Shop"], "Blue Potion": ["Potion Shop"], ...}}
```

11 shops. `unlimited` maps every item sold by >= 1 shop to the selling shop
region names — evaluate `unlimited(item)` as "any of these regions is
reachable" (`has_unlimited` = item is a slot item or the slot's
after-limit replacement).

### `barriers.json`

```json
{"version": 1,
 "semantics": {"states": {"Null": 0, "Blue": 1, "Orange": 2, "Either": 3},
               "overworld_state": "Orange", "dungeon_flow": "...",
               "rule_probe": "...", "blocked_doors": "..."},
 "doors": [{"door": "Hera Front to Lobby Barrier - Blue", "type": "Door",
            "from_region": "Hera Front", "to_region": "Hera Lobby",
            "crystal": "Blue", "c_switch": false,
            "blocked": false, "trapped": false, "stonewall": false,
            "small_key_door": false, "big_key_door": false,
            "alternative_crystal_rule": false, "req_event": null}],
 "rule_probes": [{"region": "Hera Lobby", "barrier": "blue"}]}
```

1288 doors (179 with crystal != Null: 64 Blue, 52 Orange, 63 Either =
c_switch touch points; 10 with `alternative_crystal_rule`, all Mire; 39
`blocked` trap doors — seed-dependent). `rule_probes` = the 52 unique
(region, color) pairs behind the 96 `barrier` rule nodes. The C engine that
wants exact barrier flow needs: a 2-bit carried state per dungeon region
(OR-accumulated), the door table's `crystal` column, and the
`alternative_crystal_rule`/`blocked` flags — everything else is already in
the core dump.

### `vanilla_locations.json`

```json
{"version": 1, "method": "...", 
 "profile": {"progressive": "on", "dropshuffle": "none", "pottery": "none",
             "shopsanity": false, "take_any": "none", "bow_mode": "progressive",
             "swords": "random"},
 "counts": {"locations": 226, "picked": 217, "ambiguous": 9, "not_in_this_world": 0},
 "locations_not_in_dump": [],
 "locations": [{"location": "Eastern Palace - Big Chest",
                "picked": "Progressive Bow",      // null when ambiguous
                "vanilla": ["Progressive Bow"],   // every fork-table candidate
                "sources": ["vanilla_mapping"]}]} // which table(s) contributed
```

Inverted from `source/item/FillUtil.py` static tables (see §4 for method).
The 9 `picked: null` slots are exactly the pendant/crystal `* - Prize`
locations. Bind `picked` when non-null; treat ambiguous slots as "any of
`vanilla`".

### `groups.json`

```json
{"version": 1,
 "profile": {"difficulty": "normal", "progressive": "on", "bow_mode": "progressive"},
 "limits": {"sword": 4, "shield": 3, "armor": 2, "bow": 2, "bottle": 4,
            "boss_heart_container": 255, "heart_piece": 255},
 "progressive": {"Progressive Sword": {"tiers": ["Fighter Sword", "Master Sword",
                                                 "Tempered Sword", "Golden Sword"]},
                 "Progressive Shield": {"tiers": ["Blue Shield", "Red Shield",
                                                  "Mirror Shield"],
                                        "counter": "Shield Level"},
                 "Progressive Glove": {"tiers": ["Power Glove", "Titans Mitts"]},
                 "Progressive Bow": {"tiers": ["Bow", "Silver Arrows"]},
                 "Progressive Armor": {"tiers": ["Blue Mail", "Red Mail"]}},
 "bottle_family": {"prefix": "Bottle",
                   "members": ["Bottle", "Bottle (Red Potion)", ...],
                   "hard_members": [...]},
 "collect_semantics": "..."}
```

`limits.*` are the fork's `difficulty_requirements` for this profile
(`progressive_*_limit`) and gate the chains inside `collect()` exactly as the
C scaffold's `collect()` port does — note `bottle` is 4, not the 8 currently
baked in `src/rando/rando_state.c`.

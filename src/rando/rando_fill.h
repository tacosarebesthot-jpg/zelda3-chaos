/* rando_fill.h — phase-A item FILL algorithm for the native randomizer.
 *
 * Ports the CONCEPT of the reference fork's Fill.py
 * (fill_restrictive / distribute_items_restrictive / fast_fill) onto the
 * phase-1 logic engine: restrictive placement of the progression pool with
 * per-placement validation and rollback, then a blind fast-fill of the
 * junk pool.
 *
 * Placement rule (frontier fill): keep a frontier of unfilled reachable
 * locations — sweep the REAL partial world (placed items only, no
 * assumptions), place each progression item at a random reachable unfilled
 * slot, then re-sweep and require every fill-placed progression item so
 * far (the new one included) to be obtainable; rollback and try the next
 * candidate on failure.  Reachability is monotone in the placement set, so
 * every placed item stays obtainable forever and the final sweep (all
 * items placed, nothing assumed) collects the whole pool by construction —
 * the seed is completable whenever the fill completes.  An attempt that
 * runs out of frontier for a progression item fails and the outer
 * retry-until-completable loop reshuffles (bounded attempts, reported in
 * RandoFillStats::attempts).
 *
 * Why not the literal Fill.py check (candidate reachable in the
 * everything-assumed "maximum exploration" state)?  It is only sound
 * inside the fork's full generator (prize/dungeon fills pre-seed gated
 * items with keys assumed, can_beat_game fallback, full-generation
 * retries).  Ported standalone it produces circular dependencies — e.g.
 * Moon Pearl inside Ganon's Tower, reachable in the assumed state but only
 * through the dungeons the pearl itself gates — and the final sweep stalls
 * (measured: 151-396 of 933 regions).  See tools/RANDO_FILL.md.
 *
 * Shuffle-eligible locations (phase A, multiset-preserving): exactly the
 * slots whose dumped item is in the pool and that are normal, unforced,
 * unlocked, non-mechanics slots (not shop stock / pot / drop / keydrop /
 * prize).  Clearing exactly those slots removes exactly the pool copies
 * from the world and nothing else — no dungeon item or prize can go
 * missing.  The vanilla placement table (vanilla_locations.json) is read
 * as a cross-check: the slots whose VANILLA item is in the pool but that
 * are NOT refilled hold dungeon items the fork parked there while
 * shuffling (stats->vanilla_skipped; 25 in seed 1234).  Refilling those by
 * the vanilla rule would delete keys — tried, measured, rejected (see
 * tools/RANDO_FILL.md).
 *
 * Phase-A scope (documented deviations from Fill.py live in
 * tools/RANDO_FILL.md): dungeon items (small/big keys, maps, compasses)
 * and dungeon prizes are NOT part of the shuffled pool (they are not in
 * items.json for this profile); they keep their dumped placements.  Shops,
 * pots, drops and event locations are likewise untouched.
 *
 * Engine-independent: no SDL, no g_ram, no zelda3 headers — C11 + libc.
 * Not compiled into zelda3.exe (build_msvc.cmd globs src\*.c only).
 */
#ifndef RANDO_FILL_H
#define RANDO_FILL_H

#include <stddef.h>

#include "rando_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* observed by one Rando_FillWorld call (all counters across attempts) */
typedef struct RandoFillStats {
    unsigned int seed;            /* fill seed used                        */
    int          attempts;        /* shuffle->fill->validate attempts run  */
    unsigned int prog_items;      /* progression placements (last attempt) */
    unsigned int junk_items;      /* fast-fill placements (last attempt)   */
    unsigned int eligible;        /* shuffle-eligible locations            */
    unsigned int pool_size;       /* items in the shuffled pool            */
    unsigned int sweeps;          /* full reachability sweeps performed    */
    unsigned int candidate_tests; /* candidate locations tentatively tried */
    unsigned int sphere_escapes;  /* candidate rollbacks after a failed    */
                                  /* strict validation (expected 0; the   */
                                  /* monotonicity argument forbids it)    */
    unsigned int vanilla_skipped; /* vanilla-pool slots NOT refilled (they */
                                  /* hold dungeon items; see RANDO_FILL.md)*/
    unsigned int pin_applied;     /* 1 when the demo-chest pin (phase B2)  */
                                  /* was honored; 0 = pin ignored, the     */
                                  /* unpinned fill ran                     */
    char         pin_note[160];   /* human-readable pin outcome (applied   */
                                  /* detail or the reason it was ignored)  */
    double       fill_ms;         /* wall time inside Rando_FillWorld      */
} RandoFillStats;

/* Fill `world` deterministically from `seed`.
 *
 *   world          - a loaded RandoWorld (Rando_LoadWorld); the dump's own
 *                    placements are used as the fixed base world and are
 *                    RESTORED before the call returns (the world is not
 *                    modified by a fill).
 *   seed_data_dir  - the seed dump directory the world was loaded from;
 *                    vanilla_locations.json is read from here to determine
 *                    the vanilla placement table and hence the
 *                    shuffle-eligible location subset.
 *   seed           - fill seed; identical (world, dir, seed) triples produce
 *                    identical placement tables (own splitmix64 stream).
 *   placements     - out, may be NULL: caller-allocated int[world->
 *                    n_locations]; location id -> item id, or -1 when the
 *                    location carries no item.  Reflects the complete final
 *                    world: fill placements plus the untouched fixed ones.
 *   eligible_flags - out, may be NULL: uint8_t[n_locations], 1 for the
 *                    shuffle-eligible locations the fill refills.
 *   stats          - out, may be NULL.
 *
 * Returns 0 on success (final world verified completable), -1 with `err`
 * set otherwise.
 */
int Rando_FillWorld(RandoWorld *world, const char *seed_data_dir,
                    unsigned int seed,
                    int *placements, uint8_t *eligible_flags,
                    RandoFillStats *stats, char *err, size_t errsz);

/* Rando_FillWorld with the phase-B2 DEMO-CHEST PIN: `pin_location` /
 * `pin_item` (NULL or empty = no pin, identical to Rando_FillWorld) force
 * one specific chest to hold one specific item BEFORE the shuffle runs.
 *
 * The pin is applied INSIDE the fill (not by pre-writing the caller's
 * placement table): the pinned slot leaves the shuffle-eligible set (so the
 * clear/refill cycle never touches it) and the pool loses exactly one copy
 * of `pin_item`, keeping eligible-slots == pool-size.  The world's final
 * item multiset is unchanged - the pinned copy replaces the slot's own pool
 * copy - so the fill treats the pin like any other fixed ("dump") placement:
 * sweeps see it from the first attempt, frontier/junk placement skip the
 * occupied slot, and the final can-beat validation covers it (the frontier
 * monotonicity argument only guards fill-placed items; the pin rides on the
 * final validation like every other fixed slot).  When the pinned location
 * is reachable from the start (the default, "Secret Passage", is), the
 * pinned item is collected in the very first sweep - available immediately.
 *
 * A pin that cannot be honored (unknown location/item names, slot not
 * shuffle-eligible, item not in the shuffled pool) is IGNORED, never a hard
 * failure: stats->pin_applied is 0, stats->pin_note carries the reason, and
 * the unpinned deterministic fill runs.
 *
 * eligible_flags[pin_location] is set to 2 (Rando_FillWriteJson prints
 * origin "pin" for it; 1 stays "fill", 0 "dump").
 *
 * Same return contract as Rando_FillWorld. */
int Rando_FillWorldEx(RandoWorld *world, const char *seed_data_dir,
                      unsigned int seed,
                      int *placements, uint8_t *eligible_flags,
                      RandoFillStats *stats, char *err, size_t errsz,
                      const char *pin_location, const char *pin_item);

/* 1 when the game is beatable with `placements` installed: applies the
 * table to the world (restoring the previous placements afterwards), runs
 * the existing collection sweep (Rando_SweepReachability with collect=1 —
 * locations' access rules evaluated, placed items collected into counters
 * when their location is reached) and requires both the victory item
 * (Triforce) and the goal location (Ganon) to be obtained/reached.
 * `placements == NULL` evaluates the world's current placements. */
int Rando_CanBeat(RandoWorld *world, const int *placements);

/* Install `placements` into the world's locations (no restore).  Length
 * must be world->n_locations.  Returns 0. */
int Rando_FillApply(RandoWorld *world, const int *placements);

/* ------------------------------------------------------------------ */
/* phase-B apply support: model item -> engine chest byte              */
/* ------------------------------------------------------------------ */

/* Apply-time counters for the chest-subset apply (randomizer.c writes the
 * engine's kDungeonRoomChests table from them).  The boot banner and
 * rando_placement.json report these: the fill's can-beat proof describes
 * the MODEL (chain-item counts), and an unqualified "can-beat verified"
 * may only be claimed when `skipped` is 0, i.e. every chest placement
 * actually landed in the engine table.  Item placements at slots with no
 * engine apply (NPCs, pedestals, drops, pots, events) are counted in
 * non_chest_locations - they keep their vanilla in-game content until the
 * phase-C apply layer exists. */
typedef struct RandoApplyCounts {
    int chest_locations;     /* placements resolving to an engine chest home */
    int applied;             /* chest records actually written               */
    int skipped;             /* chest placements that kept the vanilla item  */
    int non_chest_locations; /* item placements with no engine apply (ph. C) */
} RandoApplyCounts;

/* ALTTPR item name -> engine chest receipt byte, from the generated
 * kRandoItemCodes table (tools/gen_rando_tables.py), with the dungeon
 * key/map/compass names remapped to the engine's native codes (the
 * vanilla-doors profile keeps every dungeon item inside its own dungeon).
 * Returns -1 when the name is unknown.  Codes >= 0x4C (76) have no engine
 * receipt slot (the receipt tables are 76 entries); use
 * Rando_ApplyChestCode, which remaps the progressive chains on top. */
int Rando_LookupEngineCode(const char *name);
/* Pool names whose fork ROM code collides with the reserved 0x4C..0x7F
 * range -> the code the engine really can hand out: the extra receipt
 * codes 0x81..0x87 (capacity upgrades, silver arrows, triforce piece) or,
 * for "Master Sword", the plain vanilla receipt 0x01.  See
 * kRandoExtraCodes in rando_fill.c.  Returns -1 when the name is not one. */
int Rando_ExtraItemCode(const char *name);

/* ---- keysanity: dungeon-targeted key / big key / map / compass ----------
 * When a file shuffles a dungeon-item class ANYWHERE the plain engine
 * receipts cannot say WHICH dungeon the item belongs to, so the fill uses
 * a second code range instead:
 *
 *     code = 0x4C + 13 * class + (dungeon - 1)
 *       class 0 small key 0x4C..0x58   class 1 big key  0x59..0x65
 *       class 2 map       0x66..0x72   class 3 compass  0x73..0x7F
 *
 * with `dungeon` the game's palace index (cur_palace_index_x2 / 2), 1..13
 * ("Escape" = 1, Hyrule Castle + the sewers).  The full argument for the
 * range lives above the implementation in rando_fill.c; the engine end
 * (decode + grant) is Randomizer_DungeonItemBegin / ...Grant in
 * randomizer.c, called from player.c and misc.c. */

/* Which classes this file shuffles anywhere: the RANDOMIZER page values,
 * 0 = own dungeon, 1 = wild; `keys` may also be 2 = universal, which keeps
 * the plain 0x24.  Call before the apply loop; the default is all-zero
 * (own dungeon), i.e. exactly the pre-keysanity behaviour. */
void Rando_SetKeysanity(int keys, int bigkeys, int maps, int compasses);

/* KEY DROPS = SHUFFLED (the reference's dropshuffle=keys): let the fill place
 * items at the fourteen RANDO_LOC_DROP locations - the small keys enemies
 * drop in dungeons, plus Hyrule Castle's big key.  Off (the default) they are
 * refused like pots and prizes and keep their dumped content.  Call before
 * the fill; the engine end is Randomizer_KeyDropSpawned / ...Receipt in
 * randomizer.c, driven from src/sprite.c. */
void Rando_SetDropShuffle(int on);

/* POTS = SHUFFLED (the reference's pottery=keys): let the fill place items at
 * the nineteen RANDO_LOC_POT locations - the small keys hidden under pots (and
 * Ice Palace's hammer block).  Off (the default) they are refused like prizes
 * and keep their dumped content.  Only `keys` is ever sent to the reference:
 * the wider pottery modes turn hundreds of ordinary pots into slots and this
 * engine can only hand out the nineteen that have a key secret record.  Call
 * before the fill; the engine end is Randomizer_PotKeySpawned / ...Receipt in
 * randomizer.c, driven from src/sprite.c Sprite_SpawnSecret. */
void Rando_SetPotShuffle(int on);

/* BONK DROPS = SHUFFLED (the reference's bare bonk_drops flag): let the fill
 * place items at the 42 RANDO_LOC_BONK locations - the prizes hidden in trees,
 * bonk rocks and statues that a boots dash or the Quake spell shakes loose.
 * Off (the default) they are refused like pots and prizes and keep their
 * dumped content.  Call before the fill; the engine end is
 * Randomizer_BonkDropSpawned / ...Receipt in randomizer.c, driven from
 * src/sprite.c's overworld sprite loader and Sprite_HandleAbsorptionByPlayer. */
void Rando_SetBonkShuffle(int on);

/* "Big Key (Tower of Hera)" -> its dungeon-targeted code, or -1 when the
 * name is not a dungeon item, its class is not shuffled anywhere for this
 * file, or the dungeon name is not one of the reference's thirteen (that
 * includes "Small Key (Universal)"). */
int Rando_DungeonItemCode(const char *name);

/* Decode side, for the engine and for tests. */
int Rando_DungeonItemIsCode(int code);      /* in 0x4C..0x7F                */
int Rando_DungeonItemClass(int code);       /* 0 key, 1 big key, 2 map, 3 compass */
int Rando_DungeonItemDungeon(int code);     /* palace index 1..13           */
const char *Rando_DungeonItemName(int code);/* the reference dungeon name   */

/* Resolve the engine chest byte for the model item `item_id`.
 *
 * `chain_seen` (int[w->n_items], zeroed by the caller before the apply
 * loop) counts how many copies of each progressive-chain item were
 * already resolved: the k-th pool copy of a chain item maps to the k-th
 * CONCRETE tier of that chain (tier order per the reference fork's own
 * item_alternates table, ALttPDoorRandomizer ItemList.py).  That makes
 * the model's can-beat proof carry over to the engine world: owning any
 * j copies of an N-tier chain means owning j distinct tiers, whose
 * maximum is >= j-1, so every engine gate of the form "tier >= L"
 * (exactly what the vanilla check routines test) is satisfied whenever
 * the model's corresponding "chain count >= L+1" gate was.  Which
 * location got which tier does not matter for that argument.
 *
 * Returns the byte to write into the chest record (0..75), or -1 with
 * *why (if why != NULL) set to:
 *   "unknown item"           - name not in the code table
 *   "no engine receipt slot" - code >= 0x4C and not a mappable chain
 *                              (e.g. Magic Upgrade (1/4): the engine has
 *                              no quarter-magic receipt, and nothing
 *                              else in this profile's pool lands there)
 */
int Rando_ApplyChestCode(const RandoWorld *w, int item_id, int *chain_seen,
                         const char **why);

/* Write the placement table as JSON for offline inspection.  Locations
 * tagged "origin":"fill" were refilled by the algorithm; "origin":"dump"
 * kept their pre-existing placement.  `eligible_flags` (may be NULL, from
 * Rando_FillWorld) decides the tag.  `apply` (may be NULL) adds the
 * engine-apply counters next to the model claim `can_beat`:
 * can_beat_game stays what the MODEL proved; the "apply" object reports
 * what the engine world actually received.  Returns 0 on success. */
int Rando_FillWriteJson(RandoWorld *world, const int *placements,
                        const uint8_t *eligible_flags,
                        unsigned int seed, const RandoFillStats *stats,
                        int can_beat, const char *json_path,
                        const RandoApplyCounts *apply);

#ifdef __cplusplus
}
#endif

#endif /* RANDO_FILL_H */

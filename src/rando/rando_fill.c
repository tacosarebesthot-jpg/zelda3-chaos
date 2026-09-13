/* rando_fill.c — phase-A item FILL (see rando_fill.h for the contract).
 *
 * Structure (port of ALttPDoorRandomizer Fill.py concepts, flattened for a
 * single player / this profile):
 *
 *   1. Load vanilla_locations.json -> vanilla placement table; a location is
 *      SHUFFLE-ELIGIBLE iff its vanilla item ("picked") is in the item pool
 *      (items.json == world->items[].pool_count), with the bottle family
 *      treated as interchangeable (the fork randomizes bottle contents).
 *      This reproduces the fork's get_unfilled_locations() for a profile
 *      with shopsanity off, pottery/drop/keydrop none, and dungeon items +
 *      prizes handled by their own fills (not ported in phase A — they keep
 *      their dumped placements).
 *   2. Pool = expansion of items[].pool_count (the fork's world.itempool).
 *      Shuffle pool (own splitmix64 stream), stable-partition into
 *      progression / junk exactly like distribute_items_restrictive.
 *   3. Progression: fill_restrictive — per item, a maximum-exploration sweep
 *      (base state + every unplaced pool item), candidate = random
 *      max-reachable unfilled eligible location ("frontier"); tentative
 *      install, strict sphere check (partial-world sweep: all previously
 *      placed progression items still obtainable), rollback & next candidate
 *      on failure; classic assumed-only fallback when unsatisfiable.
 *   4. Junk: fast_fill — blind pop item / pop location.
 *   5. Final can-beat validation with everything really placed; outer
 *      retry-until-completable loop (reshuffle) bounds pathological seeds.
 */
#include "rando_fill.h"
#include "rando_state.h"
#include "rando_rules.h"
#include "rando_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER)
#define _strdup strdup
#endif

#define FILL_MAX_ATTEMPTS   8     /* outer shuffle->fill->validate retries */

/* ------------------------------------------------------------------ */
/* RNG: splitmix64 — deterministic, tiny, good enough for shuffles     */
/* ------------------------------------------------------------------ */

typedef struct FillRng { uint64_t s; } FillRng;

static uint64_t rng_next(FillRng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ULL;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* unbiased [0, n) */
static int rng_below(FillRng *r, int n)
{
    uint64_t m = (uint64_t)n;
    uint64_t bound = UINT64_MAX - (UINT64_MAX % m);
    uint64_t x;
    do { x = rng_next(r); } while (x >= bound);
    return (int)(x % m);
}

static void shuffle_ints(FillRng *r, int *a, int n)
{
    int i;
    for (i = n - 1; i > 0; i--) {
        int j = rng_below(r, i + 1);
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

/* ------------------------------------------------------------------ */
/* Fill context                                                        */
/* ------------------------------------------------------------------ */

typedef struct FillCtx {
    RandoWorld *w;
    RandoFillStats *stats;
    FillRng     rng;
    RandoState *st;

    int    *place;        /* [n_locations] current trial placement       */
    int    *orig;         /* [n_locations] dump placement (for restore)  */
    uint8_t *is_eligible; /* [n_locations] 1 = shuffle-eligible          */
    int    *eligible;     /* [n_eligible] eligible location ids          */
    int    *loc_order;    /* shuffled view of eligible                   */
    int    *pool;         /* [n_pool] expanded pool item ids             */
    int    *pool_order;   /* shuffled view of pool                       */
    int    *prog;         /* progression subset (shuffled order)         */
    int    *junk;         /* junk subset (shuffled order)                */
    int    *assumed;      /* scratch: ids for the max-exploration sweep  */
    int    *pp_loc;       /* fill-placed progression locations so far    */
    int    *placed_count; /* [n_items] installed copies per item id      */

    int n_locations, n_eligible, n_pool;
    int n_prog, n_junk, n_assumed, n_pp;
    int last_max_reach;   /* debug: regions reachable in the last max sweep */
    int empty_max_reach;  /* debug: regions reachable with the whole pool  */
    int debug;
} FillCtx;

static void fill_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz) snprintf(err, errsz, "%s", msg);
}

/* install / uninstall mirror `place` into the world so the existing sweep
 * (which reads locations[].placed_item) sees the trial placement */
static void install(FillCtx *c, int loc, int item)
{
    c->w->locations[loc].placed_item = item;
    c->w->locations[loc].placed_progression = c->w->items[item].progression;
    c->place[loc] = item;
    c->placed_count[item]++;
}

static void uninstall(FillCtx *c, int loc)
{
    int item = c->place[loc];
    if (item >= 0) c->placed_count[item]--;
    c->w->locations[loc].placed_item = -1;
    c->w->locations[loc].placed_progression = 0;
    c->place[loc] = -1;
}

/* max-exploration assumption list: EVERY pool copy — placed or not.  The
 * assumed state must assume everything the finished fill will deliver;
 * relying on location-driven collection for the placed subset lets the max
 * sweep itself circularly stall (observed: 277/933 regions instead of 918
 * on the first run of this code). */
static void build_assumed(FillCtx *c)
{
    int id;
    c->n_assumed = 0;
    for (id = 0; id < c->w->n_items; id++) {
        int total = c->w->items[id].pool_count;
        while (total-- > 0 && c->n_assumed < c->n_pool)
            c->assumed[c->n_assumed++] = id;
    }
}

/* one full fixpoint sweep; returns reachable-region count.
 * skip_item is -1 (disable) — 0 would silently skip every location holding
 * item id 0 ("Arrows (10)"), which is a real pool item here. */
static int run_sweep(FillCtx *c, const int *collect_ids, int n_collect)
{
    int i, nreach = 0;
    c->stats->sweeps++;
    Rando_StateReset(c->st);
    for (i = 0; i < n_collect; i++)
        Rando_StateCollectItem(c->st, collect_ids[i]);
    Rando_SweepReachability(c->st, 1, -1);
    for (i = 0; i < c->w->n_regions; i++) nreach += c->st->reachable[i];
    return nreach;
}

/* strict sphere check: with the current (partial, non-assumed) placements,
 * every previously fill-placed progression item must be obtainable
 * (its location collected by the sweep). */
static int strict_check(FillCtx *c)
{
    int i;
    run_sweep(c, NULL, 0);
    for (i = 0; i < c->n_pp; i++)
        if (!c->st->collected[c->pp_loc[i]]) return 0;
    return 1;
}

static int victory_reached(FillCtx *c)
{
    int vic = c->w->victory_item;
    int goal = c->w->goal_location;
    if (vic >= 0 && c->st->count[vic] == 0) return 0;
    if (goal >= 0 && !Rando_StateLocationReachable(c->st, goal)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* vanilla table + eligible subset                                     */
/* ------------------------------------------------------------------ */

static int loc_is_bottle(const RandoWorld *w, int item_id)
{
    const char *nm;
    if (item_id < 0 || item_id >= w->n_items) return 0;
    nm = w->items[item_id].name;
    return nm && strncmp(nm, "Bottle", 6) == 0;
}

static int pool_has_bottle(const RandoWorld *w)
{
    int i;
    for (i = 0; i < w->n_items; i++)
        if (w->items[i].pool_count > 0 && loc_is_bottle(w, i)) return 1;
    return 0;
}

/* KEY DROPS: 1 while the loaded file shuffles the reference's fourteen enemy
 * key drops (dropshuffle=keys).  Default 0 = the pre-KEY-DROPS behaviour. */
static int s_rando_dropshuffle;

void Rando_SetDropShuffle(int on)
{
    s_rando_dropshuffle = (on != 0);
}

/* POTS: 1 while the loaded file shuffles the reference's nineteen pot keys
 * (pottery=keys).  Default 0 = the pre-POTS behaviour. */
static int s_rando_potshuffle;

void Rando_SetPotShuffle(int on)
{
    s_rando_potshuffle = (on != 0);
}

/* BONK DROPS: 1 while the loaded file shuffles the reference's 42 bonk /
 * tree-pull prizes (the bare flag bonk_drops).  Default 0 = the pre-BONK
 * DROPS behaviour. */
static int s_rando_bonkshuffle;

void Rando_SetBonkShuffle(int on)
{
    s_rando_bonkshuffle = (on != 0);
}

/* Load vanilla_locations.json from `dir` (cross-check only) and mark the
 * shuffle-eligible locations.
 *
 * Eligibility (phase A, multiset-preserving): a location is eligible iff
 * its dumped item is in the shuffled pool AND the slot is a normal,
 * unforced, unlocked, non-mechanics slot (not shop stock, pot, drop,
 * keydrop or prize).  Clearing exactly those slots removes exactly the
 * pool copies from the world and nothing else — no key, map or prize can
 * go missing.
 *
 * KEY DROPS = SHUFFLED (Rando_SetDropShuffle, the reference's
 * dropshuffle=keys) is the one documented exception: the dump then leaves
 * the fourteen "... Key Drop" locations unforced, and the engine CAN hand
 * out whatever lands on them (randomizer.c Randomizer_KeyDropReceipt, from
 * the sprite death / pickup path), so they join the eligible set on the
 * same terms as any chest.  The multiset argument is unchanged: only slots
 * whose dumped item is a pool copy are cleared.  Every drop whose dumped
 * item is NOT in the pool — with SMALL KEYS on OWN DUNGEON that is most
 * of them, the dungeon items being outside the shuffled pool — stays a
 * fixed placement and is handed out from the dump, exactly as the fill
 * assumed.
 *
 * POTS = SHUFFLED (Rando_SetPotShuffle, the reference's pottery=keys) is the
 * same story for the nineteen "... Pot Key" locations, which the engine can
 * now hand out from Sprite_SpawnSecret's key.  BONK DROPS = SHUFFLED
 * (Rando_SetBonkShuffle, the reference's bare bonk_drops flag) is the same
 * again for the 42 bonk / tree-pull prize locations, which the engine hands
 * out from Sprite_HandleAbsorptionByPlayer after respawning the hidden
 * overworld prize as the green-rupee placeholder (see the BONK DROPS block in
 * randomizer.c).  Prizes are still refused: nothing in the engine can hand
 * those out yet.
 *
 * The naive vanilla-table rule ("clear every slot whose VANILLA item is in
 * the pool") was tried first and is WRONG for this purpose: the fork parks
 * dungeon items on such slots while shuffling (25 of them in seed 1234,
 * including small/big keys), so clearing by the vanilla rule deletes keys
 * and the world cannot beat the game.  The vanilla table is still read to
 * quantify that overlap (stats->vanilla_skipped).  See RANDO_FILL.md. */
static int load_eligible(RandoWorld *w, const char *dir, FillCtx *c,
                         uint8_t *eligible_flags, RandoFillStats *stats,
                         char *err, size_t errsz)
{
    char path[1024];
    int i;

    for (i = 0; i < w->n_locations; i++) {
        const RandoLocation *l = &w->locations[i];
        int item = l->placed_item;
        if (item < 0 || w->items[item].pool_count <= 0) continue;
        if (l->locked || l->forced_item >= 0) continue;
        if (l->type == RANDO_LOC_PRIZE) continue;
        if (l->type == RANDO_LOC_POT && !s_rando_potshuffle) continue;
        if (l->type == RANDO_LOC_DROP && !s_rando_dropshuffle) continue;
        if (l->type == RANDO_LOC_BONK && !s_rando_bonkshuffle) continue;
        if (!c->is_eligible[i]) {
            c->is_eligible[i] = 1;
            c->eligible[c->n_eligible++] = i;
        }
    }

    if (eligible_flags) {
        for (i = 0; i < c->n_locations; i++) eligible_flags[i] = 0;
        for (i = 0; i < c->n_eligible; i++) eligible_flags[c->eligible[i]] = 1;
    }

    /* vanilla-table cross-check */
    stats->vanilla_skipped = 0;
    {
        FILE *f;
        long sz;
        char *text;
        JsonValue *dom, *ent;
        int any_bottle = pool_has_bottle(w);
        snprintf(path, sizeof(path), "%s/vanilla_locations.json", dir);
        f = fopen(path, "rb");
        if (!f) return 0;                    /* cross-check is optional */
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = (char *)malloc((size_t)sz + 1);
        if (!text) { fclose(f); return 0; }
        if (fread(text, 1, (size_t)sz, f) != (size_t)sz) {
            free(text); fclose(f); return 0;
        }
        text[sz] = '\0';
        fclose(f);
        dom = Json_Parse(text, err, errsz);
        free(text);
        if (!dom) { err[0] = '\0'; return 0; }
        {
            const JsonValue *locs = Json_Get(dom, "locations");
            if (locs && locs->type == JSON_ARRAY) {
                for (ent = locs->child; ent; ent = ent->next) {
                    const char *locname = Json_AsString(Json_Get(ent, "location"));
                    const char *picked  = Json_AsString(Json_Get(ent, "picked"));
                    int loc;
                    if (!locname || !picked) continue;
                    loc = Rando_LocationId(w, locname);
                    if (loc < 0) continue;
                    if (!(Rando_ItemId(w, picked) >= 0 &&
                          w->items[Rando_ItemId(w, picked)].pool_count > 0) &&
                        !(any_bottle && strncmp(picked, "Bottle", 6) == 0))
                        continue;
                    if (!c->is_eligible[loc])
                        stats->vanilla_skipped++;
                }
            }
        }
        Json_Free(dom);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* one attempt: clear -> shuffle -> progression -> junk -> validate    */
/* ------------------------------------------------------------------ */

static int fill_attempt(FillCtx *c, char *err, size_t errsz)
{
    int i, k;

    /* 1. clear the eligible slots */
    for (i = 0; i < c->n_eligible; i++) {
        int loc = c->eligible[i];
        c->w->locations[loc].placed_item = -1;
        c->w->locations[loc].placed_progression = 0;
        c->place[loc] = -1;
    }
    memset(c->placed_count, 0, (size_t)c->w->n_items * sizeof(int));
    c->n_pp = 0;

    /* debug probe: assumed sweep with the ENTIRE pool pre-collected */
    if (c->debug) {
        build_assumed(c);
        c->empty_max_reach = run_sweep(c, c->assumed, c->n_assumed);
        printf("    [debug] probe: assumed full-pool sweep -> %d / %d regions\n",
               c->empty_max_reach, c->w->n_regions);
    }

    /* 2. shuffle the pool once, then stable-partition progression / junk
     *    (distribute_items_restrictive: random.shuffle(itempool), then the
     *    progitempool / restitempool comprehensions) */
    shuffle_ints(&c->rng, c->pool_order, c->n_pool);
    c->n_prog = c->n_junk = 0;
    for (i = 0; i < c->n_pool; i++) {
        int item = c->pool[c->pool_order[i]];
        if (c->w->items[item].progression) c->prog[c->n_prog++] = item;
        else                               c->junk[c->n_junk++] = item;
    }
    shuffle_ints(&c->rng, c->loc_order, c->n_eligible);

    /* 3. progression: frontier placement with strict validation.
     *
     * The literal Fill.py check (candidate reachable in the "everything
     * assumed" max state) alone is NOT sound for a from-scratch port: it
     * happily places e.g. Moon Pearl inside Ganon's Tower, whose real
     * access needs the pearls/dungeons that the remaining pool was only
     * ASSUMED to provide — a circular dependency the final sweep can never
     * unwind (measured: final sweep stalls at 151-396/933 regions).
     * The sound variant — and what this module implements — places each
     * progression item at a location reachable in the CURRENT partial
     * world (the frontier of unfilled reachable locations), then validates
     * that every fill-placed progression item so far (including the new
     * one) is obtainable in a fresh sweep; rollback and next candidate on
     * failure.  Reachability is monotone while items are added, so every
     * placed item stays obtainable forever: the final sweep collects the
     * whole pool and the seed is completable by construction. */
    for (i = 0; i < c->n_prog; i++) {
        int item = c->prog[i];
        int chosen = -1;

        /* the real frontier; its reach feeds the failure diagnostics in
         * step 5 (last_max_reach was never assigned, so failures always
         * printed 0 there) */
        c->last_max_reach = run_sweep(c, NULL, 0);

        for (k = 0; k < c->n_eligible; k++) {
            int loc = c->eligible[c->loc_order[k]];
            if (c->place[loc] >= 0) continue;
            if (!Rando_StateLocationReachable(c->st, loc)) continue;
            c->stats->candidate_tests++;
            install(c, loc, item);
            c->pp_loc[c->n_pp++] = loc;
            if (strict_check(c)) { chosen = loc; break; }
            /* paranoia path (monotonicity forbids it): roll back */
            c->n_pp--;
            uninstall(c, loc);
            c->stats->sphere_escapes++;
        }
        if (chosen < 0) {
            fill_err(err, errsz,
                     "frontier exhausted: no reachable slot for a progression item");
            return -1;
        }
    }
    c->stats->prog_items = (unsigned)c->n_prog;

    /* 4. junk: fast_fill — blind placement, no logic checks */
    {
        int li = 0;
        for (i = 0; i < c->n_junk; i++) {
            while (li < c->n_eligible && c->place[c->eligible[c->loc_order[li]]] >= 0)
                li++;
            if (li >= c->n_eligible) {
                fill_err(err, errsz, "ran out of locations during fast fill");
                return -1;
            }
            install(c, c->eligible[c->loc_order[li]], c->junk[i]);
            c->stats->junk_items++;
        }
    }

    /* 5. final validation: everything really placed — must beat the game */
    run_sweep(c, NULL, 0);
    if (!victory_reached(c)) {
        fill_err(err, errsz, "completed fill did not validate as beatable");
        if (getenv("RANDO_FILL_DEBUG")) {
            const RandoWorld *w = c->w;
            int id, loc, nreach = 0, stranded = 0;
            static const char *const gates[] = {
                "Lamp", "Hammer", "Moon Pearl", "Hookshot", "Flippers",
                "Fighter Sword", "Master Sword", "Tempered Sword", "Golden Sword",
                "Power Glove", "Titans Mitts", "Bow", "Silver Arrows",
                "Fire Rod", "Ice Rod", "Beat Agahnim 1", "Beat Agahnim 2",
                "Magic Mirror", "Book of Mudora",
            };
            size_t gi;
            printf("    [debug] last max sweep: %d / %d regions\n",
                   c->last_max_reach, c->w->n_regions);
            for (id = 0; id < w->n_regions; id++) nreach += c->st->reachable[id];
            printf("    [debug] reachable regions: %d / %d\n", nreach, w->n_regions);
            for (gi = 0; gi < sizeof(gates) / sizeof(gates[0]); gi++) {
                id = Rando_ItemId(w, gates[gi]);
                if (id >= 0 && c->st->count[id])
                    printf("    [debug]   have %s x%d\n", gates[gi], c->st->count[id]);
            }
            {
                int cr;
                int nc = 0;
                static const char *const evs[] = {
                    "Farmable Bombs", "Farmable Rupees", "Return Smith",
                    "Get Frog", "Beat Agahnim 1", "Beat Agahnim 2",
                };
                for (gi = 0; gi < sizeof(evs) / sizeof(evs[0]); gi++) {
                    id = Rando_ItemId(w, evs[gi]);
                    printf("    [debug]   counter %-16s = %d\n", evs[gi],
                           id >= 0 ? c->st->count[id] : -1);
                }
                for (cr = 1; cr <= 7; cr++) {
                    char nm[32];
                    snprintf(nm, sizeof(nm), "Crystal %d", cr);
                    id = Rando_ItemId(w, nm);
                    if (id >= 0 && c->st->count[id]) nc++;
                }
                printf("    [debug]   crystals: %d/7\n", nc);
                {
                    int kr = Rando_RegionId(w, "Kakariko Village");
                    int kl = Rando_LocationId(w, "Kakariko Village Bush Drop");
                    printf("    [debug]   Kakariko Village region=%d reachable=%d; "
                           "Bush Drop loc=%d collected=%d\n",
                           kr, kr >= 0 ? c->st->reachable[kr] : -1,
                           kl, kl >= 0 ? c->st->collected[kl] : -1);
                }
                {
                    static const char *const trace[] = {
                        "Book of Mudora", "Moon Pearl", "Hammer", "Hookshot",
                        "Flippers", "Bombs (10)", "Pegasus Boots", "Lamp",
                    };
                    for (gi = 0; gi < sizeof(trace) / sizeof(trace[0]); gi++) {
                        id = Rando_ItemId(w, trace[gi]);
                        if (id < 0) continue;
                        for (i = 0; i < c->n_eligible; i++) {
                            int l2 = c->eligible[i];
                            if (c->place[l2] != id) continue;
                            printf("    [debug]   %-16s @ %-38s region %-34s reachable=%d collected=%d\n",
                                   trace[gi], w->locations[l2].name,
                                   w->regions[w->locations[l2].region].name,
                                   c->st->reachable[w->locations[l2].region],
                                   c->st->collected[l2]);
                        }
                    }
                }
            }
            for (i = 0; i < c->n_eligible; i++) {
                loc = c->eligible[i];
                if (c->place[loc] >= 0 && !c->st->collected[loc]) {
                    const RandoLocation *l = &w->locations[loc];
                    if (stranded < 12)
                        printf("    [debug]   stranded: %s <- %s (region %s: %s, rule %s)\n",
                               l->name, w->items[c->place[loc]].name,
                               w->regions[l->region].name,
                               c->st->reachable[l->region] ? "reachable" : "UNREACHABLE",
                               l->rule >= 0 ? "eval" : "true");
                    stranded++;
                }
            }
            printf("    [debug] stranded eligible locations: %d / %d\n",
                   stranded, c->n_eligible);
            /* convergence probe: keep collecting manually — if this moves
             * the state, the sweep driver terminated early */
            {
                int extra_rounds = 0, extra_regions;
                int before = nreach;
                while (Rando_StateCollectPass(c->st, -1)) {
                    Rando_SweepReachability(c->st, 0, 0);
                    extra_rounds++;
                    if (extra_rounds > 200) break;
                }
                extra_regions = 0;
                for (i = 0; i < w->n_regions; i++) extra_regions += c->st->reachable[i];
                printf("    [debug] manual convergence: +rounds=%d, regions %d -> %d\n",
                       extra_rounds, before, extra_regions);
            }
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* phase B2: the demo-chest pin                                        */
/* ------------------------------------------------------------------ */

/* Pre-place `pin_item` at `pin_location` and treat that slot as fixed
 * content (see rando_fill.h for the contract).  Called after load_eligible,
 * before the attempts loop: it removes the slot from the eligible set and
 * one copy of the item from the pool, so the clear/refill cycle inside every
 * attempt never touches the pin while eligible-slots == pool-size still
 * holds.  Any problem -> the pin is ignored with a note in
 * stats->pin_note and the unpinned fill runs. */
static void apply_pin(RandoWorld *w, FillCtx *c, const char *pin_location,
                      const char *pin_item, uint8_t *eligible_flags,
                      RandoFillStats *stats)
{
    int pl = Rando_LocationId(w, pin_location);
    int pi = pin_location && pin_item ? Rando_ItemId(w, pin_item) : -1;
    int i;

    stats->pin_applied = 0;
    snprintf(stats->pin_note, sizeof(stats->pin_note), "pin ignored");

    if (pl < 0) {
        snprintf(stats->pin_note, sizeof(stats->pin_note),
                 "pin ignored: location '%s' not in the dump", pin_location);
        return;
    }
    if (pi < 0) {
        snprintf(stats->pin_note, sizeof(stats->pin_note),
                 "pin ignored: item '%s' not in the dump", pin_item);
        return;
    }
    for (i = 0; i < c->n_eligible && c->eligible[i] != pl; i++) {}
    if (i >= c->n_eligible) {
        snprintf(stats->pin_note, sizeof(stats->pin_note),
                 "pin ignored: '%s' is not a shuffle-eligible slot", pin_location);
        return;
    }
    if (w->items[pi].pool_count <= 0) {
        snprintf(stats->pin_note, sizeof(stats->pin_note),
                 "pin ignored: '%s' is not in the shuffled pool", pin_item);
        return;
    }
    /* find one copy of the pinned item in the expanded pool */
    for (i = 0; i < c->n_pool && c->pool[i] != pi; i++) {}
    if (i >= c->n_pool) {   /* cannot happen after the pool_count check */
        snprintf(stats->pin_note, sizeof(stats->pin_note),
                 "pin ignored: '%s' copy missing from the pool", pin_item);
        return;
    }

    /* the pinned slot leaves the eligible set */
    {
        int e;
        for (e = 0; e < c->n_eligible && c->eligible[e] != pl; e++) {}
        c->eligible[e] = c->eligible[c->n_eligible - 1];
        c->n_eligible--;
    }
    c->is_eligible[pl] = 0;
    if (eligible_flags) eligible_flags[pl] = 2;   /* JSON origin "pin" */

    /* drop exactly one copy of the pinned item from the pool */
    c->pool[i] = c->pool[c->n_pool - 1];
    c->n_pool--;
    for (i = 0; i < c->n_pool; i++) c->pool_order[i] = i;

    /* pre-place the pin; fill_attempt clears eligible slots only, and both
     * the frontier and the junk passes skip slots with place >= 0, so the
     * pin survives every attempt untouched */
    w->locations[pl].placed_item = pi;
    w->locations[pl].placed_progression = w->items[pi].progression;
    c->place[pl] = pi;

    stats->pin_applied = 1;
    snprintf(stats->pin_note, sizeof(stats->pin_note),
             "%s = %s pre-placed before the fill (pool copy consumed, "
             "slot excluded from the shuffle)", pin_location, pin_item);
}

/* ------------------------------------------------------------------ */
/* public entry                                                        */
/* ------------------------------------------------------------------ */

/* timing (mirrors rando_test.c) */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double fill_now_ms(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <time.h>
static double fill_now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}
#endif

int Rando_FillWorld(RandoWorld *world, const char *seed_data_dir,
                    unsigned int seed,
                    int *placements, uint8_t *eligible_flags,
                    RandoFillStats *stats, char *err, size_t errsz)
{
    return Rando_FillWorldEx(world, seed_data_dir, seed, placements,
                             eligible_flags, stats, err, errsz, NULL, NULL);
}

int Rando_FillWorldEx(RandoWorld *world, const char *seed_data_dir,
                      unsigned int seed,
                      int *placements, uint8_t *eligible_flags,
                      RandoFillStats *stats, char *err, size_t errsz,
                      const char *pin_location, const char *pin_item)
{
    FillCtx c;
    RandoFillStats local_stats;
    int i, rc = -1;
    double t0;

    if (err && errsz) err[0] = '\0';
    if (!stats) stats = &local_stats;
    memset(stats, 0, sizeof(*stats));
    stats->seed = seed;

    if (!world || world->n_locations <= 0 || world->n_items <= 0) {
        fill_err(err, errsz, "world not loaded");
        return -1;
    }

    memset(&c, 0, sizeof(c));
    c.w = world;
    c.stats = stats;
    c.n_locations = world->n_locations;
    c.debug = getenv("RANDO_FILL_DEBUG") != NULL;
    c.rng.s = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)seed * 0xff51afd7ed558ccdULL + 0x9E3779B97F4A7C15ULL);
    (void)rng_next(&c.rng);

    /* allocate scratch (pool arrays sized to the exact expansion) */
    {
        int total_pool = 0;
        for (i = 0; i < world->n_items; i++)
            total_pool += world->items[i].pool_count;
        c.n_pool = total_pool;
    }
    c.place        = (int *)calloc((size_t)c.n_locations, sizeof(int));
    c.orig         = (int *)calloc((size_t)c.n_locations, sizeof(int));
    c.is_eligible  = (uint8_t *)calloc((size_t)c.n_locations, 1);
    c.eligible     = (int *)calloc((size_t)c.n_locations, sizeof(int));
    c.loc_order    = (int *)calloc((size_t)c.n_locations, sizeof(int));
    c.pp_loc       = (int *)calloc((size_t)c.n_locations, sizeof(int));
    c.pool         = (int *)calloc((size_t)(c.n_pool ? c.n_pool : 1), sizeof(int));
    c.pool_order   = (int *)calloc((size_t)(c.n_pool ? c.n_pool : 1), sizeof(int));
    c.prog         = (int *)calloc((size_t)(c.n_pool ? c.n_pool : 1), sizeof(int));
    c.junk         = (int *)calloc((size_t)(c.n_pool ? c.n_pool : 1), sizeof(int));
    c.assumed      = (int *)calloc((size_t)(c.n_pool ? c.n_pool : 1), sizeof(int));
    c.placed_count = (int *)calloc((size_t)world->n_items, sizeof(int));
    if (!c.place || !c.orig || !c.is_eligible || !c.eligible || !c.loc_order ||
        !c.pp_loc || !c.pool || !c.pool_order || !c.prog || !c.junk ||
        !c.assumed || !c.placed_count) {
        fill_err(err, errsz, "oom (fill scratch)");
        goto done;
    }

    /* snapshot dump placements; init trial table to them */
    for (i = 0; i < c.n_locations; i++) {
        c.orig[i] = world->locations[i].placed_item;
        c.place[i] = world->locations[i].placed_item;
    }

    /* expand pool */
    {
        int k = 0, j;
        for (i = 0; i < world->n_items; i++)
            for (j = 0; j < world->items[i].pool_count; j++) {
                c.pool[k] = i;
                c.pool_order[k] = k;
                k++;
            }
        c.n_pool = k;
    }
    stats->pool_size = (unsigned)c.n_pool;

    /* vanilla table -> eligible subset (+ cross-check) */
    if (load_eligible(world, seed_data_dir, &c, eligible_flags, stats, err, errsz) != 0)
        goto done;

    /* phase B2: pre-place the demo-chest pin (shrinks eligible + pool by one
     * each, keeping the equality check below valid; no-op without a pin) */
    if (pin_location && pin_location[0] && pin_item && pin_item[0])
        apply_pin(world, &c, pin_location, pin_item, eligible_flags, stats);

    stats->eligible = (unsigned)c.n_eligible;
    for (i = 0; i < c.n_eligible; i++) c.loc_order[i] = i;
    if (c.n_eligible != c.n_pool) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "eligible locations (%d) != pool size (%d)", c.n_eligible, c.n_pool);
        fill_err(err, errsz, buf);
        goto done;
    }

    c.st = Rando_StateNew(world);
    if (!c.st) { fill_err(err, errsz, "oom (state)"); goto done; }

    /* attempts */
    t0 = fill_now_ms();
    for (i = 0; i < FILL_MAX_ATTEMPTS; i++) {
        stats->attempts = i + 1;
        if (fill_attempt(&c, err, errsz) == 0) { rc = 0; break; }
        /* roll the whole attempt back (restore cleared/installed slots) */
        {
            int k;
            for (k = 0; k < c.n_eligible; k++) {
                int loc = c.eligible[k];
                world->locations[loc].placed_item = c.orig[loc];
                world->locations[loc].placed_progression =
                    c.orig[loc] >= 0 ? world->items[c.orig[loc]].progression : 0;
                c.place[loc] = c.orig[loc];
            }
            memset(c.placed_count, 0, (size_t)world->n_items * sizeof(int));
            c.n_pp = 0;
        }
    }
    stats->fill_ms = fill_now_ms() - t0;

    if (rc == 0 && placements) {
        int k;
        for (k = 0; k < c.n_locations; k++)
            placements[k] = (c.place[k] >= 0) ? c.place[k] : c.orig[k];
    }

done:
    /* restore the world to its dump state on every path */
    if (c.orig) {
        int k;
        for (k = 0; k < c.n_locations; k++) {
            world->locations[k].placed_item = c.orig[k];
            world->locations[k].placed_progression =
                c.orig[k] >= 0 ? world->items[c.orig[k]].progression : 0;
        }
    }
    if (c.st) Rando_StateFree(c.st);
    free(c.place); free(c.orig); free(c.is_eligible); free(c.eligible);
    free(c.loc_order); free(c.pp_loc); free(c.pool); free(c.pool_order);
    free(c.prog); free(c.junk); free(c.assumed); free(c.placed_count);
    return rc;
}

/* ------------------------------------------------------------------ */
/* can-beat wrapper + placement utilities                              */
/* ------------------------------------------------------------------ */

int Rando_CanBeat(RandoWorld *world, const int *placements)
{
    int *saved = NULL;
    uint8_t *saved_prog = NULL;
    RandoState *st;
    int i, ok = 0;

    if (placements) {
        saved = (int *)malloc((size_t)world->n_locations * sizeof(int));
        saved_prog = (uint8_t *)malloc((size_t)world->n_locations);
        if (!saved || !saved_prog) { free(saved); free(saved_prog); return 0; }
        for (i = 0; i < world->n_locations; i++) {
            saved[i] = world->locations[i].placed_item;
            saved_prog[i] = world->locations[i].placed_progression;
            if (world->locations[i].placed_item != placements[i]) {
                world->locations[i].placed_item = placements[i];
                world->locations[i].placed_progression =
                    placements[i] >= 0 ? world->items[placements[i]].progression : 0;
            }
        }
    }

    st = Rando_StateNew(world);
    if (st) {
        Rando_SweepReachability(st, 1, -1);   /* -1: collect every location */
        ok = world->victory_item >= 0 && st->count[world->victory_item] > 0;
        if (ok && world->goal_location >= 0)
            ok = Rando_StateLocationReachable(st, world->goal_location);
        Rando_StateFree(st);
    }

    if (placements) {
        for (i = 0; i < world->n_locations; i++) {
            world->locations[i].placed_item = saved[i];
            world->locations[i].placed_progression = saved_prog[i];
        }
        free(saved);
        free(saved_prog);
    }
    return ok;
}

int Rando_FillApply(RandoWorld *world, const int *placements)
{
    int i;
    if (!world || !placements) return -1;
    for (i = 0; i < world->n_locations; i++) {
        world->locations[i].placed_item = placements[i];
        world->locations[i].placed_progression =
            placements[i] >= 0 ? world->items[placements[i]].progression : 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* phase-B apply support: model item -> engine chest byte              */
/* ------------------------------------------------------------------ */

/* Generated fork-item table (ALTTPR name -> ROM item code, see
 * tools/gen_rando_tables.py).  Included under a guard because the unity
 * TU (randomizer.c includes this file AND the table) must see it exactly
 * once - randomizer.c's own include site uses the same guard.  The table
 * uses the engine's `uint16` type name: in the unity TU src/types.h is
 * already in scope (assets.h), here we provide the identical typedef. */
#ifndef RANDO_TABLES_INCLUDED
#define RANDO_TABLES_INCLUDED
#ifndef ZELDA3_TYPES_H_
typedef uint16_t uint16;
#endif
#include "../rando_tables.inc"
#endif

/* ---- keysanity: dungeon-targeted key / big key / map / compass ---------
 *
 * With the RANDOMIZER page's SMALL KEYS / BIG KEYS / MAPS / COMPASSES rows
 * on ANYWHERE (reference keyshuffle/bigkeyshuffle/mapshuffle/compassshuffle
 * = wild) the fill hands out items named for a dungeon that is NOT the one
 * the chest or the NPC sits in - "Small Key (Eastern Palace)" out of a
 * Desert Palace chest, "Map (Escape)" from the Sick Kid.  The engine's
 * vanilla receipts 0x24 / 0x32 / 0x33 / 0x25 always credit whatever dungeon
 * Link is standing in, so they cannot express that; before this they were
 * handed out anyway and silently credited the wrong dungeon.
 *
 * So: one extra receipt code per (class, dungeon),
 *
 *     code = 0x4C + 13 * class + (dungeon - 1)
 *
 *       class 0  small key   0x4C .. 0x58
 *       class 1  big key     0x59 .. 0x65
 *       class 2  map         0x66 .. 0x72
 *       class 3  compass     0x73 .. 0x7F
 *
 * `dungeon` is the game's palace index (cur_palace_index_x2 / 2), 1..13.
 * The reference never names palace 0 (Sewers): its "Escape" is Hyrule
 * Castle AND the sewers, which the engine runs as palaces 1 and 0 sharing
 * one key counter, so "Escape" maps to 1 and the engine-side grant
 * (Randomizer_DungeonItemGrant, randomizer.c) credits both.
 *
 * Why exactly 0x4C..0x7F, with nothing to spare: the engine's receipt
 * tables (misc.c kMemoryLocationToGiveItemTo / kReceiveItemGfx, player.c
 * kReceiveItemAlternates) are 76 = 0x4C entries, so every code below 0x4C
 * is taken, and Link_PerformOpenChest reads a chest byte with bit 7 set as
 * "no item" (sign8), so 0x80 and up is unusable in a chest.  0x4C..0x7F is
 * 52 codes = 4 classes * 13 dungeons - an exact fit.  These codes overlap
 * the FORK's own ROM item codes (rando_tables.inc, e.g. 0x50 Master Sword):
 * that is harmless because the two never meet - Rando_ApplyChestCode
 * resolves dungeon items by NAME below, before the kRandoItemCodes lookup
 * can ever return a fork code, and fork codes >= 0x4C are still refused.
 *
 * The plain vanilla codes stay in use for OWN DUNGEON files and for
 * "Small Key (Universal)" (see Rando_SetKeysanity): there the item really
 * is for wherever Link is, and the fill's behaviour is unchanged.  Which is
 * also why UNIVERSAL needs no code of its own - and could not have one, the
 * range being an exact fit: the engine gives universal files a single global
 * key counter instead, so the plain 0x24 is literally correct.
 */
#define kRandoDungeonCodeBase 0x4c
#define kRandoDungeonCount    13

/* reference dungeon name -> palace index, in code order (index + 1) */
static const char *const kRandoDungeonNames[kRandoDungeonCount] = {
    "Escape",              /*  1 Hyrule Castle (+ Sewers, palace 0) */
    "Eastern Palace",      /*  2 */
    "Desert Palace",       /*  3 */
    "Agahnims Tower",      /*  4 Castle Tower */
    "Swamp Palace",        /*  5 */
    "Palace of Darkness",  /*  6 */
    "Misery Mire",         /*  7 */
    "Skull Woods",         /*  8 */
    "Ice Palace",          /*  9 */
    "Tower of Hera",       /* 10 */
    "Thieves Town",        /* 11 */
    "Turtle Rock",         /* 12 */
    "Ganons Tower",        /* 13 */
};

/* per class: does the loaded file shuffle it outside its home dungeon? */
static int s_rando_ks_wild[4];

void Rando_SetKeysanity(int keys, int bigkeys, int maps, int compasses)
{
    /* keys == 2 is UNIVERSAL: the reference pools one nameless
     * "Small Key (Universal)" item, which has no dungeon to target and so
     * keeps the plain 0x24 (see Rando_LookupEngineCode).  That is the right
     * code for it: under universal keys the engine turns link_num_keys into
     * one shared pool for the whole game (g_rando_universal_keys /
     * Randomizer_UniversalKeysHold, randomizer.c), so a plain 0x24 credits
     * the pool wherever Link picks it up, with the vanilla presentation. */
    s_rando_ks_wild[0] = (keys == 1);
    s_rando_ks_wild[1] = (bigkeys == 1);
    s_rando_ks_wild[2] = (maps == 1);
    s_rando_ks_wild[3] = (compasses == 1);
}

int Rando_DungeonItemIsCode(int code)
{
    return code >= kRandoDungeonCodeBase &&
           code < kRandoDungeonCodeBase + 4 * kRandoDungeonCount;
}

int Rando_DungeonItemClass(int code)
{
    return (code - kRandoDungeonCodeBase) / kRandoDungeonCount;
}

int Rando_DungeonItemDungeon(int code)
{
    return (code - kRandoDungeonCodeBase) % kRandoDungeonCount + 1;
}

const char *Rando_DungeonItemName(int code)
{
    if (!Rando_DungeonItemIsCode(code)) return "";
    return kRandoDungeonNames[Rando_DungeonItemDungeon(code) - 1];
}

int Rando_DungeonItemCode(const char *name)
{
    static const struct { const char *prefix; int len, cls; } kCls[4] = {
        { "Small Key (", 11, 0 }, { "Big Key (", 9, 1 },
        { "Map (", 5, 2 },        { "Compass (", 9, 3 },
    };
    int c, i;
    if (!name) return -1;
    for (c = 0; c < 4; c++) {
        const char *d;
        size_t dl;
        if (strncmp(name, kCls[c].prefix, (size_t)kCls[c].len) != 0)
            continue;
        if (!s_rando_ks_wild[c])
            return -1;            /* OWN DUNGEON: the plain code is right */
        d = name + kCls[c].len;
        dl = strlen(d);
        if (dl == 0 || d[dl - 1] != ')')
            return -1;
        dl--;                     /* drop the closing paren */
        for (i = 0; i < kRandoDungeonCount; i++)
            if (strlen(kRandoDungeonNames[i]) == dl &&
                strncmp(d, kRandoDungeonNames[i], dl) == 0)
                return kRandoDungeonCodeBase + kRandoDungeonCount * c + i;
        return -1;                /* "Small Key (Universal)", or a new name */
    }
    return -1;
}

/* RE-HOMED pool items: names whose fork ROM code (kRandoItemCodes,
 * rando_tables.inc) falls inside the 0x4C..0x7F range this port reserves for
 * the dungeon-targeted receipts, so the fork value cannot be used and
 * Rando_ApplyChestCode used to refuse the name outright ("no engine receipt
 * slot").  Each one is mapped here to a code the engine really can hand out:
 * either a plain VANILLA receipt that does the same thing, or one of the
 * EXTRA codes 0x80..0xFE that Randomizer_ExtraItemBegin (randomizer.c)
 * decodes.  Resolved by NAME, before the kRandoItemCodes lookup, exactly like
 * the dungeon items above.
 *
 *   0x51..0x54  capacity upgrades -> extra 0x81..0x85.  Real pool items:
 *               shopsanity shuffles the Capacity Upgrade shop's stock and
 *               `bombbag` adds two "Bomb Upgrade (+10)" copies as the bag
 *               itself.  "Arrow Upgrade (+70)" is the reference's one-shot
 *               full quiver - listed although no profile dumped so far
 *               places one, because it costs nothing to accept.
 *   0x50        "Master Sword" -> vanilla receipt 0x01.  A real pool item
 *               under `progressive=off`, where the sword chain is dealt out
 *               as the four concrete swords (Fighter 0x49, Master, Tempered
 *               0x02, Golden 0x03 - only Master had no usable code).  0x01 is
 *               the engine's own master-sword receipt, already used as tier 2
 *               of kChainSword below.
 *   0x58        "Silver Arrows" -> extra 0x86.  A real pool item under
 *               `bow_mode=silvers`, where the pool holds "Bow" AND "Silver
 *               Arrows" as two separate items.  The plain silver-bow receipt
 *               0x3b cannot be used directly: it writes link_item_bow = 3,
 *               which would hand a bowless player a bow.  0x86 wears 0x3b for
 *               the pose and graphic and applies the upgrade correctly
 *               (Randomizer_TakeBowUpgrade, randomizer.c).
 *   0x6c        "Triforce Piece" -> extra 0x87.  30 of them under
 *               `goal=triforcehunt`.  The vanilla engine has no triforce
 *               receipt at all, so 0x87 wears the no-op receipt 0x42 with the
 *               pendant sprite and counts into link_triforce_pieces
 *               (Randomizer_TakeTriforcePiece, randomizer.c).
 *
 * Deliberately NOT re-homed (see the round notes): "Magic Upgrade (1/4)",
 * "Rupoor", "Nothing", the three clocks, "Single/Multi RNG", "Progressive Bow
 * (Alt)", "Triforce" and "Power Star".  None of them can be produced by any
 * value of the 31 settings rows this port offers - the clocks need a `timer`
 * row, the RNG items and Power Star are customizer-only, "Triforce" is the
 * locked event item at the pedestal / Murahdahla, and "Progressive Bow (Alt)"
 * only appears in multiworld item links. */
static const struct { const char *name; uint16_t code; } kRandoExtraCodes[] = {
    { "Bomb Upgrade (+5)",   0x81 },
    { "Bomb Upgrade (+10)",  0x82 },
    { "Arrow Upgrade (+5)",  0x83 },
    { "Arrow Upgrade (+10)", 0x84 },
    { "Arrow Upgrade (+70)", 0x85 },
    { "Silver Arrows",       0x86 },
    { "Triforce Piece",      0x87 },
    { "Master Sword",        0x01 },   /* a plain vanilla receipt, not an extra code */
};

int Rando_ExtraItemCode(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < (int)(sizeof(kRandoExtraCodes) / sizeof(kRandoExtraCodes[0])); i++)
        if (strcmp(kRandoExtraCodes[i].name, name) == 0)
            return kRandoExtraCodes[i].code;
    return -1;
}

int Rando_LookupEngineCode(const char *name)
{
    int i;
    if (!name) return -1;
    /* dungeon keys/maps/compasses: the fork names them per dungeon (their
     * codes are >= 0x4C with no engine receipt); the engine's vanilla
     * native codes are what an OWN DUNGEON file wants, because the item is
     * then always for the dungeon Link is standing in.  A file that shuffles
     * the class ANYWHERE never reaches here - Rando_ApplyChestCode resolves
     * it through Rando_DungeonItemCode first.  "Small Key (Universal)" does
     * reach here and stays a plain 0x24, which the engine credits to the
     * one shared key pool (see Rando_SetKeysanity). */
    if (strncmp(name, "Small Key", 9) == 0) return 0x24;
    if (strncmp(name, "Big Key", 7) == 0) return 0x32;
    if (strncmp(name, "Map (", 5) == 0) return 0x33;
    if (strncmp(name, "Compass (", 9) == 0) return 0x25;
    for (i = 0; i < (int)(sizeof(kRandoItemCodes) / sizeof(kRandoItemCodes[0])); i++)
        if (strcmp(kRandoItemCodes[i].name, name) == 0)
            return kRandoItemCodes[i].code;
    return -1;
}

/* Progressive chain -> concrete engine receipt bytes, 0-based copy index
 * -> tier.  Tier order cross-checked against the reference fork's own
 * item_alternates table (ALttPDoorRandomizer ItemList.py), which names the
 * concrete item each progressive tier stands for:
 *   Progressive Sword   1-4: Fighter, Master, Tempered, Golden
 *   Progressive Glove   1-2: Power Glove, Titans Mitts
 *   Progressive Bow     1-2: Bow, Silver Arrows
 *   Progressive Shield  1-3: Blue, Red, Mirror
 *   Progressive Armor   1-2: Blue Mail, Red Mail   (the MAIL chain in this
 *                        fork - ItemList.py maps Blue/Red Mail onto it)
 *   Magic Upgrade (1/2)   1: half magic
 *
 * Concrete ids verified against the ENGINE receipt tables (misc.c
 * kMemoryLocationToGiveItemTo / kValueToGiveItemTo, names in tracker.c;
 * all < 0x4C so every 76-entry receipt table stays in range):
 *   sword:  0x49 Fighter (fork agrees; 0x00 "Sword and Shield" would also
 *           grant a shield), 0x01 Master (the fork's own 0x50 is out of
 *           receipt range), 0x02 Tempered, 0x03 Golden (fork agrees)
 *   glove:  0x1B, 0x1C (fork agrees)
 *   bow:    0x0B Bow (fork agrees); silver = 0x3B (writes f340 <- 3, the
 *           "bow & silver arrows" receipt; the fork's 0x58 is >= 0x4C)
 *   shield: 0x04, 0x05, 0x06 (fork agrees)
 *   mail:   0x22 Blue (the engine's own progressive-mail receipt),
 *           0x23 Red (fork agrees on both)
 *   magic:  0x80 - an EXTRA code above the vanilla receipts (0x42 turned out
 *           to be the live shop-heart receipt: ancilla.c fills a heart on it,
 *           so the old mapping gave half magic with every shop heart).
 *           Link_ReceiveItem decodes 0x80 to the heart's presentation with
 *           the half-magic effect instead (Randomizer_ExtraItemBegin);
 *           chests accept it because only 0xff means "no item" now.
 *   (was)   0x42 - a receipt that wrote NOTHING in the vanilla engine
 *           (value -1, no bottle match) and is granted by no vanilla data
 *           (verified against the US 1.0 chest table in zelda3.sfc);
 *           misc.c now gives it the half-magic upgrade, mirroring how the
 *           engine's only other magic-upgrade path (the Mad Batter sprite)
 *           writes link_magic_consumption directly.
 * Magic Upgrade (1/4) has no engine receipt at all and stays refused. */
static const uint16_t kChainSword[4] = { 0x49, 0x01, 0x02, 0x03 };
static const uint16_t kChainGlove[2] = { 0x1b, 0x1c };
static const uint16_t kChainBow[2]   = { 0x0b, 0x3b };
static const uint16_t kChainShield[3] = { 0x04, 0x05, 0x06 };
static const uint16_t kChainArmor[2] = { 0x22, 0x23 };
static const uint16_t kChainMagic[1] = { 0x80 };   /* extra code: half magic (see Randomizer_ExtraItemBegin) */
static const struct ChainMap {
    const char *name;        /* model item name                    */
    int n_tiers;             /* tiers the engine can express       */
    const uint16_t *tiers;   /* concrete receipt per copy index    */
} kChains[] = {
    { "Progressive Sword", 4, kChainSword },
    { "Progressive Glove", 2, kChainGlove },
    { "Progressive Bow", 2, kChainBow },
    { "Progressive Shield", 3, kChainShield },
    { "Progressive Armor", 2, kChainArmor },
    { "Magic Upgrade (1/2)", 1, kChainMagic },
};

int Rando_ApplyChestCode(const RandoWorld *w, int item_id, int *chain_seen,
                         const char **why)
{
    const char *name;
    int code, i;
    if (why) *why = NULL;
    if (item_id < 0 || item_id >= w->n_items || !w->items[item_id].name) {
        if (why) *why = "unknown item";
        return -1;
    }
    name = w->items[item_id].name;
    /* keysanity first: a dungeon-named key/big key/map/compass whose class
     * this file shuffles ANYWHERE resolves to its dungeon-targeted code
     * (see the code-range comment above).  Doing it by name here, before
     * the kRandoItemCodes lookup, is what keeps the fork's own ROM codes
     * (which share the 0x4C..0x7F range) from ever being mistaken for one. */
    code = Rando_DungeonItemCode(name);
    if (code >= 0)
        return code;
    /* re-homed pool items (capacity upgrades, Silver Arrows, Triforce Piece,
     * Master Sword): resolved by name for the same reason the dungeon items
     * are - the fork's codes for them collide with the reserved 0x4C..0x7F
     * range, so they get an extra code 0x81..0x87 or, where the engine has a
     * vanilla receipt that already does the job, that receipt.  See
     * kRandoExtraCodes. */
    code = Rando_ExtraItemCode(name);
    if (code >= 0)
        return code;
    code = Rando_LookupEngineCode(name);
    if (code < 0) {
        if (why) *why = "unknown item";
        return -1;
    }
    if (code < 0x4C)
        return code;
    /* >= 0x4C: no direct receipt slot.  Remap progressive chains by pool
     * copy index (soundness argument in rando_fill.h).  A dump with more
     * copies of a chain than the engine has tiers clamps to the last tier
     * (this profile's pool matches exactly: 4/2/2/3/2/1). */
    for (i = 0; i < (int)(sizeof(kChains) / sizeof(kChains[0])); i++) {
        if (strcmp(kChains[i].name, name) == 0) {
            int k = chain_seen[item_id]++;
            if (k >= kChains[i].n_tiers)
                k = kChains[i].n_tiers - 1;
            return kChains[i].tiers[k];
        }
    }
    if (why) *why = "no engine receipt slot";
    return -1;
}

/* ------------------------------------------------------------------ */
/* JSON emission                                                       */
/* ------------------------------------------------------------------ */

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { fputc('\\', f); fputc(ch, f); }
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

int Rando_FillWriteJson(RandoWorld *world, const int *placements,
                        const uint8_t *eligible_flags,
                        unsigned int seed, const RandoFillStats *stats,
                        int can_beat, const char *json_path,
                        const RandoApplyCounts *apply)
{
    FILE *f;
    int i;
    if (!world || !placements || !json_path) return -1;
    f = fopen(json_path, "wb");
    if (!f) return -1;

    fprintf(f, "{\n \"version\": 1,\n \"seed\": %u,\n \"algorithm\": "
               "\"frontier fill_restrictive (per-placement validation) + fast_fill, phase A\",\n",
            seed);
    if (stats)
        fprintf(f, " \"stats\": {\"attempts\": %u, \"progression\": %u, "
                   "\"junk\": %u, \"eligible\": %u, \"pool_size\": %u, "
                   "\"sweeps\": %u, \"candidate_tests\": %u, "
                   "\"sphere_escapes\": %u, \"fill_ms\": %.2f},\n",
                stats->attempts, stats->prog_items, stats->junk_items,
                stats->eligible, stats->pool_size, stats->sweeps,
                stats->candidate_tests, stats->sphere_escapes, stats->fill_ms);
    fprintf(f, " \"can_beat_game\": %s,\n", can_beat ? "true" : "false");
    /* can_beat_game above is the MODEL claim (proved on chain-item counts);
     * the apply object reports what the ENGINE world actually received, so
     * the file never implies more than was applied to the chest table. */
    if (apply)
        fprintf(f, " \"apply\": {\"chest_locations\": %d, \"applied\": %d, "
                   "\"skipped\": %d, \"non_chest_locations\": %d},\n",
                apply->chest_locations, apply->applied, apply->skipped,
                apply->non_chest_locations);
    fprintf(f, " \"placements\": [\n");
        {
            int first = 1;
            for (i = 0; i < world->n_locations; i++) {
                int item = placements[i];
                const char *origin;
                if (item < 0) continue;
                if (!first) fprintf(f, ",\n");
                first = 0;
                /* eligible_flags: 0 = dump, 1 = fill, 2 = pinned (phase B2) */
                origin = "dump";
                if (eligible_flags)
                    origin = eligible_flags[i] == 2 ? "pin"
                           : eligible_flags[i] ? "fill" : "dump";
                fprintf(f, "  {\"id\": %d, \"location\": ", i);
                json_string(f, world->locations[i].name);
                fprintf(f, ", \"item\": ");
                json_string(f, world->items[item].name);
                fprintf(f, ", \"origin\": \"%s\"}", origin);
            }
        }
    fprintf(f, "\n ]\n}\n");
    fclose(f);
    return 0;
}

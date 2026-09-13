/* rando_state.c — CollectionState + reachability fixpoint sweep.
 *
 * The two baked-in tables below are profile data for the committed dumps
 * (players=1, logic=noglitches, mode=open, goal=ganon, door_shuffle=vanilla,
 * shuffle=vanilla, keyshuffle=none).  They cover exactly the parts of the
 * fork's rule layer that are NOT expressible from the world dump alone;
 * values are identical for both committed seeds (verified against the
 * reference fork's key_logic / shop stock).
 *
 *   kKeyDoors[]  — one entry per dungeon small-key door.  The fork attaches
 *                  eval_small_key_door_partial (ALL doors in this profile)
 *                  which is:
 *                    open = keys >= min(WorstCase, small_key_num)
 *                       OR key placed at the AllowSmall location
 *                       OR (big key placed at a Lock location
 *                           AND keys >= min(Lock_n, alternate_small_key))
 *                  lock_need below is min(Lock_n, alternate_small_key).
 *                  10 Misery Mire barrier doors use the CrystalAlternative
 *                  lambda instead: keys >= 2.
 *
 *   kUnlimited[] — can_buy_unlimited(item): shops whose stock of `item`
 *                  is unlimited (slot max == 0) in this profile.
 */
#include "rando_state.h"
#include "rando_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Baked vanilla-profile tables                                        */
/* ================================================================== */

#define SK_NAME(d) "Small Key (" d ")"
#define SK_ESCAPE "Small Key (Escape)"

static const char *key_name_for_dungeon(const char *d)
{
    if (strcmp(d, "Hyrule Castle") == 0) return SK_ESCAPE;
    if (strcmp(d, "Eastern Palace") == 0) return SK_NAME("Eastern Palace");
    if (strcmp(d, "Desert Palace") == 0) return SK_NAME("Desert Palace");
    if (strcmp(d, "Tower of Hera") == 0) return SK_NAME("Tower of Hera");
    if (strcmp(d, "Agahnims Tower") == 0) return SK_NAME("Agahnims Tower");
    if (strcmp(d, "Palace of Darkness") == 0) return SK_NAME("Palace of Darkness");
    if (strcmp(d, "Thieves Town") == 0) return SK_NAME("Thieves Town");
    if (strcmp(d, "Skull Woods") == 0) return SK_NAME("Skull Woods");
    if (strcmp(d, "Swamp Palace") == 0) return SK_NAME("Swamp Palace");
    if (strcmp(d, "Ice Palace") == 0) return SK_NAME("Ice Palace");
    if (strcmp(d, "Misery Mire") == 0) return SK_NAME("Misery Mire");
    if (strcmp(d, "Turtle Rock") == 0) return SK_NAME("Turtle Rock");
    if (strcmp(d, "Ganons Tower") == 0) return SK_NAME("Ganons Tower");
    return NULL;
}

typedef struct KeyDoorDef {
    const char *door;
    const char *dungeon;
    uint16_t worstcase;        /* WorstCase number (0 = absent)          */
    uint16_t small_key_num;    /* partial-variant cap                    */
    uint8_t crystal_alt;       /* CrystalAlternative count (0 = absent)  */
    uint8_t allowsmall;
    const char *allowsmall_loc;
    const char *lock_item;     /* NULL = no Lock branch                  */
    uint16_t lock_need;        /* min(new_rules Lock n, alternate_small) */
    const char *lock_locs[10]; /* NULL-terminated (GT rooms list 9 + NUL) */
} KeyDoorDef;

static const KeyDoorDef kKeyDoors[] = {
    /* Hyrule Castle / Escape */
    {"Sewers Secret Room Key Door S", "Hyrule Castle", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Hyrule Dungeon Map Room Key Door S", "Hyrule Castle", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Sewers Dark Cross Key Door N", "Hyrule Castle", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Sewers Water S", "Hyrule Castle", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    {"Hyrule Dungeon Armory Interior Key Door N", "Hyrule Castle", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    {"Sewers Key Rat NE", "Hyrule Castle", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    /* Eastern Palace */
    {"Eastern Dark Square Key Door WN", "Eastern Palace", 2, 2, 0, 1,
     "Eastern Palace - Big Key Chest", "Big Key (Eastern Palace)", 1,
     {"Eastern Palace - Big Key Chest", NULL}},
    {"Eastern Darkness Up Stairs", "Eastern Palace", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    /* Desert Palace */
    {"Desert East Wing Key Door EN", "Desert Palace", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    {"Desert Tiles 1 Up Stairs", "Desert Palace", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Desert Beamos Hall NE", "Desert Palace", 3, 3, 0, 0, NULL, NULL, 0, {NULL}},
    {"Desert Tiles 2 NE", "Desert Palace", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    /* Tower of Hera */
    {"Hera Lobby Key Stairs", "Tower of Hera", 1, 1, 0, 1,
     "Tower of Hera - Big Key Chest", NULL, 0, {NULL}},
    /* Agahnims Tower */
    {"Tower Room 03 Up Stairs", "Agahnims Tower", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},
    {"Tower Dark Maze ES", "Agahnims Tower", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Tower Dark Archers Up Stairs", "Agahnims Tower", 3, 3, 0, 0, NULL, NULL, 0, {NULL}},
    {"Tower Circle of Pots ES", "Agahnims Tower", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    /* Palace of Darkness */
    {"PoD Middle Cage N", "Palace of Darkness", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},
    {"PoD Dark Pegs WN", "Palace of Darkness", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"PoD Arena Main NW", "Palace of Darkness", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    {"PoD Pit Room S", "Palace of Darkness", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"PoD Basement Ledge Up Stairs", "Palace of Darkness", 6, 6, 0, 1,
     "Palace of Darkness - Big Key Chest", NULL, 0, {NULL}},
    {"PoD Falling Bridge WN", "Palace of Darkness", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"PoD Compass Room SE", "Palace of Darkness", 6, 6, 0, 1,
     "Palace of Darkness - Harmless Hellway", NULL, 0, {NULL}},
    /* Thieves Town */
    {"Thieves Hallway WS", "Thieves Town", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},
    {"Thieves Spike Switch Up Stairs", "Thieves Town", 3, 3, 0, 0, NULL, NULL, 0, {NULL}},
    {"Thieves Conveyor Bridge WS", "Thieves Town", 3, 3, 0, 1,
     "Thieves' Town - Big Chest", NULL, 0, {NULL}},
    /* Skull Woods */
    {"Skull Pinball NE", "Skull Woods", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Skull 1 Lobby WS", "Skull Woods", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Skull 3 Lobby NW", "Skull Woods", 4, 4, 0, 0, NULL, NULL, 0, {NULL}},
    {"Skull 2 West Lobby NW", "Skull Woods", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Skull Map Room SE", "Skull Woods", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Skull Pot Prison ES", "Skull Woods", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Skull Spike Corner ES", "Skull Woods", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    /* Swamp Palace */
    {"Swamp Entrance Down Stairs", "Swamp Palace", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},
    {"Swamp Pot Row WS", "Swamp Palace", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Swamp Trench 1 Key Ledge NW", "Swamp Palace", 3, 3, 0, 0, NULL, NULL, 0, {NULL}},
    {"Swamp Hub WN", "Swamp Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Swamp Hub North Ledge N", "Swamp Palace", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Swamp Waterway NW", "Swamp Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    /* Ice Palace */
    {"Ice Jelly Key Down Stairs", "Ice Palace", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Conveyor SW", "Ice Palace", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Spike Cross ES", "Ice Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Tall Hint SE", "Ice Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Spike Room WS", "Ice Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Lonely Freezor NE", "Ice Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Backwards Room Down Stairs", "Ice Palace", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"Ice Switch Room ES", "Ice Palace", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    /* Misery Mire: 9 key doors + 10 CrystalAlternative barriers */
    {"Mire Hub WS", "Misery Mire", 5, 5, 0, 0, NULL, "Big Key (Misery Mire)", 3,
     {"Misery Mire - Big Key Chest", "Misery Mire - Compass Chest", NULL}},
    {"Mire Spike Barrier NE", "Misery Mire", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Spikes NW", "Misery Mire", 4, 3, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Conveyor Crystal WS", "Misery Mire", 6, 6, 0, 0, NULL, "Big Key (Misery Mire)", 4,
     {"Misery Mire - Big Key Chest", "Misery Mire - Compass Chest", NULL}},
    {"Mire Hub Right EN", "Misery Mire", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Map Spot WN", "Misery Mire", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Fishbone SE", "Misery Mire", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Ledgehop SW", "Misery Mire", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Dark Shooters SE", "Misery Mire", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"Mire Hub Upper Blue Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Hub Lower Blue Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Hub Right Blue Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Hub Top Blue Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Hub Switch Blue Barrier N", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Hub Switch Blue Barrier S", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Map Spot Blue Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Map Spike Side Blue Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Crystal Dead End Left Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    {"Mire Crystal Dead End Right Barrier", "Misery Mire", 0, 0, 2, 0, NULL, NULL, 0, {NULL}},
    /* Turtle Rock */
    {"TR Hub NW", "Turtle Rock", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},
    {"TR Pokey 1 NW", "Turtle Rock", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},
    /* far-side names of two paired doors (Doors.py PairedDoor / DoorShuffle
     * pairs): the OW-glitch profiles reach them from the other room, so the
     * dump names that side.  Same physical door, same key logic. */
    {"TR Pokey 1 SW", "Turtle Rock", 1, 1, 0, 0, NULL, NULL, 0, {NULL}},      /* == TR Hub NW */
    {"TR Chain Chomps SW", "Turtle Rock", 2, 2, 0, 0, NULL, NULL, 0, {NULL}},  /* == TR Pokey 1 NW */
    {"TR Chain Chomps Down Stairs", "Turtle Rock", 3, 3, 0, 0, NULL, NULL, 0, {NULL}},
    {"TR Pokey 2 ES", "Turtle Rock", 6, 6, 0, 1,
     "Turtle Rock - Big Key Chest", "Big Key (Turtle Rock)", 4,
     {"Turtle Rock - Big Key Chest", NULL}},
    {"TR Crystaroller Down Stairs", "Turtle Rock", 5, 5, 0, 0, NULL, NULL, 0, {NULL}},
    {"TR Dash Bridge WS", "Turtle Rock", 6, 6, 0, 0, NULL, NULL, 0, {NULL}},
    /* Ganons Tower */
    {"GT Hope Room WN", "Ganons Tower", 8, 8, 0, 0, NULL, NULL, 0, {NULL}},
    {"GT Tile Room EN", "Ganons Tower", 7, 7, 0, 0, NULL, "Big Key (Ganons Tower)", 5,
     {"Ganons Tower - Big Key Room - Right", "Ganons Tower - Big Chest",
      "Ganons Tower - Big Key Chest", "Ganons Tower - Compass Room - Top Right",
      "Ganons Tower - Bob's Chest", "Ganons Tower - Compass Room - Bottom Right",
      "Ganons Tower - Compass Room - Bottom Left", "Ganons Tower - Big Key Room - Left",
      "Ganons Tower - Compass Room - Top Left", NULL}},
    {"GT Torch EN", "Ganons Tower", 8, 8, 0, 0, NULL, NULL, 0, {NULL}},
    {"GT Hookshot ES", "Ganons Tower", 8, 8, 0, 1,
     "Ganons Tower - Map Chest", "Big Key (Ganons Tower)", 5,
     {"Ganons Tower - Map Chest", NULL}},
    {"GT Double Switch EN", "Ganons Tower", 7, 6, 0, 0, NULL, "Big Key (Ganons Tower)", 4,
     {"Ganons Tower - Randomizer Room - Top Left", "Ganons Tower - Randomizer Room - Bottom Right",
      "Ganons Tower - Randomizer Room - Bottom Left", "Ganons Tower - Firesnake Room",
      "Ganons Tower - Randomizer Room - Top Right", NULL}},
    {"GT Conveyor Star Pits EN", "Ganons Tower", 7, 6, 0, 0, NULL, "Big Key (Ganons Tower)", 5,
     {"Ganons Tower - Big Key Room - Right", "Ganons Tower - Big Chest",
      "Ganons Tower - Big Key Chest", "Ganons Tower - Bob's Chest",
      "Ganons Tower - Big Key Room - Left", NULL}},
    {"GT Firesnake Room SW", "Ganons Tower", 8, 8, 0, 0, NULL, "Big Key (Ganons Tower)", 5,
     {"Ganons Tower - Randomizer Room - Top Left", "Ganons Tower - Randomizer Room - Bottom Right",
      "Ganons Tower - Big Key Room - Right", "Ganons Tower - Big Chest",
      "Ganons Tower - Big Key Chest", "Ganons Tower - Bob's Chest",
      "Ganons Tower - Randomizer Room - Bottom Left", "Ganons Tower - Big Key Room - Left",
      "Ganons Tower - Randomizer Room - Top Right", NULL}},
    {"GT Mini Helmasaur Room WN", "Ganons Tower", 7, 6, 0, 0, NULL, NULL, 0, {NULL}},
    {"GT Crystal Circles SW", "Ganons Tower", 8, 8, 0, 0, NULL, NULL, 0, {NULL}},
};

#define N_KEYDOORS (sizeof(kKeyDoors) / sizeof(kKeyDoors[0]))

int Rando_KeyDoorIndex(const char *door)
{
    size_t i;
    for (i = 0; i < N_KEYDOORS; i++)
        if (strcmp(kKeyDoors[i].door, door) == 0) return (int)i;
    return -1;
}

/* shops with unlimited stock per item (shopsanity off) */
typedef struct UnlimitedDef {
    const char *item;
    const char *regions[9];
} UnlimitedDef;

static const UnlimitedDef kUnlimited[] = {
    {"Bombs (10)", {"Dark Death Mountain Shop", "Dark Lake Hylia Shop",
                    "Dark Lumberjack Shop", "Village of Outcasts Shop",
                    "Dark Potion Shop", "Paradox Shop", "Kakariko Shop",
                    "Lake Hylia Shop", NULL}},
    {"Green Potion", {"Potion Shop", NULL}},
    {"Blue Potion", {"Potion Shop", NULL}},
};

#define N_UNLIMITED (sizeof(kUnlimited) / sizeof(kUnlimited[0]))

/* progressive-item profile limits (difficulty "normal") — from the fork's
 * difficulty_requirements: sword 4, bottle 4 (dump-gap agent finding) */
enum { PROG_SWORD_LIMIT = 4, PROG_BOTTLE_LIMIT = 4 };

/* ================================================================== */
/* Table binding (called by the loader after the model is built)       */
/* ================================================================== */

static int bind_err(char *err, size_t errsz, const char *msg, const char *arg)
{
    if (err && errsz) snprintf(err, errsz, msg, arg ? arg : "");
    return -1;
}

int Rando_BindProfileTables(RandoWorld *w, char *err, size_t errsz)
{
    size_t i;
    size_t n_key = N_KEYDOORS;
    size_t n_unl = N_UNLIMITED;

    /* account for the compiled program (nodes + shared id lists) */
    w->bytes_allocated += (size_t)w->program.n_nodes * sizeof(RandoRuleNode)
                        + (size_t)w->program.n_idlists * sizeof(uint16_t);

    w->keydoors = (RandoKeyDoor *)calloc(n_key, sizeof(RandoKeyDoor));
    if (!w->keydoors) return bind_err(err, errsz, "oom binding keydoors", NULL);
    w->n_keydoors = (int)n_key;
    w->bytes_allocated += n_key * sizeof(RandoKeyDoor);

    for (i = 0; i < n_key; i++) {
        const KeyDoorDef *d = &kKeyDoors[i];
        RandoKeyDoor *kd = &w->keydoors[i];
        const char *keyname;
        size_t j;
        kd->door = d->door;
        kd->dungeon = d->dungeon;
        keyname = key_name_for_dungeon(d->dungeon);
        if (!keyname) return bind_err(err, errsz, "no small key name for dungeon %s", d->dungeon);
        kd->key_item = (int16_t)Rando_ItemId(w, keyname);
        if (kd->key_item < 0) return bind_err(err, errsz, "unknown key item %s", keyname);
        kd->worstcase = d->worstcase;
        kd->small_key_num = d->small_key_num;
        kd->crystal_alt = d->crystal_alt;
        kd->allowsmall_loc = (int16_t)(d->allowsmall ? Rando_LocationId(w, d->allowsmall_loc) : -1);
        if (d->allowsmall && kd->allowsmall_loc < 0)
            return bind_err(err, errsz, "unknown AllowSmall location %s", d->allowsmall_loc);
        kd->lock_item = -1;
        if (d->lock_item) {
            size_t n = 0;
            kd->lock_item = (int16_t)Rando_ItemId(w, d->lock_item);
            if (kd->lock_item < 0) return bind_err(err, errsz, "unknown lock item %s", d->lock_item);
            kd->lock_need = d->lock_need;
            while (n < 9 && d->lock_locs[n]) n++;
            kd->lock_locs = (int16_t *)calloc(n ? n : 1, sizeof(int16_t));
            if (!kd->lock_locs) return bind_err(err, errsz, "oom", NULL);
            w->bytes_allocated += n * sizeof(int16_t);
            for (j = 0; j < n; j++) {
                kd->lock_locs[j] = (int16_t)Rando_LocationId(w, d->lock_locs[j]);
                if (kd->lock_locs[j] < 0)
                    return bind_err(err, errsz, "unknown lock location %s", d->lock_locs[j]);
            }
            kd->n_lock_locs = (uint16_t)n;
        }
    }

    /* resolve RR_SMALL_KEY_DOOR marker nodes: a = kKeyDoors index, bound by
     * the compiler via Rando_KeyDoorIndex; verify consistency here */
    for (i = 0; i < (size_t)w->program.n_nodes; i++) {
        const RandoRuleNode *node = &w->program.nodes[i];
        if (node->op == RR_SMALL_KEY_DOOR &&
            (node->a < 0 || node->a >= (int)n_key))
            return bind_err(err, errsz, "unbound small_key_door node in program", NULL);
    }

    w->unlimited = (RandoUnlimitedEntry *)calloc(n_unl, sizeof(RandoUnlimitedEntry));
    if (!w->unlimited) return bind_err(err, errsz, "oom binding shops", NULL);
    w->n_unlimited = (int)n_unl;
    w->bytes_allocated += n_unl * sizeof(RandoUnlimitedEntry);
    for (i = 0; i < n_unl; i++) {
        const UnlimitedDef *d = &kUnlimited[i];
        RandoUnlimitedEntry *e = &w->unlimited[i];
        size_t j, n = 0;
        e->item = Rando_ItemId(w, d->item);
        if (e->item < 0) return bind_err(err, errsz, "unknown shop item %s", d->item);
        while (n < 9 && d->regions[n]) n++;
        e->regions = (int16_t *)calloc(n ? n : 1, sizeof(int16_t));
        if (!e->regions) return bind_err(err, errsz, "oom", NULL);
        w->bytes_allocated += n * sizeof(int16_t);
        for (j = 0; j < n; j++) {
            e->regions[j] = (int16_t)Rando_RegionId(w, d->regions[j]);
            if (e->regions[j] < 0)
                return bind_err(err, errsz, "unknown shop region %s", d->regions[j]);
        }
        e->n_regions = (uint16_t)n;
    }

    /* potion ids for extend_magic */
    w->unlimited_magic_potions[0] = (int16_t)Rando_ItemId(w, "Green Potion");
    w->unlimited_magic_potions[1] = (int16_t)Rando_ItemId(w, "Blue Potion");
    return 0;
}

/* ================================================================== */
/* CollectionState                                                     */
/* ================================================================== */

RandoState *Rando_StateNew(const RandoWorld *world)
{
    RandoState *st = (RandoState *)calloc(1, sizeof(RandoState));
    if (!st) return NULL;
    st->world = world;
    st->count = (uint16_t *)calloc((size_t)world->n_items, sizeof(uint16_t));
    st->reachable = (uint8_t *)calloc((size_t)world->n_regions, 1);
    st->barrier = (uint8_t *)calloc((size_t)world->n_regions, 1);
    st->collected = (uint8_t *)calloc((size_t)world->n_locations, 1);
    st->_queue_mem = malloc((size_t)world->n_regions * sizeof(int));
    if (!st->count || !st->reachable || !st->barrier || !st->collected || !st->_queue_mem) {
        Rando_StateFree(st);
        return NULL;
    }
    Rando_StateReset(st);
    return st;
}

void Rando_StateFree(RandoState *st)
{
    if (!st) return;
    free(st->count);
    free(st->reachable);
    free(st->barrier);
    free(st->collected);
    free(st->_queue_mem);
    free(st);
}

void Rando_StateReset(RandoState *st)
{
    const RandoWorld *w = st->world;
    int i;
    memset(st->count, 0, (size_t)w->n_items * sizeof(uint16_t));
    memset(st->reachable, 0, (size_t)w->n_regions);
    memset(st->barrier, 0, (size_t)w->n_regions);
    memset(st->collected, 0, (size_t)w->n_locations);
    st->sweep_rounds = 0;
    st->eval_depth_exceeded = 0;
    for (i = 0; i < w->n_items; i++)               /* precollected (none in  */
        if (w->items[i].precollected)              /* the committed profile) */
            Rando_StateCollectItem(st, i);
}

static void make_ctx(const RandoState *st, RandoEvalCtx *ctx)
{
    ctx->world = st->world;
    ctx->count = st->count;
    ctx->reachable = st->reachable;
    ctx->barrier = st->barrier;
    ctx->max_depth = RANDO_EVAL_MAX_DEPTH;
    ctx->depth_exceeded = 0;
    ctx->_depth = 0;
}

/* fork CollectionState.collect(item, event=true): progressive chains are
 * folded; bottles capped at the profile limit; everything else bumps its
 * own counter. */
void Rando_StateCollectItem(RandoState *st, int item_id)
{
    const RandoWorld *w = st->world;
    const char *name;
    if (item_id < 0 || item_id >= w->n_items) return;
    name = w->items[item_id].name;
    if (!name) return;

    if (strncmp(name, "Progressive Sword", 17) == 0) {
        static const char *const tiers[PROG_SWORD_LIMIT] = {
            "Fighter Sword", "Master Sword", "Tempered Sword", "Golden Sword" };
        int t;
        for (t = 0; t < PROG_SWORD_LIMIT; t++) {
            int id = Rando_ItemId(w, tiers[t]);
            if (id < 0) return;
            if (st->count[id] == 0) { st->count[id]++; return; }
        }
        return; /* all tiers held: the fork's explicit 'pass' */
    }
    if (strcmp(name, "Progressive Glove") == 0) {
        int pg = Rando_ItemId(w, "Power Glove");
        int tm = Rando_ItemId(w, "Titans Mitts");
        if (pg >= 0 && tm >= 0) {
            if (st->count[pg] > 0) st->count[tm]++;
            else st->count[pg]++;
        }
        return;
    }
    if (strcmp(name, "Progressive Shield") == 0) {
        int blue = Rando_ItemId(w, "Blue Shield");
        int red = Rando_ItemId(w, "Red Shield");
        int mirror = Rando_ItemId(w, "Mirror Shield");
        int lvl = Rando_ItemId(w, "Shield Level");
        if (blue < 0 || red < 0 || mirror < 0 || lvl < 0) return;
        if (st->count[blue] && st->count[red]) { st->count[mirror]++; st->count[lvl]++; }
        else if (st->count[blue]) { st->count[red]++; st->count[lvl]++; }
        else { st->count[blue]++; st->count[lvl]++; }
        return;
    }
    if (strcmp(name, "Progressive Bow") == 0) {
        int bow = Rando_ItemId(w, "Bow");
        int silver = Rando_ItemId(w, "Silver Arrows");
        if (bow >= 0 && silver >= 0) {
            if (st->count[bow] > 0) st->count[silver]++;
            else st->count[bow]++;
        }
        return;
    }
    if (strcmp(name, "Progressive Armor") == 0) {
        int blue = Rando_ItemId(w, "Blue Mail");
        int red = Rando_ItemId(w, "Red Mail");
        if (blue >= 0 && red >= 0) {
            if (st->count[blue] > 0) st->count[red]++;
            else st->count[blue]++;
        }
        return;
    }
    if (strncmp(name, "Bottle", 6) == 0) {
        int held = 0, i;
        for (i = 0; i < w->n_bottles; i++)
            if (st->count[w->bottles[i]] > 0) held++;
        if (held < PROG_BOTTLE_LIMIT) st->count[item_id]++;
        return;
    }
    st->count[item_id]++;
}

/* one collection pass over all locations; 1 if anything new collected */
int Rando_StateCollectPass(RandoState *st, int skip_item)
{
    const RandoWorld *w = st->world;
    int i, any = 0;
    for (i = 0; i < w->n_locations; i++) {
        const RandoLocation *loc = &w->locations[i];
        RandoEvalCtx ctx;
        if (st->collected[i]) continue;
        if (loc->placed_item < 0 || loc->placed_item == skip_item) continue;
        if (!st->reachable[loc->region]) continue;
        make_ctx(st, &ctx);
        if (loc->rule >= 0 && !Rando_RuleEval(&w->program, loc->rule, &ctx)) {
            if (ctx.depth_exceeded) st->eval_depth_exceeded = 1;
            continue;
        }
        st->collected[i] = 1;
        Rando_StateCollectItem(st, loc->placed_item);
        any = 1;
    }
    return any;
}

/* full fixpoint: BFS over edges + collection passes until stable */
int Rando_SweepReachability(RandoState *st, int collect, int skip_item)
{
    const RandoWorld *w = st->world;
    int *queue = (int *)st->_queue_mem;
    int qhead, qtail;
    int i, changed = 1;

    memset(st->reachable, 0, (size_t)w->n_regions);
    memset(st->barrier, 0, (size_t)w->n_regions);
    qhead = qtail = 0;
    for (i = 0; i < w->n_start_regions; i++) {
        int r = w->start_regions[i];
        if (r >= 0 && !st->reachable[r]) {
            st->reachable[r] = 1;
            queue[qtail++] = r;
        }
    }
    st->sweep_rounds = 0;
    while (changed) {
        changed = 0;
        st->sweep_rounds++;
        while (qhead < qtail) {
            int cur = queue[qhead++];
            int ei;
            for (ei = w->edge_offsets[cur]; ei < w->edge_offsets[cur + 1]; ei++) {
                const RandoEdge *edge = &w->edges[w->edge_order[ei]];
                RandoEvalCtx ctx;
                int to = edge->to;
                int ok;
                if (st->reachable[to]) continue;
                make_ctx(st, &ctx);
                ok = edge->rule < 0 ? 1
                    : (Rando_RuleEval(&w->program, edge->rule, &ctx) &&
                       !ctx.depth_exceeded);
                if (ctx.depth_exceeded) st->eval_depth_exceeded = 1;
                if (!ok) continue;
                st->reachable[to] = 1;
                st->barrier[to] |= w->regions[to].is_dungeon ? 3 : RR_BARRIER_ORANGE;
                queue[qtail++] = to;
                changed = 1;
            }
        }
        if (collect && Rando_StateCollectPass(st, skip_item)) {
            changed = 1;
            qhead = qtail = 0;
            for (i = 0; i < w->n_regions; i++)
                if (st->reachable[i]) queue[qtail++] = i;
        }
    }
    {
        int count = 0;
        for (i = 0; i < w->n_regions; i++) count += st->reachable[i];
        return count;
    }
}

int Rando_SweepReachabilityNoCollect(RandoState *st)
{
    return Rando_SweepReachability(st, 0, 0);
}

int Rando_StateCanReachRegion(const RandoState *st, int region_id)
{
    if (region_id < 0 || region_id >= st->world->n_regions) return 0;
    return st->reachable[region_id];
}

int Rando_StateHas(const RandoState *st, int item_id, int count)
{
    if (item_id < 0 || item_id >= st->world->n_items) return 0;
    return st->count[item_id] >= count;
}

int Rando_StateLocationReachable(const RandoState *st, int loc_id)
{
    RandoEvalCtx ctx;
    int v;
    if (loc_id < 0 || loc_id >= st->world->n_locations) return 0;
    if (!st->reachable[st->world->locations[loc_id].region]) return 0;
    if (st->world->locations[loc_id].rule < 0) return 1;
    make_ctx(st, &ctx);
    v = Rando_RuleEval(&st->world->program, st->world->locations[loc_id].rule, &ctx);
    return v && !ctx.depth_exceeded;
}

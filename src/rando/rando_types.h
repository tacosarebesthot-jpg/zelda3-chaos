/* rando_types.h — core data model for the native randomizer logic engine.
 *
 * Phase-1 scaffold: consumes the phase-0 JSON dumps produced by
 * randomizer_ref/dump_logic.py (see randomizer_ref/README.md and
 * randomizer_ref/SCHEMA_NOTES.md).  Everything here is engine-independent:
 * no SDL, no g_ram, no zelda3 headers — pure C11 + libc.
 *
 * Naming conventions:
 *   - region ids / item ids / location ids are dense ints (array indices).
 *   - rule references are entry-point indices into a compiled RandoProgram
 *     (see rando_rules.h).
 *   - "item" means any collectible name in the ALTTPR CollectionState sense,
 *     including event items ("Beat Agahnim 2", "Crystal 3", ...).
 */
#ifndef RANDO_TYPES_H
#define RANDO_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "rando_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Region                                                              */
/* ------------------------------------------------------------------ */

enum {
    RANDO_REGION_MENU = 0,
    RANDO_REGION_LIGHTWORLD,
    RANDO_REGION_DARKWORLD,
    RANDO_REGION_CAVE,
    RANDO_REGION_DUNGEON,
    RANDO_REGION_OTHER,
};

typedef struct RandoRegion {
    const char *name;
    const char *dungeon;      /* dungeon object name or NULL */
    int id;                   /* == index into RandoWorld::regions      */
    uint8_t type;             /* RANDO_REGION_*                        */
    uint8_t light_world;
    uint8_t dark_world;
    uint8_t indoors;
    uint8_t is_dungeon;       /* dungeon != NULL                       */
} RandoRegion;

/* ------------------------------------------------------------------ */
/* Edge (directed connection between regions)                          */
/* ------------------------------------------------------------------ */

typedef struct RandoEdge {
    int from;                 /* region id                             */
    int to;                   /* region id                             */
    int rule;                 /* entry index in program, -1 = TRUE     */
    const char *entrance;     /* entrance/exit name                    */
    const char *door;         /* door name or NULL                     */
    const char *door_type;    /* Logical/Interior/... or NULL          */
    uint8_t spot_type;        /* index into kRandoSpotTypes            */
} RandoEdge;

/* spot_type strings as emitted by the dumper */
extern const char *const kRandoSpotTypes[10];
enum {
    RANDO_SPOT_ENTRANCE = 0, RANDO_SPOT_LEDGE, RANDO_SPOT_MIRROR,
    RANDO_SPOT_FLUTE, RANDO_SPOT_OWEDGE, RANDO_SPOT_OWTERRAIN,
    RANDO_SPOT_OPENTERRAIN, RANDO_SPOT_PORTAL, RANDO_SPOT_WHIRLPOOL,
    RANDO_SPOT_OTHER,
};
int Rando_SpotTypeFromName(const char *name);   /* RANDO_SPOT_OTHER if unknown */

/* ------------------------------------------------------------------ */
/* Location                                                            */
/* ------------------------------------------------------------------ */

enum {
    RANDO_LOC_NORMAL = 0, RANDO_LOC_PRIZE, RANDO_LOC_LOGICAL,
    RANDO_LOC_SHOP, RANDO_LOC_POT, RANDO_LOC_DROP, RANDO_LOC_BONK,
    RANDO_LOC_OTHER,
};

typedef struct RandoLocation {
    const char *name;
    int id;                   /* == index into RandoWorld::locations   */
    int region;               /* region id                             */
    int rule;                 /* entry index in program, -1 = TRUE     */
    int placed_item;          /* item id or -1                         */
    int forced_item;          /* item id or -1                         */
    uint8_t placed_progression;
    uint8_t type;             /* RANDO_LOC_*                           */
    uint8_t event;
    uint8_t real;
    uint8_t locked;
} RandoLocation;

/* ------------------------------------------------------------------ */
/* Item definition (pool entry; event items get zero-count defs)       */
/* ------------------------------------------------------------------ */

typedef struct RandoItemDef {
    const char *name;
    const char *itype;        /* SmallKey/BigKey/Map/... or NULL       */
    int pool_count;           /* copies in the seed pool               */
    uint8_t progression;      /* fork "advancement" flag               */
    uint8_t precollected;     /* granted at start                      */
} RandoItemDef;

/* ------------------------------------------------------------------ */
/* Key-door table (resolved from the built-in vanilla-profile table).  */
/* See rando_state.c: the fork evaluates small-key doors with the      */
/* "partial" algorithm: WorstCase capped by small_key_num, Lock counts */
/* capped by alternate_small_key, AllowSmall = key placed at small_loc */
/* ------------------------------------------------------------------ */

typedef struct RandoKeyDoor {
    const char *door;
    const char *dungeon;
    int key_item;             /* item id of "Small Key (<dungeon>)"    */
    int16_t allowsmall_loc;   /* location id or -1                     */
    int16_t lock_item;        /* big-key item id or -1                 */
    int16_t *lock_locs;       /* location ids (or NULL)                */
    uint16_t n_lock_locs;
    uint16_t worstcase;       /* WorstCase number (0 = absent)         */
    uint16_t small_key_num;   /* cap for WorstCase (partial variant)   */
    uint16_t lock_need;       /* effective Lock key count (capped)     */
    uint16_t crystal_alt;     /* CrystalAlternative count (0 = absent) */
} RandoKeyDoor;

/* can_buy_unlimited(item): true if any shop region selling `item` with
 * unlimited stock is reachable.  Baked for the committed profile.      */
typedef struct RandoUnlimitedEntry {
    int item;                 /* item id                               */
    int16_t *regions;         /* shop region ids                       */
    uint16_t n_regions;
} RandoUnlimitedEntry;

/* ------------------------------------------------------------------ */
/* World                                                                */
/* ------------------------------------------------------------------ */

typedef struct RandoWorld {
    RandoRegion *regions;     int n_regions;
    RandoEdge *edges;         int n_edges;
    RandoLocation *locations; int n_locations;
    RandoItemDef *items;      int n_items;

    /* outgoing edges grouped by region (CSR): edge ids at
     * edge_order[edge_offsets[r] .. edge_offsets[r+1]) are from region r */
    int *edge_offsets;        /* n_regions + 1                         */
    int *edge_order;          /* n_edges                               */

    RandoProgram program;     /* compiled rule trees                   */

    RandoNameTable *region_by_name;   /* name -> region id             */
    RandoNameTable *loc_by_name;      /* name -> location id           */
    RandoNameTable *item_by_name;     /* name -> item id               */

    /* resolved profile tables */
    RandoKeyDoor *keydoors;           int n_keydoors;
    RandoUnlimitedEntry *unlimited;   int n_unlimited;

    /* semantic item groups used by the interpreter */
    int16_t bottles[16];      int n_bottles;     /* names start "Bottle"  */
    int16_t crystals[7];      int n_crystals;    /* Crystal 1..7          */
    int16_t pendants[3];      int n_pendants;
    int16_t heart_boss;       /* "Boss Heart Container"                */
    int16_t heart_sanctuary;  /* "Sanctuary Heart Container"           */
    int16_t heart_piece;      /* "Piece of Heart"                      */
    int16_t magic_half, magic_quarter; /* magic upgrades               */
    int16_t unlimited_magic_potions[2];/* "Green Potion","Blue Potion"  */

    /* start / victory */
    int16_t start_regions[4]; int n_start_regions; /* Menu, Links House */
    int goal_location;        /* "Ganon" location id or -1             */
    int victory_item;         /* "Triforce" item id or -1              */

    /* meta.json cross-check data */
    struct {
        int seed;
        int counts_regions, counts_edges, counts_locations;
        int counts_items_in_pool, counts_item_names;
    } meta;

    size_t bytes_allocated;   /* rough heap footprint of the model     */
} RandoWorld;

void Rando_InitWorld(RandoWorld *w);
void Rando_FreeWorld(RandoWorld *w);

/* name -> id lookups; return -1 when absent */
int Rando_RegionId(const RandoWorld *w, const char *name);
int Rando_LocationId(const RandoWorld *w, const char *name);
int Rando_ItemId(const RandoWorld *w, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* RANDO_TYPES_H */

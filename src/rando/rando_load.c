/* rando_load.c — loads the six phase-0 JSON dumps into a RandoWorld.
 *
 * Load order matters: regions first (region name table), then the item
 * registry (pool + placed/forced names), then the rule compiler (which may
 * intern further event item names), then edges/locations resolve their rule
 * ids, then the profile tables (key doors, shops) bind, and finally the
 * semantic item groups / start regions / victory markers are located.
 */
#include "rando_load.h"
#include "rando_json.h"
#include "rando_rules.h"
#include "rando_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER)
#define _strdup strdup
#endif

/* ------------------------------------------------------------------ */
/* small utils                                                         */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *read_text_file(const char *path, size_t *out_len, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long sz;
    if (!f) {
        if (err && errsz) snprintf(err, errsz, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); if (err && errsz) snprintf(err, errsz, "ftell failed on %s", path); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (err && errsz) snprintf(err, errsz, "oom reading %s", path); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f);
        if (err && errsz) snprintf(err, errsz, "short read on %s", path);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

const char *const kRandoSpotTypes[10] = {
    "Entrance", "Ledge", "Mirror", "Flute", "OWEdge",
    "OWTerrain", "OpenTerrain", "Portal", "Whirlpool", "?",
};

int Rando_SpotTypeFromName(const char *name)
{
    int i;
    if (!name) return RANDO_SPOT_OTHER;
    for (i = 0; i < 9; i++)
        if (strcmp(kRandoSpotTypes[i], name) == 0) return i;
    return RANDO_SPOT_OTHER;
}

static int region_type_from_name(const char *s)
{
    if (!s) return RANDO_REGION_OTHER;
    if (strcmp(s, "Menu") == 0) return RANDO_REGION_MENU;
    if (strcmp(s, "LightWorld") == 0) return RANDO_REGION_LIGHTWORLD;
    if (strcmp(s, "DarkWorld") == 0) return RANDO_REGION_DARKWORLD;
    if (strcmp(s, "Cave") == 0) return RANDO_REGION_CAVE;
    if (strcmp(s, "Dungeon") == 0) return RANDO_REGION_DUNGEON;
    return RANDO_REGION_OTHER;
}

static int loc_type_from_name(const char *s)
{
    if (!s) return RANDO_LOC_OTHER;
    if (strcmp(s, "Normal") == 0) return RANDO_LOC_NORMAL;
    if (strcmp(s, "Prize") == 0) return RANDO_LOC_PRIZE;
    if (strcmp(s, "Logical") == 0) return RANDO_LOC_LOGICAL;
    if (strcmp(s, "Shop") == 0) return RANDO_LOC_SHOP;
    if (strcmp(s, "Pot") == 0) return RANDO_LOC_POT;
    if (strcmp(s, "Drop") == 0) return RANDO_LOC_DROP;
    if (strcmp(s, "Bonk") == 0) return RANDO_LOC_BONK;
    return RANDO_LOC_OTHER;
}

static int load_err(char *err, size_t errsz, const char *msg, const char *arg)
{
    if (err && errsz) snprintf(err, errsz, msg, arg ? arg : "");
    return -1;
}

/* ------------------------------------------------------------------ */
/* item registry (build-time)                                          */
/* ------------------------------------------------------------------ */

typedef struct ItemAcc {
    const char *name;         /* points into the JSON DOM during load  */
    const char *itype;
    int pool_count;
    int progression;
    int precollected;
    int known_from_pool;
} ItemAcc;

typedef struct LoadCtx {
    RandoNameTable *items;    /* name -> index into acc                */
    ItemAcc *acc;
    int n, cap;
} LoadCtx;

static int acc_intern(LoadCtx *lc, const char *name)
{
    int id = Rando_NamesGet(lc->items, name);
    if (id >= 0) return id;
    if (lc->n == lc->cap) {
        int nc = lc->cap ? lc->cap * 2 : 256;
        ItemAcc *na = (ItemAcc *)realloc(lc->acc, (size_t)nc * sizeof(ItemAcc));
        if (!na) return -1;
        lc->acc = na; lc->cap = nc;
    }
    memset(&lc->acc[lc->n], 0, sizeof(ItemAcc));
    lc->acc[lc->n].name = name;
    Rando_NamesPut(lc->items, name, lc->n);
    return lc->n++;
}

/* Rule-compiler trampolines.  The loader is single-world / single-threaded
 * (test harness + later in-engine use on one thread); the contexts are set
 * at the top of Rando_LoadWorld. */
static LoadCtx *g_lc;
static RandoWorld *g_w;

static int rc_intern_bind(void *ud, const char *name)
{
    (void)ud;
    return acc_intern(g_lc, name);
}

static int rc_lookup_region_bind(void *ud, const char *name)
{
    (void)ud;
    return Rando_RegionId(g_w, name);
}

static int rc_lookup_door_bind(void *ud, const char *name)
{
    (void)ud;
    return Rando_KeyDoorIndex(name);
}

/* ------------------------------------------------------------------ */
/* world lifetime                                                      */
/* ------------------------------------------------------------------ */

void Rando_InitWorld(RandoWorld *w)
{
    memset(w, 0, sizeof(*w));
    Rando_ProgramInit(&w->program);
    w->goal_location = -1;
    w->victory_item = -1;
    w->heart_boss = w->heart_sanctuary = w->heart_piece = -1;
    w->magic_half = w->magic_quarter = -1;
}

void Rando_FreeWorld(RandoWorld *w)
{
    int i;
    if (!w) return;
    for (i = 0; i < w->n_regions; i++) {
        free((void *)w->regions[i].name);
        free((void *)w->regions[i].dungeon);
    }
    free(w->regions);
    for (i = 0; i < w->n_edges; i++) {
        free((void *)w->edges[i].entrance);
        free((void *)w->edges[i].door);
        free((void *)w->edges[i].door_type);
    }
    free(w->edges);
    for (i = 0; i < w->n_locations; i++) free((void *)w->locations[i].name);
    free(w->locations);
    for (i = 0; i < w->n_items; i++) {
        free((void *)w->items[i].name);
        free((void *)w->items[i].itype);
    }
    free(w->items);
    free(w->edge_offsets);
    free(w->edge_order);
    Rando_ProgramFree(&w->program);
    Rando_NamesFree(w->region_by_name);
    Rando_NamesFree(w->loc_by_name);
    Rando_NamesFree(w->item_by_name);
    for (i = 0; i < w->n_keydoors; i++) free(w->keydoors[i].lock_locs);
    free(w->keydoors);
    for (i = 0; i < w->n_unlimited; i++) free(w->unlimited[i].regions);
    free(w->unlimited);
    memset(w, 0, sizeof(*w));
}

/* world lookups (see rando_types.h) */
int Rando_RegionId(const RandoWorld *w, const char *name)
{
    return Rando_NamesGet(w->region_by_name, name);
}
int Rando_LocationId(const RandoWorld *w, const char *name)
{
    return Rando_NamesGet(w->loc_by_name, name);
}
int Rando_ItemId(const RandoWorld *w, const char *name)
{
    return Rando_NamesGet(w->item_by_name, name);
}

/* ------------------------------------------------------------------ */
/* loader                                                              */
/* ------------------------------------------------------------------ */

static int load_regions(RandoWorld *w, const JsonValue *arr, char *err, size_t errsz)
{
    const JsonValue *r;
    int i = 0;
    w->n_regions = Json_Size(arr);
    w->regions = (RandoRegion *)calloc((size_t)w->n_regions, sizeof(RandoRegion));
    if (!w->regions) return load_err(err, errsz, "oom regions", NULL);
    w->bytes_allocated += (size_t)w->n_regions * sizeof(RandoRegion);
    w->region_by_name = Rando_NamesNew();
    if (!w->region_by_name) return load_err(err, errsz, "oom", NULL);
    for (r = arr->child; r; r = r->next, i++) {
        RandoRegion *rg = &w->regions[i];
        const char *ty;
        if (Json_AsInt(Json_Get(r, "id")) != i)
            return load_err(err, errsz, "region id mismatch (ids must be 0..n-1)", NULL);
        rg->id = i;
        rg->name = xstrdup(Json_AsString(Json_Get(r, "name")));
        ty = Json_AsString(Json_Get(r, "type"));
        rg->type = (uint8_t)region_type_from_name(ty);
        if (!rg->name) return load_err(err, errsz, "region missing name", NULL);
        if (Json_AsString(Json_Get(r, "dungeon")))
            rg->dungeon = xstrdup(Json_AsString(Json_Get(r, "dungeon")));
        rg->light_world = (uint8_t)Json_AsBool(Json_Get(r, "light_world"));
        rg->dark_world = (uint8_t)Json_AsBool(Json_Get(r, "dark_world"));
        rg->indoors = (uint8_t)Json_AsBool(Json_Get(r, "indoors"));
        rg->is_dungeon = (uint8_t)(rg->dungeon != NULL);
        Rando_NamesPut(w->region_by_name, rg->name, i);
    }
    return 0;
}

static int load_items_acc(RandoWorld *w, const JsonValue *arr, LoadCtx *lc,
                          char *err, size_t errsz)
{
    const JsonValue *it;
    (void)w;
    for (it = arr->child; it; it = it->next) {
        const char *name = Json_AsString(Json_Get(it, "item"));
        int id;
        if (!name) return load_err(err, errsz, "item missing name", NULL);
        id = acc_intern(lc, name);
        if (id < 0) return load_err(err, errsz, "oom items", NULL);
        lc->acc[id].pool_count = Json_AsInt(Json_Get(it, "count"));
        lc->acc[id].progression = Json_AsBool(Json_Get(it, "progression"));
        lc->acc[id].precollected = Json_AsInt(Json_Get(it, "precollected"));
        lc->acc[id].itype = Json_AsString(Json_Get(it, "type"));
        lc->acc[id].known_from_pool = 1;
    }
    return 0;
}

static int intern_location_item_names(const JsonValue *arr, LoadCtx *lc,
                                      char *err, size_t errsz)
{
    const JsonValue *l;
    for (l = arr->child; l; l = l->next) {
        const JsonValue *pi = Json_Get(l, "placed_item");
        const JsonValue *fi = Json_Get(l, "forced_item");
        const char *nm;
        if (pi && (nm = Json_AsString(Json_Get(pi, "name")))) {
            if (acc_intern(lc, nm) < 0) return load_err(err, errsz, "oom", NULL);
        }
        if (fi && (nm = Json_AsString(fi))) {
            if (acc_intern(lc, nm) < 0) return load_err(err, errsz, "oom", NULL);
        }
    }
    return 0;
}

static int finalize_items(RandoWorld *w, LoadCtx *lc, char *err, size_t errsz)
{
    int i;
    w->n_items = lc->n;
    w->items = (RandoItemDef *)calloc((size_t)lc->n, sizeof(RandoItemDef));
    if (!w->items) return load_err(err, errsz, "oom items", NULL);
    w->bytes_allocated += (size_t)lc->n * sizeof(RandoItemDef);
    w->item_by_name = Rando_NamesNew();
    if (!w->item_by_name) return load_err(err, errsz, "oom", NULL);
    for (i = 0; i < lc->n; i++) {
        ItemAcc *a = &lc->acc[i];
        w->items[i].name = xstrdup(a->name);
        w->items[i].itype = a->itype ? xstrdup(a->itype) : NULL;
        w->items[i].pool_count = a->pool_count;
        w->items[i].progression = (uint8_t)a->progression;
        w->items[i].precollected = (uint8_t)a->precollected;
        Rando_NamesPut(w->item_by_name, w->items[i].name, i);
    }
    return 0;
}

static int load_locations(RandoWorld *w, const JsonValue *arr, char *err, size_t errsz)
{
    const JsonValue *l;
    int i = 0;
    w->n_locations = Json_Size(arr);
    w->locations = (RandoLocation *)calloc((size_t)w->n_locations, sizeof(RandoLocation));
    if (!w->locations) return load_err(err, errsz, "oom locations", NULL);
    w->bytes_allocated += (size_t)w->n_locations * sizeof(RandoLocation);
    w->loc_by_name = Rando_NamesNew();
    if (!w->loc_by_name) return load_err(err, errsz, "oom", NULL);
    for (l = arr->child; l; l = l->next, i++) {
        RandoLocation *loc = &w->locations[i];
        const JsonValue *pi;
        const char *rid, *nm;
        if (Json_AsInt(Json_Get(l, "id")) != i)
            return load_err(err, errsz, "location id mismatch (ids must be 0..n-1)", NULL);
        loc->id = i;
        nm = Json_AsString(Json_Get(l, "name"));
        if (!nm) return load_err(err, errsz, "location missing name", NULL);
        loc->name = xstrdup(nm);
        loc->region = Json_AsInt(Json_Get(l, "region"));
        /* regions load before locations; a corrupt region id would
         * otherwise OOB-index reachable[] during the sweeps */
        if (loc->region < 0 || loc->region >= w->n_regions)
            return load_err(err, errsz, "location region id out of range", NULL);
        loc->type = (uint8_t)loc_type_from_name(Json_AsString(Json_Get(l, "type")));
        loc->event = (uint8_t)Json_AsBool(Json_Get(l, "event"));
        loc->real = (uint8_t)Json_AsBool(Json_Get(l, "real"));
        loc->locked = (uint8_t)Json_AsBool(Json_Get(l, "locked"));
        rid = Json_AsString(Json_Get(l, "rule"));
        loc->rule = rid ? Rando_ProgramRuleId(&w->program, rid) : -1;
        if (rid && loc->rule < 0)
            return load_err(err, errsz, "location rule id not compiled: %s", rid);
        pi = Json_Get(l, "placed_item");
        if (pi) {
            const char *in = Json_AsString(Json_Get(pi, "name"));
            loc->placed_item = in ? Rando_ItemId(w, in) : -1;
            loc->placed_progression = (uint8_t)Json_AsBool(Json_Get(pi, "progression"));
            if (in && loc->placed_item < 0)
                return load_err(err, errsz, "placed item not registered: %s", in);
        } else {
            loc->placed_item = -1;
        }
        nm = Json_AsString(Json_Get(l, "forced_item"));
        loc->forced_item = nm ? Rando_ItemId(w, nm) : -1;
        Rando_NamesPut(w->loc_by_name, loc->name, i);
    }
    return 0;
}

static int load_edges(RandoWorld *w, const JsonValue *arr, char *err, size_t errsz)
{
    const JsonValue *e;
    int i = 0;
    w->n_edges = Json_Size(arr);
    w->edges = (RandoEdge *)calloc((size_t)w->n_edges, sizeof(RandoEdge));
    if (!w->edges) return load_err(err, errsz, "oom edges", NULL);
    w->bytes_allocated += (size_t)w->n_edges * sizeof(RandoEdge);
    for (e = arr->child; e; e = e->next, i++) {
        RandoEdge *edge = &w->edges[i];
        const char *rid;
        edge->from = Json_AsInt(Json_Get(e, "from"));
        edge->to = Json_AsInt(Json_Get(e, "to"));
        if (edge->from < 0 || edge->from >= w->n_regions ||
            edge->to < 0 || edge->to >= w->n_regions)
            return load_err(err, errsz, "edge region id out of range", NULL);
        rid = Json_AsString(Json_Get(e, "rule"));
        edge->rule = rid ? Rando_ProgramRuleId(&w->program, rid) : -1;
        if (rid && edge->rule < 0)
            return load_err(err, errsz, "edge rule id not compiled: %s", rid);
        edge->entrance = xstrdup(Json_AsString(Json_Get(e, "entrance")));
        if (Json_AsString(Json_Get(e, "door")))
            edge->door = xstrdup(Json_AsString(Json_Get(e, "door")));
        if (Json_AsString(Json_Get(e, "door_type")))
            edge->door_type = xstrdup(Json_AsString(Json_Get(e, "door_type")));
        edge->spot_type = (uint8_t)Rando_SpotTypeFromName(Json_AsString(Json_Get(e, "spot_type")));
    }
    return 0;
}

static int build_csr(RandoWorld *w, char *err, size_t errsz)
{
    int i;
    w->edge_offsets = (int *)calloc((size_t)w->n_regions + 1, sizeof(int));
    w->edge_order = (int *)calloc((size_t)w->n_edges ? (size_t)w->n_edges : 1, sizeof(int));
    if (!w->edge_offsets || !w->edge_order) return load_err(err, errsz, "oom csr", NULL);
    w->bytes_allocated += ((size_t)w->n_regions + 1) * sizeof(int)
                        + (size_t)w->n_edges * sizeof(int);
    for (i = 0; i < w->n_edges; i++)
        w->edge_offsets[w->edges[i].from + 1]++;
    for (i = 0; i < w->n_regions; i++)
        w->edge_offsets[i + 1] += w->edge_offsets[i];
    {
        int *cursor = (int *)calloc((size_t)w->n_regions, sizeof(int));
        if (!cursor) return load_err(err, errsz, "oom csr", NULL);
        for (i = 0; i < w->n_edges; i++) {
            int from = w->edges[i].from;
            w->edge_order[w->edge_offsets[from] + cursor[from]] = i;
            cursor[from]++;
        }
        free(cursor);
    }
    return 0;
}

static int locate_semantic_groups(RandoWorld *w, char *err, size_t errsz)
{
    static const char *const kCrystals[7] = {
        "Crystal 1", "Crystal 2", "Crystal 3", "Crystal 4",
        "Crystal 5", "Crystal 6", "Crystal 7",
    };
    static const char *const kPendants[3] = { "Green Pendant", "Red Pendant", "Blue Pendant" };
    int i;
    for (i = 0; i < w->n_items; i++) {
        const char *nm = w->items[i].name;
        if (strncmp(nm, "Bottle", 6) == 0 && w->n_bottles < 16)
            w->bottles[w->n_bottles++] = (int16_t)i;
    }
    for (i = 0; i < 7; i++) {
        int id = Rando_ItemId(w, kCrystals[i]);
        if (id < 0) return load_err(err, errsz, "missing crystal item %s", kCrystals[i]);
        w->crystals[w->n_crystals++] = (int16_t)id;
    }
    for (i = 0; i < 3; i++) {
        int id = Rando_ItemId(w, kPendants[i]);
        if (id < 0) return load_err(err, errsz, "missing pendant item %s", kPendants[i]);
        w->pendants[w->n_pendants++] = (int16_t)id;
    }
    w->heart_boss = (int16_t)Rando_ItemId(w, "Boss Heart Container");
    w->heart_sanctuary = (int16_t)Rando_ItemId(w, "Sanctuary Heart Container");
    w->heart_piece = (int16_t)Rando_ItemId(w, "Piece of Heart");
    w->magic_half = (int16_t)Rando_ItemId(w, "Magic Upgrade (1/2)");
    w->magic_quarter = (int16_t)Rando_ItemId(w, "Magic Upgrade (1/4)");
    if (w->heart_boss < 0 || w->heart_sanctuary < 0 || w->heart_piece < 0)
        return load_err(err, errsz, "missing heart items", NULL);

    /* start regions: Menu (the fork's BFS root) + Links House (spawn) */
    {
        static const char *const starts[] = { "Menu", "Links House" };
        int n = 0;
        for (i = 0; i < 2; i++) {
            int r = Rando_RegionId(w, starts[i]);
            if (r >= 0) w->start_regions[n++] = (int16_t)r;
        }
        w->n_start_regions = n;
        if (n == 0) return load_err(err, errsz, "no start regions found", NULL);
    }
    w->goal_location = Rando_LocationId(w, "Ganon");   /* victory spot (goal=ganon) */
    w->victory_item = Rando_ItemId(w, "Triforce");
    return 0;
}

static int load_meta(RandoWorld *w, const JsonValue *meta, char *err, size_t errsz)
{
    const JsonValue *counts = Json_Get(meta, "counts");
    if (!counts) return load_err(err, errsz, "meta.json missing counts", NULL);
    w->meta.seed = Json_AsInt(Json_Get(meta, "seed"));
    w->meta.counts_regions = Json_AsInt(Json_Get(counts, "regions"));
    w->meta.counts_edges = Json_AsInt(Json_Get(counts, "edges"));
    w->meta.counts_locations = Json_AsInt(Json_Get(counts, "locations"));
    w->meta.counts_items_in_pool = Json_AsInt(Json_Get(counts, "items_in_pool"));
    w->meta.counts_item_names = Json_AsInt(Json_Get(counts, "item_names"));
    return 0;
}

int Rando_LoadWorld(const char *dir, RandoWorld *w, char *err, size_t errsz)
{
    char path[1024];
    size_t len;
    char *text[6] = { 0, 0, 0, 0, 0, 0 };
    /* phase-B fix: must be zeroed - the `fail` path below frees dom[0..5],
     * and a missing first file leaves the tail entries uninitialized
     * (wild-pointer crash in Json_Free, ASLR-dependent). */
    JsonValue *dom[6] = { 0, 0, 0, 0, 0, 0 };
    static const char *const kFiles[6] = {
        "regions.json", "edges.json", "locations.json",
        "items.json", "rules.json", "meta.json",
    };
    LoadCtx lc;
    RandoRuleCompiler rc;
    int i, rc_err;
    int pool_item_count;

    if (err && errsz) err[0] = '\0';
    Rando_InitWorld(w);

    memset(&lc, 0, sizeof(lc));
    lc.items = Rando_NamesNew();
    if (!lc.items) return load_err(err, errsz, "oom", NULL);
    g_lc = &lc;
    g_w = w;

    /* 1. read + parse the six files */
    for (i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, kFiles[i]);
        text[i] = read_text_file(path, &len, err, errsz);
        if (!text[i]) { goto fail; }
        dom[i] = Json_Parse(text[i], err, errsz);
        if (!dom[i]) goto fail;
    }

    /* 2. regions (name table must exist before rule compilation) */
    if (load_regions(w, dom[0], err, errsz) != 0) goto fail;

    /* 3. item registry: pool, then placed/forced names */
    if (load_items_acc(w, dom[3], &lc, err, errsz) != 0) goto fail;
    pool_item_count = 0;
    {
        const JsonValue *it;
        for (it = dom[3]->child; it; it = it->next)
            pool_item_count += Json_AsInt(Json_Get(it, "count"));
    }
    if (intern_location_item_names(dom[2], &lc, err, errsz) != 0) goto fail;

    /* 4. compile rules */
    rc.prog = &w->program;
    rc.intern_item = rc_intern_bind;
    rc.lookup_region = rc_lookup_region_bind;
    rc.lookup_door = rc_lookup_door_bind;
    rc.ud = NULL;
    rc.err = err; rc.errsz = errsz;
    rc.opaque_as_false = 0;
    rc_err = Rando_RulesCompile(Json_Get(dom[4], "nodes"), &rc);
    if (rc_err < 0) goto fail;
    if (rc.opaque_as_false)
        printf("[randomizer] WARNING: %d untranslated glitch clause(s) in this "
               "logic tier were treated as unavailable (logic slightly stricter "
               "than the reference randomizer)\n", rc.opaque_as_false);

    /* 5. finalize item defs */
    if (finalize_items(w, &lc, err, errsz) != 0) goto fail;

    /* 6. locations + edges (resolve rule ids) */
    if (load_locations(w, dom[2], err, errsz) != 0) goto fail;
    if (load_edges(w, dom[1], err, errsz) != 0) goto fail;
    if (build_csr(w, err, errsz) != 0) goto fail;

    /* 7. profile tables (key doors + shops) */
    if (Rando_BindProfileTables(w, err, errsz) != 0) goto fail;

    /* 8. semantic groups / start / victory */
    if (locate_semantic_groups(w, err, errsz) != 0) goto fail;

    /* 9. meta */
    if (load_meta(w, dom[5], err, errsz) != 0) goto fail;

    /* sanity vs meta counts */
    if (w->n_regions != w->meta.counts_regions ||
        w->n_edges != w->meta.counts_edges ||
        w->n_locations != w->meta.counts_locations ||
        pool_item_count != w->meta.counts_items_in_pool) {
        load_err(err, errsz, "counts mismatch vs meta.json", NULL);
        goto fail;
    }

    for (i = 0; i < 6; i++) { Json_Free(dom[i]); free(text[i]); }
    Rando_NamesFree(lc.items);
    free(lc.acc);
    return 0;

fail:
    for (i = 0; i < 6; i++) { Json_Free(dom[i]); free(text[i]); }
    Rando_NamesFree(lc.items);
    free(lc.acc);
    Rando_FreeWorld(w);
    return -1;
}

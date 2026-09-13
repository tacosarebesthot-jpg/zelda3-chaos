/* rando_rules.c — rule IR compiler + interpreter.  See rando_rules.h. */
#include "rando_rules.h"
#include "rando_json.h"
#include "rando_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER)
#define _strdup strdup
#endif

/* ================================================================== */
/* Name table                                                          */
/* ================================================================== */

typedef struct NameSlot {
    const char *key;
    uint32_t hash;
    int value;
    int used;
} NameSlot;

struct RandoNameTable {
    NameSlot *slots;
    size_t cap;      /* power of two */
    int count;
};

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h ? h : 1;
}

RandoNameTable *Rando_NamesNew(void)
{
    RandoNameTable *t = (RandoNameTable *)calloc(1, sizeof(RandoNameTable));
    if (!t) return NULL;
    t->cap = 1024;
    t->slots = (NameSlot *)calloc(t->cap, sizeof(NameSlot));
    if (!t->slots) { free(t); return NULL; }
    return t;
}

void Rando_NamesFree(RandoNameTable *t)
{
    size_t i;
    if (!t) return;
    for (i = 0; i < t->cap; i++)
        if (t->slots[i].used) free((void *)t->slots[i].key);
    free(t->slots);
    free(t);
}

static int names_grow(RandoNameTable *t);

void Rando_NamesPut(RandoNameTable *t, const char *key, int value)
{
    uint32_t h = fnv1a(key);
    size_t mask;
    if (t->count * 10 >= (int)t->cap * 7) { if (!names_grow(t)) return; }
    mask = t->cap - 1;
    h &= (uint32_t)mask;
    while (t->slots[h].used) {
        if (strcmp(t->slots[h].key, key) == 0) { t->slots[h].value = value; return; }
        h = (h + 1) & mask;
    }
    t->slots[h].used = 1;
    t->slots[h].hash = 0; /* not needed for lookup */
    t->slots[h].key = _strdup(key);
    t->slots[h].value = value;
    t->count++;
}

int Rando_NamesGet(const RandoNameTable *t, const char *key)
{
    uint32_t h;
    size_t mask;
    if (!t) return -1;
    mask = t->cap - 1;
    h = fnv1a(key) & (uint32_t)mask;
    while (t->slots[h].used) {
        if (strcmp(t->slots[h].key, key) == 0) return t->slots[h].value;
        h = (h + 1) & mask;
    }
    return -1;
}

int Rando_NamesCount(const RandoNameTable *t) { return t ? t->count : 0; }

static int names_grow(RandoNameTable *t)
{
    size_t newcap = t->cap * 2, i;
    NameSlot *ns = (NameSlot *)calloc(newcap, sizeof(NameSlot));
    if (!ns) return 0;
    for (i = 0; i < t->cap; i++) {
        uint32_t h;
        if (!t->slots[i].used) continue;
        h = fnv1a(t->slots[i].key) & (uint32_t)(newcap - 1);
        while (ns[h].used) h = (h + 1) & (newcap - 1);
        ns[h] = t->slots[i];
    }
    free(t->slots);
    t->slots = ns;
    t->cap = newcap;
    return 1;
}

/* ================================================================== */
/* Program                                                             */
/* ================================================================== */

const char *const kRandoRuleOpNames[RR_OP_COUNT_] = {
    "FALSE", "TRUE", "ITEM", "EVENT", "ITEM_SUM",
    "AND", "OR", "NOT", "REACH", "BARRIER",
    "UNLIMITED", "BOTTLES", "CRYSTALS", "PENDANTS",
    "HEARTS", "EXTEND_MAGIC", "SMALL_KEY_DOOR", "REACH_LW", "REACH_DW",
    "OPAQUE",
};

void Rando_ProgramInit(RandoProgram *p)
{
    memset(p, 0, sizeof(*p));
}

void Rando_ProgramFree(RandoProgram *p)
{
    free(p->nodes);
    free(p->idlists);
    Rando_NamesFree(p->rule_index);
    memset(p, 0, sizeof(*p));
}

static int prog_node(RandoProgram *p, const RandoRuleNode *n)
{
    if (p->n_nodes == p->cap_nodes) {
        int nc = p->cap_nodes ? p->cap_nodes * 2 : 1024;
        RandoRuleNode *nn = (RandoRuleNode *)realloc(p->nodes, (size_t)nc * sizeof(RandoRuleNode));
        if (!nn) return -1;
        p->nodes = nn; p->cap_nodes = nc;
    }
    p->nodes[p->n_nodes] = *n;
    return p->n_nodes++;
}

static int prog_idlist(RandoProgram *p, const uint16_t *ids, int n)
{
    int start = p->n_idlists, i;
    if (p->n_idlists + n > p->cap_idlists) {
        int nc = p->cap_idlists ? p->cap_idlists : 256;
        uint16_t *nl;
        while (p->n_idlists + n > nc) nc *= 2;
        nl = (uint16_t *)realloc(p->idlists, (size_t)nc * sizeof(uint16_t));
        if (!nl) return -1;
        p->idlists = nl; p->cap_idlists = nc;
    }
    for (i = 0; i < n; i++) p->idlists[p->n_idlists++] = ids[i];
    return start;
}

int Rando_ProgramRuleId(RandoProgram *prog, const char *rule_id)
{
    return Rando_NamesGet(prog->rule_index, rule_id);
}

void Rando_ProgramHistogram(const RandoProgram *prog, int counts[RR_OP_COUNT_])
{
    int i;
    memset(counts, 0, sizeof(int) * RR_OP_COUNT_);
    for (i = 0; i < prog->n_nodes; i++)
        if (prog->nodes[i].op < RR_OP_COUNT_) counts[prog->nodes[i].op]++;
}

/* ================================================================== */
/* Compiler                                                            */
/* ================================================================== */

static int compile_tree(RandoRuleCompiler *rc, const JsonValue *n);

static int compile_error(RandoRuleCompiler *rc, const char *fmt, const char *arg)
{
    if (rc->err && rc->errsz > 0)
        snprintf(rc->err, rc->errsz, fmt, arg ? arg : "");
    return -1;
}

static int compile_bool_combine(RandoRuleCompiler *rc, int is_and,
                                const JsonValue *kids)
{
    const JsonValue *c;
    /* child ROOT indices are not contiguous (post-order emission), so they
     * are stored as an idlist and the parent node references the list */
    uint16_t *child_ids = NULL;
    int n = 0, cap = 0, list_start, self;
    RandoRuleNode node;
    for (c = kids ? kids->child : NULL; c; c = c->next) {
        int idx = compile_tree(rc, c);
        if (idx < 0) { free(child_ids); return -1; }
        if (n == cap) {
            int nc = cap ? cap * 2 : 8;
            uint16_t *ni = (uint16_t *)realloc(child_ids, (size_t)nc * sizeof(uint16_t));
            if (!ni) { free(child_ids); return compile_error(rc, "oom", NULL); }
            child_ids = ni; cap = nc;
        }
        child_ids[n++] = (uint16_t)idx;
    }
    if (n == 0) {
        free(child_ids);
        node.op = (uint8_t)(is_and ? RR_TRUE : RR_FALSE);
        node.flags = 0; node.count = 0; node.a = 0;
        return prog_node(rc->prog, &node);
    }
    if (n == 1) {
        int only = child_ids[0];
        free(child_ids);
        return only;
    }
    list_start = prog_idlist(rc->prog, child_ids, n);
    free(child_ids);
    if (list_start < 0) return compile_error(rc, "oom", NULL);
    node.op = (uint8_t)(is_and ? RR_AND : RR_OR);
    node.flags = 0; node.count = (uint16_t)n; node.a = list_start;
    self = prog_node(rc->prog, &node);
    return self;
}

static int compile_tree(RandoRuleCompiler *rc, const JsonValue *n)
{
    const char *op;
    RandoRuleNode node;

    if (!n || n->type != JSON_OBJECT)
        return compile_error(rc, "rule node is not an object", NULL);
    op = Json_AsString(Json_Get(n, "op"));
    if (!op) return compile_error(rc, "rule node missing op", NULL);
    memset(&node, 0, sizeof(node));

    if (strcmp(op, "static") == 0) {
        node.op = (uint8_t)(Json_AsBool(Json_Get(n, "value")) ? RR_TRUE : RR_FALSE);
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "ref") == 0) {
        /* shared sub-rule: compile the registry entry once, reuse its index.
         * -2 marks "being compiled" so a self-referential dump is an error
         * instead of infinite recursion. */
        const char *id = Json_AsString(Json_Get(n, "id"));
        const JsonValue *target;
        int idx;
        if (!id) return compile_error(rc, "'ref' needs an id", NULL);
        if (!rc->prog->rule_index)
            rc->prog->rule_index = Rando_NamesNew();
        idx = Rando_NamesGet(rc->prog->rule_index, id);
        if (idx >= 0) return idx;
        if (idx == -2) return compile_error(rc, "ref cycle at rule", id);
        target = rc->registry ? Json_Get(rc->registry, id) : NULL;
        if (!target) return compile_error(rc, "ref: unknown rule id", id);
        Rando_NamesPut(rc->prog->rule_index, id, -2);
        idx = compile_tree(rc, target);
        if (idx < 0) return -1;
        Rando_NamesPut(rc->prog->rule_index, id, idx);
        return idx;
    }
    if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0)
        return compile_bool_combine(rc, op[0] == 'a', Json_Get(n, "nodes"));
    if (strcmp(op, "not") == 0) {
        const JsonValue *c = Json_Get(n, "nodes");
        int kid;
        if (!c || c->type != JSON_ARRAY || Json_Size(c) != 1)
            return compile_error(rc, "'not' needs exactly one child", NULL);
        kid = compile_tree(rc, c->child);
        if (kid < 0) return -1;
        node.op = RR_NOT; node.a = kid;
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "item") == 0) {
        const char *name = Json_AsString(Json_Get(n, "item"));
        int id;
        if (!name) return compile_error(rc, "'item' missing name", NULL);
        id = rc->intern_item(rc->ud, name);
        node.op = RR_ITEM;
        node.count = (uint16_t)(Json_AsInt(Json_Get(n, "count")) <= 0
                                ? 1 : Json_AsInt(Json_Get(n, "count")));
        node.a = id;
        node.flags = (uint8_t)(Json_AsBool(Json_Get(n, "key")) ? 1 : 0);
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "item_count_sum") == 0) {
        const JsonValue *items = Json_Get(n, "items");
        const JsonValue *c;
        uint16_t ids[64];
        int cnt = 0, start;
        if (!items || items->type != JSON_ARRAY)
            return compile_error(rc, "'item_count_sum' missing items", NULL);
        for (c = items->child; c; c = c->next) {
            const char *nm = Json_AsString(c);
            if (!nm || cnt >= 64) return compile_error(rc, "bad item_count_sum", NULL);
            ids[cnt++] = (uint16_t)rc->intern_item(rc->ud, nm);
        }
        start = prog_idlist(rc->prog, ids, cnt);
        if (start < 0) return compile_error(rc, "oom", NULL);
        node.op = RR_ITEM_SUM;
        node.flags = (uint8_t)cnt;
        node.count = (uint16_t)Json_AsInt(Json_Get(n, "count"));
        node.a = start;
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "reachable") == 0) {
        const char *spot = Json_AsString(Json_Get(n, "spot"));
        const char *stype = Json_AsString(Json_Get(n, "spot_type"));
        int rid;
        if (!spot) return compile_error(rc, "'reachable' missing spot", NULL);
        if (stype && strcmp(stype, "Region") != 0)
            return compile_error(rc, "unsupported reachable spot_type", stype);
        rid = rc->lookup_region(rc->ud, spot);
        if (rid < 0) return compile_error(rc, "reachable: unknown region", spot);
        node.op = RR_REACH; node.a = rid;
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "barrier") == 0) {
        const char *spot = Json_AsString(Json_Get(n, "region"));
        const char *bar = Json_AsString(Json_Get(n, "barrier"));
        int rid;
        if (!spot) return compile_error(rc, "'barrier' missing region", NULL);
        rid = rc->lookup_region(rc->ud, spot);
        if (rid < 0) return compile_error(rc, "barrier: unknown region", spot);
        node.op = RR_BARRIER;
        node.flags = (uint8_t)((bar && strcmp(bar, "blue") == 0)
                               ? RR_BARRIER_BLUE : RR_BARRIER_ORANGE);
        node.a = rid;
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "unlimited") == 0) {
        /* The shop table is profile data; the loader pre-resolves the shop
         * regions per item name into world->unlimited.  We emit a marker
         * node referencing the item id; the interpreter matches it against
         * world->unlimited at eval time (linear scan over a 3-entry table). */
        const char *name = Json_AsString(Json_Get(n, "item"));
        int id;
        if (!name) return compile_error(rc, "'unlimited' missing item", NULL);
        id = rc->intern_item(rc->ud, name);
        node.op = RR_UNLIMITED; node.a = id;
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "bottles") == 0) {
        node.op = RR_BOTTLES;
        node.count = (uint16_t)(Json_AsInt(Json_Get(n, "count")) <= 0 ? 1 : Json_AsInt(Json_Get(n, "count")));
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "crystals") == 0) {
        node.op = RR_CRYSTALS;
        node.count = (uint16_t)Json_AsInt(Json_Get(n, "count"));
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "pendants") == 0) {
        node.op = RR_PENDANTS;
        node.count = (uint16_t)Json_AsInt(Json_Get(n, "count"));
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "hearts") == 0) {
        node.op = RR_HEARTS;
        node.count = (uint16_t)Json_AsInt(Json_Get(n, "count"));
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "extend_magic") == 0) {
        node.op = RR_EXTEND_MAGIC;
        node.count = (uint16_t)Json_AsInt(Json_Get(n, "magic"));
        node.flags = (uint8_t)(Json_AsBool(Json_Get(n, "fullrefill")) ? 1 : 0);
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "small_key_door") == 0) {
        /* node.a = static key-door table index (profile data bound by the
         * loader via Rando_BindProfileTables in the same order) */
        const char *door = Json_AsString(Json_Get(n, "door"));
        int di;
        if (!door) return compile_error(rc, "'small_key_door' missing door", NULL);
        di = rc->lookup_door ? rc->lookup_door(rc->ud, door) : -1;
        if (di < 0) return compile_error(rc, "small_key_door not in profile table: %s", door);
        node.op = RR_SMALL_KEY_DOOR;
        node.a = di;
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "not_bunny") == 0) {
        const char *region = Json_AsString(Json_Get(n, "region"));
        int rid;
        if (!region) return compile_error(rc, "'not_bunny' missing region", NULL);
        rid = rc->lookup_region ? rc->lookup_region(rc->ud, region) : -1;
        if (rid < 0) return compile_error(rc, "not_bunny: unknown region", region);
        node.op = RR_NOT_BUNNY;
        node.a = rid;
        node.count = (uint16_t)rc->intern_item(rc->ud, "Moon Pearl");
        return prog_node(rc->prog, &node);
    }
    if (strcmp(op, "reach_light_world") == 0) { node.op = RR_REACH_LW; return prog_node(rc->prog, &node); }
    if (strcmp(op, "reach_dark_world") == 0) { node.op = RR_REACH_DW; return prog_node(rc->prog, &node); }
    if (strcmp(op, "opaque") == 0) {
        rc->opaque_as_false++;
        node.op = RR_FALSE;
        return prog_node(rc->prog, &node);
    }
    /* ops present in the IR spec but not in the committed dumps: fail loud */
    return compile_error(rc, "unsupported rule op: %s", op);
}

int Rando_RulesCompile(struct JsonValue *rules_nodes_obj, RandoRuleCompiler *rc)
{
    const JsonValue *c;
    if (!rules_nodes_obj || rules_nodes_obj->type != JSON_OBJECT) {
        compile_error(rc, "rules 'nodes' is not an object", NULL);
        return -1;
    }
    rc->registry = rules_nodes_obj;
    if (!rc->prog->rule_index)
        rc->prog->rule_index = Rando_NamesNew();
    for (c = rules_nodes_obj->child; c; c = c->next) {
        int idx = Rando_NamesGet(rc->prog->rule_index, c->key);
        if (idx >= 0) continue;              /* already compiled via a ref */
        Rando_NamesPut(rc->prog->rule_index, c->key, -2);
        idx = compile_tree(rc, c);
        if (idx < 0) return -1;
        Rando_NamesPut(rc->prog->rule_index, c->key, idx);
    }
    return Rando_NamesCount(rc->prog->rule_index);
}

/* ================================================================== */
/* Interpreter                                                         */
/* ================================================================== */

static int eval_node(const RandoProgram *prog, int index, RandoEvalCtx *ctx);

/* count of distinct held item names matching a predicate list */
static int distinct_held(const RandoEvalCtx *ctx, const int16_t *ids, int n)
{
    int i, k = 0;
    for (i = 0; i < n; i++)
        if (ctx->count[ids[i]] > 0) k++;
    return k;
}

static int eval_kids(const RandoProgram *prog, const RandoRuleNode *node,
                     RandoEvalCtx *ctx, int want_all)
{
    int i;
    for (i = 0; i < (int)node->count; i++) {
        int v = eval_node(prog, prog->idlists[node->a + i], ctx);
        if (ctx->depth_exceeded) return 0;
        if (want_all && !v) return 0;
        if (!want_all && v) return 1;
    }
    return want_all ? 1 : 0;
}

static int eval_node(const RandoProgram *prog, int index, RandoEvalCtx *ctx)
{
    const RandoRuleNode *node;
    if (index < 0 || index >= prog->n_nodes) { ctx->depth_exceeded = 1; return 0; }
    node = &prog->nodes[index];
    if (++ctx->_depth > ctx->max_depth) {
        ctx->depth_exceeded = 1;
        ctx->_depth--;
        return 0;
    }
    switch (node->op) {
    case RR_FALSE: ctx->_depth--; return 0;
    case RR_TRUE:  ctx->_depth--; return 1;
    case RR_ITEM:
    case RR_EVENT:
        { int v = ctx->count[node->a] >= node->count; ctx->_depth--; return v; }
    case RR_ITEM_SUM: {
        int i, sum = 0;
        for (i = 0; i < (int)node->flags; i++)
            sum += ctx->count[prog->idlists[node->a + i]];
        ctx->_depth--;
        return sum >= node->count;
    }
    case RR_AND: { int v = eval_kids(prog, node, ctx, 1); ctx->_depth--; return v; }
    case RR_OR:  { int v = eval_kids(prog, node, ctx, 0); ctx->_depth--; return v; }
    case RR_NOT: {
        int v = eval_node(prog, node->a, ctx);
        if (ctx->depth_exceeded) { ctx->_depth--; return 0; }
        ctx->_depth--;
        return !v;
    }
    case RR_REACH:
        { int v = ctx->reachable[node->a] != 0; ctx->_depth--; return v; }
    case RR_BARRIER: {
        int v = ctx->reachable[node->a] &&
                (ctx->barrier[node->a] & node->flags) == node->flags;
        ctx->_depth--;
        return v;
    }
    case RR_UNLIMITED: {
        /* world->unlimited maps item id -> list of shop regions */
        const struct RandoWorld *w = ctx->world;
        int i, j, v = 0;
        for (i = 0; w && i < w->n_unlimited && !v; i++) {
            if (w->unlimited[i].item != node->a) continue;
            for (j = 0; j < w->unlimited[i].n_regions; j++)
                if (ctx->reachable[w->unlimited[i].regions[j]]) { v = 1; break; }
        }
        ctx->_depth--;
        return v;
    }
    case RR_BOTTLES: {
        int v = distinct_held(ctx, ctx->world ? ctx->world->bottles : NULL,
                              ctx->world ? ctx->world->n_bottles : 0) >= node->count;
        ctx->_depth--;
        return v;
    }
    case RR_CRYSTALS: {
        int v = distinct_held(ctx, ctx->world->crystals, ctx->world->n_crystals) >= node->count;
        ctx->_depth--;
        return v;
    }
    case RR_PENDANTS: {
        int v = distinct_held(ctx, ctx->world->pendants, ctx->world->n_pendants) >= node->count;
        ctx->_depth--;
        return v;
    }
    case RR_HEARTS: {
        /* fork heart_count with the profile's difficulty limits (normal:
         * limits 255 -> no clamping) */
        const struct RandoWorld *w = ctx->world;
        int hearts = 3 + ctx->count[w->heart_boss] + ctx->count[w->heart_sanctuary]
                       + ctx->count[w->heart_piece] / 4;
        ctx->_depth--;
        return hearts >= (int)node->count;
    }
    case RR_EXTEND_MAGIC: {
        /* fork can_extend_magic (difficulty normal): basemagic 8/16/32,
         * doubled per distinct bottle when unlimited potions reachable */
        const struct RandoWorld *w = ctx->world;
        int basemagic, i, v;
        if (w->magic_quarter >= 0 && ctx->count[w->magic_quarter] > 0) basemagic = 32;
        else if (w->magic_half >= 0 && ctx->count[w->magic_half] > 0) basemagic = 16;
        else basemagic = 8;
        for (i = 0; i < w->n_unlimited; i++) {
            int j, n;
            if (w->unlimited[i].item != w->unlimited_magic_potions[0] &&
                w->unlimited[i].item != w->unlimited_magic_potions[1]) continue;
            /* cache n_regions: the escape below sets i = n_unlimited and the
             * inner loop condition would then read unlimited[i] out of
             * bounds (found by the fill soak run under ASan) */
            n = w->unlimited[i].n_regions;
            for (j = 0; j < n; j++) {
                if (ctx->reachable[w->unlimited[i].regions[j]]) {
                    basemagic += basemagic * distinct_held(ctx, w->bottles, w->n_bottles);
                    j = n;
                    i = w->n_unlimited;
                }
            }
        }
        v = basemagic >= (int)node->count;
        ctx->_depth--;
        return v;
    }
    case RR_SMALL_KEY_DOOR: {
        /* partial variant (see rando_state.c table) */
        const struct RandoWorld *w = ctx->world;
        const RandoKeyDoor *kd = &w->keydoors[node->a];
        int k, v;
        if (kd->crystal_alt) {
            v = ctx->count[kd->key_item] >= kd->crystal_alt;
            ctx->_depth--;
            return v;
        }
        k = ctx->count[kd->key_item];
        /* partial variant: WorstCase capped by small_key_num */
        v = k >= (int)(kd->small_key_num < kd->worstcase ? kd->small_key_num
                                                         : kd->worstcase);
        if (!v && kd->allowsmall_loc >= 0) {
            int loc = kd->allowsmall_loc;
            v = (w->locations[loc].placed_item == kd->key_item);
        }
        if (!v && kd->lock_item >= 0) {
            int i;
            for (i = 0; i < (int)kd->n_lock_locs; i++) {
                int loc = kd->lock_locs[i];
                if (w->locations[loc].placed_item == kd->lock_item) {
                    v = k >= (int)kd->lock_need;
                    break;
                }
            }
        }
        ctx->_depth--;
        return v;
    }
    case RR_NOT_BUNNY: {
        /* BaseClasses.is_not_bunny: has_Pearl or not region.can_cause_bunny;
         * can_cause_bunny is "region is dark world" for non-inverted modes
         * (every profile dumped here is mode=open, not inverted). */
        const struct RandoWorld *w = ctx->world;
        int v = ctx->count[node->count] > 0 || !w->regions[node->a].dark_world;
        ctx->_depth--;
        return v;
    }
    case RR_REACH_LW:
    case RR_REACH_DW: {
        const struct RandoWorld *w = ctx->world;
        int i, v = 0;
        for (i = 0; i < w->n_regions; i++) {
            if (!ctx->reachable[i]) continue;
            if (node->op == RR_REACH_LW ? w->regions[i].light_world
                                        : w->regions[i].dark_world) { v = 1; break; }
        }
        ctx->_depth--;
        return v;
    }
    default:
        ctx->depth_exceeded = 1;
        ctx->_depth--;
        return 0;
    }
}

int Rando_RuleEval(const RandoProgram *prog, int index, RandoEvalCtx *ctx)
{
    if (ctx->max_depth <= 0) ctx->max_depth = RANDO_EVAL_MAX_DEPTH;
    ctx->_depth = 0;
    return eval_node(prog, index, ctx);
}

/* rando_rules.h — rule IR: flat tagged-union node array + interpreter.
 *
 * The phase-0 dump stores every access rule as a small JSON tree under a
 * deduplicated id ("r" + 10 hex).  The compiler flattens each tree into a
 * node array (opcodes below); edges/locations reference entry-point
 * indices.  Per SCHEMA_NOTES.md this is the recommended representation
 * (tagged union / "bytecode", NOT function pointers).
 *
 * Evaluation is a short-circuit recursive interpreter with a depth cap.
 * Ops that consult the reachable-region set (RR_REACH, RR_BARRIER,
 * RR_UNLIMITED) read the caller-provided snapshot arrays — the BFS driver
 * (rando_state.c) iterates sweeps to a fixpoint, which is what keeps the
 * reachability cycle from recursing forever (equivalent to the fork's
 * assume-reachable cache / spurious_logic trick).
 */
#ifndef RANDO_RULES_H
#define RANDO_RULES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct JsonValue;
struct RandoWorld;

/* ------------------------------------------------------------------ */
/* Minimal open-addressing string -> int hash table                    */
/* (keys are owned copies; used for rule ids and name lookups)         */
/* ------------------------------------------------------------------ */

typedef struct RandoNameTable RandoNameTable;

RandoNameTable *Rando_NamesNew(void);
void            Rando_NamesFree(RandoNameTable *t);
void            Rando_NamesPut(RandoNameTable *t, const char *key, int value);   /* copies key */
int             Rando_NamesGet(const RandoNameTable *t, const char *key);        /* -1 if absent */
int             Rando_NamesCount(const RandoNameTable *t);

/* ------------------------------------------------------------------ */
/* Opcodes                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    RR_FALSE = 0,        /* constant false                              */
    RR_TRUE,             /* constant true                               */
    RR_ITEM,             /* counter[item_id] >= count                   */
    RR_EVENT,            /* same as RR_ITEM (item is an event item)     */
    RR_ITEM_SUM,         /* sum of counters of a list >= count          */
    RR_AND,              /* short-circuit AND of count children         */
    RR_OR,               /* short-circuit OR of count children          */
    RR_NOT,              /* negation of one child                       */
    RR_REACH,            /* region `a` is reachable (spot_type Region)  */
    RR_BARRIER,          /* region `a` reachable AND barrier flag match */
    RR_UNLIMITED,        /* any region in shop list reachable           */
    RR_BOTTLES,          /* distinct "Bottle*" counters >= count        */
    RR_CRYSTALS,         /* distinct Crystal 1..7 counters >= count     */
    RR_PENDANTS,         /* distinct pendant counters >= count          */
    RR_HEARTS,           /* BHC + SHC + pieces//4 + 3 >= count          */
    RR_EXTEND_MAGIC,     /* can_extend_magic(count, fullrefill)         */
    RR_SMALL_KEY_DOOR,   /* keydoors[a] partial algorithm               */
    RR_REACH_LW,         /* any reachable region is light_world         */
    RR_REACH_DW,         /* any reachable region is dark_world          */
    RR_NOT_BUNNY,        /* state.is_not_bunny(region): Moon Pearl held, or the
                            region cannot bunny Link (non-inverted: it is not
                            dark_world).  a = region id, count = pearl item id */
    RR_OPAQUE,           /* untranslatable subtree: compiles as FALSE (counted) */
    RR_OP_COUNT_
} RandoRuleOp;

extern const char *const kRandoRuleOpNames[RR_OP_COUNT_];

/* barrier flag values (node.flags for RR_BARRIER) */
enum { RR_BARRIER_ORANGE = 1, RR_BARRIER_BLUE = 2 };

typedef struct RandoRuleNode {
    uint8_t op;
    uint8_t flags;       /* RR_BARRIER: orange/blue; RR_EXTEND_MAGIC: bit0 fullrefill;
                            RR_ITEM: bit0 = fork has_sm_key flag; RR_ITEM_SUM/RR_UNLIMITED: list length */
    uint16_t count;      /* required count / child count / magic amount */
    int32_t a;           /* item id, region id, first-child index, keydoor index,
                            or start index into program idlists                 */
} RandoRuleNode;

typedef struct RandoProgram {
    RandoRuleNode *nodes;    int n_nodes, cap_nodes;
    uint16_t *idlists;       int n_idlists, cap_idlists;  /* shared u16 id lists */
    struct RandoNameTable *rule_index;   /* rule id ("rXXXXXXXXXX") -> entry node index */
} RandoProgram;

void Rando_ProgramInit(RandoProgram *p);
void Rando_ProgramFree(RandoProgram *p);

/* ------------------------------------------------------------------ */
/* Compiler                                                            */
/* ------------------------------------------------------------------ */

/* The compiler interns item names through this callback (rules may name
 * event items that are not in the item pool).  Region names must already
 * be known to the world; unknown regions are a hard error. */
typedef struct RandoRuleCompiler {
    RandoProgram *prog;
    int (*intern_item)(void *ud, const char *name);    /* returns id  */
    int (*lookup_region)(void *ud, const char *name);  /* -1 = error  */
    int (*lookup_door)(void *ud, const char *door);    /* -1 = error  */
    void *ud;
    char *err; size_t errsz;
    /* set by Rando_RulesCompile: the rules.json "nodes" registry, so a
     * {"op":"ref","id":"rXXXX"} node can compile (once) and share the
     * registered tree instead of embedding a copy.  The glitch-tier dumps
     * lean on this: expanded bunny/path options are shared thousands of
     * times and would otherwise inflate rules.json from 170 KB to 150 MB. */
    const struct JsonValue *registry;
    /* count of {"op":"opaque"} nodes compiled as FALSE.  An opaque node is a
     * glitch clause the dump tool could not translate (hybridglitches has a
     * handful of underworld clip rules); treating it as "that route is not
     * available" keeps the logic conservative instead of refusing the world.
     * The loader prints the count so nobody mistakes it for full parity. */
    int opaque_as_false;
} RandoRuleCompiler;

/* Compile all trees of rules.json "nodes" (a JSON object id -> tree).
 * Returns entry count (>0) or -1 with rc->err set.  Caller keeps the
 * rule ids in prog->rule_index for edge/location resolution:
 *   int Rando_ProgramRuleId(RandoProgram*, const char *rule_id);
 * (-1 when the id is unknown). */
int  Rando_RulesCompile(struct JsonValue *rules_nodes_obj, RandoRuleCompiler *rc);
int  Rando_ProgramRuleId(RandoProgram *prog, const char *rule_id);

/* histogram: fills counts[op] (array of RR_OP_COUNT_ ints) */
void Rando_ProgramHistogram(const RandoProgram *prog, int counts[RR_OP_COUNT_]);

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */

typedef struct RandoEvalCtx {
    const struct RandoWorld *world;
    const uint16_t *count;      /* item id -> counter                    */
    const uint8_t *reachable;   /* region id -> 0/1 (snapshot or live)   */
    const uint8_t *barrier;     /* region id -> RR_BARRIER_* mask        */
    int max_depth;              /* recursion cap (default 64)            */
    int depth_exceeded;         /* set (sticky) if the cap is hit        */
    int _depth;
} RandoEvalCtx;

#define RANDO_EVAL_MAX_DEPTH 64

/* evaluate entry point `index`; TRUE/FALSE rules cost O(1) */
int Rando_RuleEval(const RandoProgram *prog, int index, RandoEvalCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RANDO_RULES_H */

/* rando_state.h — CollectionState + reachability BFS driver.
 *
 * Mirrors the fork's CollectionState as recommended by SCHEMA_NOTES.md:
 * a bag of per-item counters plus a reachable-region set.  Rule evaluation
 * reads the live state; the sweep iterates BFS + event collection to a
 * fixpoint, which reproduces the fork's update_reachable_regions /
 * sweep_for_events loop without any recursion into the graph search.
 */
#ifndef RANDO_STATE_H
#define RANDO_STATE_H

#include <stddef.h>

#include "rando_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RandoState {
    const RandoWorld *world;
    uint16_t *count;        /* item id -> counter                        */
    uint8_t *reachable;     /* region id -> 0/1                          */
    uint8_t *barrier;       /* region id -> RR_BARRIER_* mask            */
    uint8_t *collected;     /* location id -> 0/1                        */
    int queue;              /* scratch (BFS queue allocated separately)  */
    int sweep_rounds;       /* fixpoint iterations of the last sweep     */
    int eval_depth_exceeded;/* sticky flag from rule evaluation          */
    void *_queue_mem;       /* internal BFS queue                        */
} RandoState;

RandoState *Rando_StateNew(const RandoWorld *world);   /* applies precollected items */
void        Rando_StateFree(RandoState *st);
void        Rando_StateReset(RandoState *st);          /* zero counters/sets, re-apply precollected */

/* fork CollectionState.collect(item, event=true): progressive chains are
 * folded (Fighter->Master->Tempered->Golden, gloves, shields, bow, mail),
 * bottles capped at the profile limit, everything else bumps its own
 * counter. */
void Rando_StateCollectItem(RandoState *st, int item_id);

/* one collection pass over locations; returns 1 if something new was
 * collected (events AND normal spots — can_beat_game semantics).  Items
 * whose id equals skip_item are left in place (<= 0 disables). */
int Rando_StateCollectPass(RandoState *st, int skip_item);

/* full fixpoint: BFS over edges (traversable = from-region reachable AND
 * rule TRUE under current counters/sets) + collection passes until stable.
 * Returns the number of reachable regions. */
int Rando_SweepReachability(RandoState *st, int collect, int skip_item);

/* one BFS wave only (no collection) — for structural reachability tests */
int Rando_SweepReachabilityNoCollect(RandoState *st);

int  Rando_StateCanReachRegion(const RandoState *st, int region_id);
int  Rando_StateHas(const RandoState *st, int item_id, int count);
int  Rando_StateLocationReachable(const RandoState *st, int loc_id);

/* bind the built-in vanilla-profile key-door / shop tables into the world
 * (resolves names -> ids).  Called by the loader.  0 on success. */
int Rando_BindProfileTables(RandoWorld *w, char *err, size_t errsz);

/* static key-door table lookup by door name (for the rule compiler);
 * returns the kKeyDoors index or -1 */
int Rando_KeyDoorIndex(const char *door);

#ifdef __cplusplus
}
#endif

#endif /* RANDO_STATE_H */

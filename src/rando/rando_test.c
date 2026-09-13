/* rando_test.c — STANDALONE test main for the phase-1 rando logic engine.
 *
 * NOT part of zelda3.exe: build_msvc.cmd globs only src\*.c (non-recursive),
 * so files under src\rando\ never enter the main build.  Build this test
 * explicitly (see tools/RANDO_PHASE1.md):
 *
 *   cl /nologo /std:c11 /W0 /O2 /I. /DRANDO_TEST src\rando\*.c ^
 *      /Fe:tools\rando_test.exe /Fo:tools\rando_test_build\
 *
 * Usage: rando_test.exe [seed_dir ...]     (default: seed_1234 seed_5678)
 *
 * The module is engine-independent: no g_ram, no SDL, no zelda3 headers.
 */
#ifdef RANDO_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rando_load.h"
#include "rando_state.h"
#include "rando_rules.h"
#include "rando_fill.h"
#include "rando_json.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>   /* _mkdir / _rmdir for the corrupt-dump test */
static double now_ms(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <unistd.h>
#include <sys/stat.h>
static double now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}
#endif

static int g_pass = 0, g_fail = 0;

/* ---- small file helpers for the phase-B honesty tests ---------------- */

/* read a whole file; malloc'd NUL-terminated buffer or NULL */
static char *test_read_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[sz] = '\0';
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static int test_copy_file(const char *src, const char *dst)
{
    size_t len;
    char *text = test_read_all(src, &len);
    FILE *f;
    if (!text) return -1;
    f = fopen(dst, "wb");
    if (!f) { free(text); return -1; }
    fwrite(text, 1, len, f);
    fclose(f);
    free(text);
    return 0;
}

/* recover each location's ROM address from locations.json (-1 when absent),
 * like randomizer.c does for the chest-home classification */
static int *test_parse_addresses(const char *dir, int n_locations)
{
    char path[1024], err[128];
    char *text;
    JsonValue *root;
    const JsonValue *v;
    int *addr, i;
    snprintf(path, sizeof(path), "%s\\locations.json", dir);
    text = test_read_all(path, NULL);
    if (!text) return NULL;
    root = Json_Parse(text, err, sizeof(err));
    free(text);
    if (!root || root->type != JSON_ARRAY) { Json_Free(root); return NULL; }
    addr = (int *)malloc((size_t)n_locations * sizeof(int));
    if (!addr) { Json_Free(root); return NULL; }
    for (i = 0; i < n_locations; i++) addr[i] = -1;
    for (v = root->child; v; v = v->next) {
        const JsonValue *id = Json_Get(v, "id");
        const JsonValue *address = Json_Get(v, "address");
        int loc_id = id ? Json_AsInt(id) : -1;
        const char *s;
        if (!id || !address || loc_id < 0 || loc_id >= n_locations) continue;
        if (address->type != JSON_STRING) continue;
        s = Json_AsString(address);
        if (strncmp(s, "0x", 2) != 0) continue;
        addr[loc_id] = (int)strtoul(s + 2, NULL, 16);
    }
    Json_Free(root);
    return addr;
}

/* crystal names for the (c) report */
static const char *const kCrystalNames[7] = {
    "Crystal 1", "Crystal 2", "Crystal 3", "Crystal 4",
    "Crystal 5", "Crystal 6", "Crystal 7",
};

static void check(const char *name, int ok, const char *detail)
{
    printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
           detail && detail[0] ? " — " : "", detail ? detail : "");
    if (ok) g_pass++; else g_fail++;
}

static void print_histogram(const RandoWorld *w)
{
    int counts[RR_OP_COUNT_];
    int i;
    Rando_ProgramHistogram(&w->program, counts);
    printf("  rule nodes: %d (entry rules: %d), idlists: %d\n",
           w->program.n_nodes, Rando_NamesCount(w->program.rule_index),
           w->program.n_idlists);
    for (i = 0; i < RR_OP_COUNT_; i++)
        if (counts[i])
            printf("  %-16s %d\n", kRandoRuleOpNames[i], counts[i]);
}

static void test_seed(const char *dir)
{
    RandoWorld w;
    RandoState *st;
    char err[512];
    double t0, t1;
    int nreach, empty_menu, empty_lh;
    int ganon_loc, triforce, hammer, i;
    /* hammer-only-gated regions (single edge, and(Hammer, ...) rules) */
    static const char *const hammer_regions[] = {
        "Hammer Peg Cave",              /* peg cave behind Dark World pegs   */
        "Village of Outcasts Bush Yard",/* bush yard pegs                    */
        "Dark C Whirlpool Portal Area", /* whirlpool pegs                    */
    };
    /* same kind of gate but with an alternate path: must NOT flip */
    static const char *const alt_path_regions[] = {
        "Darkness Nook Area",
    };

    printf("==================================================================\n");
    printf("seed dir: %s\n", dir);
    printf("==================================================================\n");

    /* ---- (a) load -------------------------------------------------- */
    t0 = now_ms();
    if (Rando_LoadWorld(dir, &w, err, sizeof(err)) != 0) {
        check("(a) load dumps", 0, err);
        return;
    }
    t1 = now_ms();
    {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "%d regions, %d edges, %d locations, %d item defs, %zu bytes model, %.1f ms",
                 w.n_regions, w.n_edges, w.n_locations, w.n_items,
                 w.bytes_allocated, t1 - t0);
        check("(a) load dumps + meta counts cross-check", 1, detail);
    }

    print_histogram(&w);

    /* ---- (b) empty-state reachability ------------------------------- */
    st = Rando_StateNew(&w);
    if (!st) { check("(b) state alloc", 0, "oom"); Rando_FreeWorld(&w); return; }

    t0 = now_ms();
    nreach = Rando_SweepReachabilityNoCollect(st);
    t1 = now_ms();
    empty_menu = nreach;
    {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "%d / %d regions reachable in %d fixpoint rounds (%.2f ms)",
                 nreach, w.n_regions, st->sweep_rounds, t1 - t0);
        check("(b) empty-state BFS from Menu+LinksHouse", nreach > 100, detail);
    }
    /* Links House only: rebuild the start set with just Links House */
    {
        int lh = Rando_RegionId(&w, "Links House");
        int saved_n = w.n_start_regions;
        int16_t saved[4];
        memcpy(saved, w.start_regions, sizeof(saved));
        if (lh >= 0) {
            w.start_regions[0] = (int16_t)lh;
            w.n_start_regions = 1;
        } else {
            w.n_start_regions = 0;
        }
        Rando_StateReset(st);
        empty_lh = Rando_SweepReachabilityNoCollect(st);
        /* restore */
        memcpy(w.start_regions, saved, sizeof(saved));
        w.n_start_regions = saved_n;
        {
            char detail[128];
            snprintf(detail, sizeof(detail),
                     "%d regions from Links House only (Menu adds %d)",
                     empty_lh, empty_menu - empty_lh);
            check("(b) empty-state BFS from Links House only", empty_lh > 100, detail);
        }
    }

    /* ---- (c) full vanilla placement: can beat game ------------------ */
    ganon_loc = Rando_LocationId(&w, "Ganon");
    triforce = Rando_ItemId(&w, "Triforce");
    Rando_StateReset(st);
    t0 = now_ms();
    nreach = Rando_SweepReachability(st, 1, 0);
    t1 = now_ms();
    {
        int crystals = 0, ganon_ok, triforce_ok;
        char detail[256];
        for (i = 0; i < 7; i++) {
            int c = Rando_ItemId(&w, kCrystalNames[i]);
            if (c >= 0 && Rando_StateHas(st, c, 1)) crystals++;
        }
        ganon_ok = Rando_StateLocationReachable(st, ganon_loc);
        triforce_ok = Rando_StateHas(st, triforce, 1);
        snprintf(detail, sizeof(detail),
                 "%d / %d regions, %d rounds, %.2f ms; crystals %d/7, "
                 "Ganon location reachable: %s, has(Triforce): %s",
                 nreach, w.n_regions, st->sweep_rounds, t1 - t0, crystals,
                 ganon_ok ? "yes" : "no", triforce_ok ? "yes" : "no");
        check("(c) can-beat-game with dumped placements", crystals == 7 && ganon_ok && triforce_ok, detail);

        /* bench: average full can-beat sweep over 100 runs */
        {
            int run;
            double t = now_ms();
            for (run = 0; run < 100; run++) {
                Rando_StateReset(st);
                Rando_SweepReachability(st, 1, 0);
            }
            t = now_ms() - t;
            printf("  bench: can-beat sweep x100 (collect on): %.2f ms total, %.3f ms/sweep\n",
                   t, t / 100.0);
            t = now_ms();
            for (run = 0; run < 100; run++) {
                Rando_StateReset(st);
                Rando_SweepReachabilityNoCollect(st);
            }
            t = now_ms() - t;
            printf("  bench: structural BFS x100 (no collect):  %.2f ms total, %.3f ms/sweep\n",
                   t, t / 100.0);
        }
    }

    /* ---- (d) remove Hammer -> hammer-gated regions unreachable ------ */
    hammer = Rando_ItemId(&w, "Hammer");
    if (hammer < 0) {
        check("(d) no-Hammer gating", 0, "no Hammer item in registry");
    } else {
        int all_ok = 1, alt_kept = 1;
        char detail[512];
        size_t k;
        Rando_StateReset(st);
        Rando_SweepReachability(st, 1, hammer);
        for (k = 0; k < sizeof(hammer_regions) / sizeof(hammer_regions[0]); k++) {
            int r = Rando_RegionId(&w, hammer_regions[k]);
            int was = 1; /* all three were reachable in the full sweep (c) */
            int now = Rando_StateCanReachRegion(st, r);
            if (was && now) all_ok = 0;
        }
        for (k = 0; k < sizeof(alt_path_regions) / sizeof(alt_path_regions[0]); k++) {
            int r = Rando_RegionId(&w, alt_path_regions[k]);
            if (!Rando_StateCanReachRegion(st, r)) alt_kept = 0;
        }
        snprintf(detail, sizeof(detail),
                 "Hammer Peg Cave / Bush Yard / Dark C Whirlpool Portal Area now unreachable: %s; "
                 "alt-path region (Darkness Nook Area) still reachable: %s",
                 all_ok ? "yes" : "NO", alt_kept ? "yes" : "no");
        check("(d) no-Hammer gating", all_ok && alt_kept, detail);
    }

    Rando_StateFree(st);
    st = NULL;

    /* ---- (e) FILL: shuffle the pool into the eligible locations ----- */
    {
        int *pl1, *pl2, *snap;
        uint8_t *elig;
        RandoFillStats fs;
        unsigned fill_seed = (unsigned)(w.meta.seed > 0 ? w.meta.seed : 1);
        int i, rc;
        {
            const char *env = getenv("RANDO_FILL_SEED");     /* debug: single seed */
            if (env && strtoul(env, NULL, 10) > 0) fill_seed = strtoul(env, NULL, 10);
        }

        pl1 = (int *)malloc((size_t)w.n_locations * sizeof(int));
        pl2 = (int *)malloc((size_t)w.n_locations * sizeof(int));
        snap = (int *)malloc((size_t)w.n_locations * sizeof(int));
        elig = (uint8_t *)malloc((size_t)w.n_locations);
        if (!pl1 || !pl2 || !snap || !elig) {
            check("(e) fill", 0, "oom");
            free(pl1); free(pl2); free(snap); free(elig);
            Rando_FreeWorld(&w);
            return;
        }
        for (i = 0; i < w.n_locations; i++) snap[i] = w.locations[i].placed_item;

        t0 = now_ms();
        rc = Rando_FillWorld(&w, dir, fill_seed, pl1, elig, &fs, err, sizeof(err));
        t1 = now_ms();
        if (rc != 0) {
            check("(e) fill world", 0, err);
            free(pl1); free(pl2); free(snap); free(elig);
            Rando_FreeWorld(&w);
            return;
        }
        {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "seed %u: %u eligible slots, %u progression + %u junk placed in "
                     "%u attempt(s), %u sweeps, %u candidate tests, %u sphere escapes "
                     "(%.1f ms)",
                     fill_seed, fs.eligible, fs.prog_items, fs.junk_items,
                     fs.attempts, fs.sweeps, fs.candidate_tests, fs.sphere_escapes,
                     t1 - t0);
            check("(e) fill world (frontier restrictive + fast fill)", 1, detail);
        }

        /* (e1) every eligible location filled exactly once; nothing else moved;
         *      the eligible multiset equals the pool multiset */
        {
            int ok = 1, nonelig_moved = -1, multiset_ok = 1;
            int *want = (int *)calloc((size_t)w.n_items, sizeof(int));
            int *got = (int *)calloc((size_t)w.n_items, sizeof(int));
            char detail[256];
            if (!want || !got) {
                ok = 0;
            } else {
                int unfilled = 0;
                for (i = 0; i < w.n_items; i++) want[i] = w.items[i].pool_count;
                for (i = 0; i < w.n_locations; i++) {
                    if (elig[i]) {
                        if (pl1[i] < 0) unfilled++;
                        else got[pl1[i]]++;
                    } else if (pl1[i] != snap[i]) {
                        nonelig_moved = i;
                    }
                }
                if (unfilled) ok = 0;
                for (i = 0; i < w.n_items; i++)
                    if (want[i] != got[i]) multiset_ok = 0;
                if (!multiset_ok) ok = 0;
                if (nonelig_moved >= 0) ok = 0;
            }
            snprintf(detail, sizeof(detail),
                     "%u eligible slots all filled; item multiset == pool multiset: %s; "
                     "non-eligible slots untouched: %s",
                     fs.eligible, multiset_ok ? "yes" : "NO",
                     nonelig_moved < 0 ? "yes" : "NO");
            check("(e1) eligible locations filled exactly once (pool preserved)", ok, detail);
            free(want); free(got);
        }

        /* (e2) all progression pool items placed */
        {
            int ok = 1, missing = 0, prog_total = 0;
            char detail[128];
            for (i = 0; i < w.n_locations; i++)
                if (elig[i] && pl1[i] >= 0 && w.items[pl1[i]].progression)
                    prog_total++;
            /* every progression pool copy must appear exactly once */
            for (i = 0; i < w.n_items; i++) {
                int copies = 0, j;
                if (!w.items[i].progression || w.items[i].pool_count <= 0) continue;
                for (j = 0; j < w.n_locations; j++)
                    if (elig[j] && pl1[j] == i) copies++;
                if (copies != w.items[i].pool_count) missing++;
            }
            ok = (missing == 0);
            snprintf(detail, sizeof(detail),
                     "%d progression placements, all %d progression pool items at eligible slots: %s",
                     prog_total, prog_total, ok ? "yes" : "NO");
            check("(e2) all progression items placed", ok, detail);
        }

        /* (e3) can-beat-game on the final placement (the critical test) */
        {
            int beat = Rando_CanBeat(&w, pl1);
            char path[1024];
            snprintf(path, sizeof(path), "%s\\rando_placement.json", dir);
            Rando_FillWriteJson(&w, pl1, elig, fill_seed, &fs, beat, path, NULL);
            check("(e3) can-beat-game on the filled placement", beat,
                  "Triforce collected + Ganon location reachable after full sweep; "
                  "table written to rando_placement.json");
        }

        /* (e4) determinism: same seed -> identical table; different seed -> different */
        {
            int same_ok, diff_ok;
            char detail[128];
            rc = Rando_FillWorld(&w, dir, fill_seed, pl2, elig, &fs, err, sizeof(err));
            same_ok = (rc == 0) && (memcmp(pl1, pl2, (size_t)w.n_locations * sizeof(int)) == 0);
            rc = Rando_FillWorld(&w, dir, fill_seed + 1, pl2, elig, &fs, err, sizeof(err));
            diff_ok = (rc == 0) && (memcmp(pl1, pl2, (size_t)w.n_locations * sizeof(int)) != 0);
            snprintf(detail, sizeof(detail),
                     "seed %u twice -> identical: %s; seed %u -> different: %s",
                     fill_seed, same_ok ? "yes" : "NO", fill_seed + 1,
                     diff_ok ? "yes" : "NO");
            check("(e4) fill determinism", same_ok && diff_ok, detail);
        }

        /* ---- (f) 10-seed batch: completability rate ------------------- */
        {
            int s, n_batch = 10, completions = 0, total_attempts = 0, total_escapes = 0;
            char detail[256];
            double t_batch;
            {
                const char *env = getenv("RANDO_BATCH_N");   /* larger soak runs */
                if (env && atoi(env) > 0) n_batch = atoi(env);
            }
            t_batch = now_ms();
            for (s = 1; s <= n_batch; s++) {
                RandoFillStats bfs;
                if (n_batch > 10) printf("  batch: filling seed %u...\n", (unsigned)s);
                rc = Rando_FillWorld(&w, dir, (unsigned)s, pl1, elig, &bfs, err, sizeof(err));
                total_attempts += bfs.attempts;
                total_escapes += (int)bfs.sphere_escapes;
                if (rc == 0 && Rando_CanBeat(&w, pl1)) completions++;
                else printf("       batch seed %u FAILED: %s (rc=%d)\n", (unsigned)s,
                            rc != 0 ? err : "can-beat false", rc);
            }
            t_batch = now_ms() - t_batch;
            snprintf(detail, sizeof(detail),
                     "%d/%d completable, %d fill attempts total (retry bound 8), "
                     "%d sphere escapes, %.0f ms for %d fills",
                     completions, n_batch, total_attempts, total_escapes,
                     t_batch, n_batch);
            check("(f) batch completability", completions == n_batch, detail);
        }

        /* ---- (g) phase-B2 demo-chest pin: Secret Passage = Hookshot ---- */
        {
            const char *pin_loc_name = "Secret Passage";
            const char *pin_item_name = "Hookshot";
            int pin_loc = Rando_LocationId(&w, pin_loc_name);
            int pin_item = Rando_ItemId(&w, pin_item_name);
            RandoFillStats pfs;
            char detail[256];
            int rc_pin, unfilled_in_pin = 0;

            if (pin_loc < 0 || pin_item < 0) {
                check("(g) pin fill", 0, "Secret Passage / Hookshot not in dump");
            } else {
                rc_pin = Rando_FillWorldEx(&w, dir, fill_seed, pl1, elig, &pfs,
                                           err, sizeof(err), pin_loc_name,
                                           pin_item_name);
                snprintf(detail, sizeof(detail),
                         "rc=%d pin_applied=%u (%s), eligible %u, %u attempt(s)",
                         rc_pin, pfs.pin_applied, pfs.pin_note, pfs.eligible,
                         pfs.attempts);
                check("(g) pin fill (Rando_FillWorldEx)",
                      rc_pin == 0 && pfs.pin_applied == 1, detail);

                /* (g1) the pinned slot holds the pinned item AND beatable */
                {
                    int beat = Rando_CanBeat(&w, pl1);
                    snprintf(detail, sizeof(detail),
                             "Secret Passage = %s (%s); can-beat-game: %s",
                             pl1[pin_loc] == pin_item ? pin_item_name : "WRONG ITEM",
                             pl1[pin_loc] == pin_item ? "as pinned" : "PIN MISSING",
                             beat ? "true" : "FALSE");
                    check("(g1) pin present + can-beat-game",
                          pl1[pin_loc] == pin_item && beat, detail);
                }

                /* (g2) determinism: same seed -> identical table incl. pin */
                {
                    int same_ok;
                    rc_pin = Rando_FillWorldEx(&w, dir, fill_seed, pl2, elig,
                                               &pfs, err, sizeof(err),
                                               pin_loc_name, pin_item_name);
                    same_ok = (rc_pin == 0) && pfs.pin_applied &&
                              (memcmp(pl1, pl2,
                                      (size_t)w.n_locations * sizeof(int)) == 0);
                    snprintf(detail, sizeof(detail),
                             "seed %u pinned fill twice -> identical table "
                             "(incl. Secret Passage = Hookshot): %s",
                             fill_seed, same_ok ? "yes" : "NO");
                    check("(g2) pin determinism (same seed)", same_ok, detail);
                }

                /* (g3) pin survives a seed change (layout differs, pin stays) */
                {
                    int pin_ok, differs;
                    rc_pin = Rando_FillWorldEx(&w, dir, fill_seed + 1, pl2, elig,
                                               &pfs, err, sizeof(err),
                                               pin_loc_name, pin_item_name);
                    differs = (rc_pin == 0) &&
                              (memcmp(pl1, pl2,
                                      (size_t)w.n_locations * sizeof(int)) != 0);
                    pin_ok = (rc_pin == 0) && pfs.pin_applied &&
                             (pl2[pin_loc] == pin_item);
                    snprintf(detail, sizeof(detail),
                             "seed %u: layout differs from seed %u: %s; "
                             "Secret Passage = Hookshot: %s",
                             fill_seed + 1, fill_seed, differs ? "yes" : "no",
                             pin_ok ? "yes" : "NO");
                    check("(g3) pin present regardless of seed",
                          pin_ok && differs, detail);
                }

                /* (g4) multiset: eligible slots + pinned copy == full pool */
                {
                    int ok = 1, multiset_ok = 1;
                    int *want = (int *)calloc((size_t)w.n_items, sizeof(int));
                    int *got = (int *)calloc((size_t)w.n_items, sizeof(int));
                    if (!want || !got) {
                        ok = 0;
                    } else {
                        for (i = 0; i < w.n_items; i++)
                            want[i] = w.items[i].pool_count;
                        for (i = 0; i < w.n_locations; i++) {
                            if (elig[i] == 2) got[pl1[i]]++;       /* the pin */
                            else if (elig[i]) {
                                if (pl1[i] < 0) unfilled_in_pin++;
                                else got[pl1[i]]++;
                            }
                        }
                        if (unfilled_in_pin) ok = 0;
                        for (i = 0; i < w.n_items; i++)
                            if (want[i] != got[i]) multiset_ok = 0;
                        if (!multiset_ok) ok = 0;
                    }
                    snprintf(detail, sizeof(detail),
                             "shuffled slots + pinned copy == pool multiset: %s; "
                             "every shuffled slot filled: %s",
                             multiset_ok ? "yes" : "NO",
                             unfilled_in_pin ? "NO" : "yes");
                    check("(g4) pin multiset preserved (pool copy consumed)",
                          ok, detail);
                    free(want); free(got);
                }

                /* (g5) start availability: empty-state sweep collects the pin */
                {
                    RandoState *st2;
                    int got_it = 0, triforce_have = 0;
                    Rando_FillApply(&w, pl1);
                    st2 = Rando_StateNew(&w);
                    if (st2) {
                        Rando_SweepReachability(st2, 1, -1);
                        got_it = st2->collected[pin_loc] &&
                                 st2->count[pin_item] > 0;
                        triforce_have = w.victory_item >= 0 &&
                                        st2->count[w.victory_item] > 0;
                    }
                    snprintf(detail, sizeof(detail),
                             "zero-item sweep: Secret Passage collected %s, "
                             "Hookshot in inventory %s, Triforce %s (the pin "
                             "is available from the start of a new game)",
                             st2 && st2->collected[pin_loc] ? "yes" : "NO",
                             st2 && st2->count[pin_item] > 0 ? "yes" : "NO",
                             triforce_have ? "collected" : "NOT");
                    check("(g5) pinned item available from the empty state",
                          st2 && got_it, detail);
                    if (st2) Rando_StateFree(st2);
                }
            }
        }

        free(pl1); free(pl2); free(snap); free(elig);
    }

    /* ---- (h) phase-B apply honesty: chest-byte resolution --------------
     * Mirrors the engine boot path (randomizer.c ApplyFillPlacements):
     * fresh load -> fill with the demo-chest pin -> resolve every CHEST
     * placement to its engine receipt byte via Rando_ApplyChestCode (the
     * engine writes that byte into its chest record).  Asserts the
     * resolution is total (skipped == 0), stays in receipt range (0..75,
     * the engine's 76-entry receipt tables) and hands progressive chains
     * out as concrete tiers in copy-index order. */
    {
        /* expected concrete tiers per chain, copy-index order (verified
         * against misc.c kMemoryLocationToGiveItemTo / kValueToGiveItemTo,
         * tracker.c names and the fork's item_alternates table) */
        static const struct {
            const char *name;
            int n;
            unsigned code[4];
        } kExpectChains[] = {
            { "Progressive Sword", 4, { 0x49, 0x01, 0x02, 0x03 } },
            { "Progressive Glove", 2, { 0x1b, 0x1c } },
            { "Progressive Bow", 2, { 0x0b, 0x3b } },
            { "Progressive Shield", 3, { 0x04, 0x05, 0x06 } },
            { "Progressive Armor", 2, { 0x22, 0x23 } },
            { "Magic Upgrade (1/2)", 1, { 0x42 } },
        };
        /* the fork's Location.address is a PC offset into the US 1.0 ROM;
         * this range is the 168 3-byte chest records (randomizer.c
         * kForkChestBase) */
        enum { TEST_CHEST_BASE = 0xE96E, TEST_CHEST_RECORDS = 168 };

        RandoWorld aw;
        char aerr[512];
        if (Rando_LoadWorld(dir, &aw, aerr, sizeof(aerr)) != 0) {
            check("(h) apply resolution", 0, aerr);
        } else {
            int *apl = (int *)malloc((size_t)aw.n_locations * sizeof(int));
            int *apl2 = (int *)malloc((size_t)aw.n_locations * sizeof(int));
            uint8_t *aelig = (uint8_t *)malloc((size_t)aw.n_locations);
            int *chain_seen = (int *)calloc((size_t)aw.n_items, sizeof(int));
            int *addr = test_parse_addresses(dir, aw.n_locations);
            RandoFillStats afs;
            int nchest = 0, napplied = 0, nskip = 0, noob = 0, chain_bad = 0;
            int occ[6];
            char bad_detail[192];
            double tA, tB;
            bad_detail[0] = '\0';
            memset(occ, 0, sizeof(occ));
            if (!apl || !apl2 || !aelig || !chain_seen || !addr) {
                check("(h) apply resolution (fresh load)", 0, "oom");
            } else {
                unsigned fill_seed = (unsigned)(aw.meta.seed > 0 ? aw.meta.seed : 1);
                {   /* debug override, same contract as block (e) */
                    const char *env = getenv("RANDO_FILL_SEED");
                    if (env && strtoul(env, NULL, 10) > 0)
                        fill_seed = strtoul(env, NULL, 10);
                }
                tA = now_ms();
                if (Rando_FillWorldEx(&aw, dir, fill_seed, apl, aelig, &afs,
                                      aerr, sizeof(aerr),
                                      "Secret Passage", "Hookshot") != 0) {
                    check("(h) apply resolution (fresh load)", 0, aerr);
                } else {
                    int loc, ci;
                    tB = now_ms();
                    for (loc = 0; loc < aw.n_locations; loc++) {
                        const char *cwhy = NULL;
                        int code, a = addr[loc];
                        if (a < TEST_CHEST_BASE ||
                            a >= TEST_CHEST_BASE + 3 * TEST_CHEST_RECORDS ||
                            (a - TEST_CHEST_BASE) % 3 != 0)
                            continue;   /* not an engine chest home */
                        nchest++;
                        code = Rando_ApplyChestCode(&aw, apl[loc], chain_seen,
                                                    &cwhy);
                        if (code < 0) {
                            nskip++;
                            if (!bad_detail[0])
                                snprintf(bad_detail, sizeof(bad_detail),
                                         "%s <- %s: %s",
                                         aw.locations[loc].name,
                                         apl[loc] >= 0 ?
                                             aw.items[apl[loc]].name : "?",
                                         cwhy ? cwhy : "?");
                            continue;
                        }
                        if (code > 75) noob++;
                        napplied++;
                        for (ci = 0;
                             ci < (int)(sizeof(kExpectChains) / sizeof(kExpectChains[0]));
                             ci++) {
                            int k;
                            if (strcmp(aw.items[apl[loc]].name,
                                       kExpectChains[ci].name) != 0)
                                continue;
                            k = occ[ci]++;
                            if (k >= kExpectChains[ci].n ||
                                code != (int)kExpectChains[ci].code[k]) {
                                chain_bad = 1;
                                if (!bad_detail[0])
                                    snprintf(bad_detail, sizeof(bad_detail),
                                             "%s copy %d -> 0x%02x", kExpectChains[ci].name,
                                             k, (unsigned)code);
                            }
                        }
                    }
                    {
                        char detail[320];
                        snprintf(detail, sizeof(detail),
                                 "%d/%d chest placements resolved, %d skipped, "
                                 "codes >75: %d; load+fill+resolve %.1f ms "
                                 "(fill %.1f ms); %d non-engine slots stay "
                                 "vanilla (phase C)",
                                 napplied, nchest, nskip, noob, tB - tA,
                                 afs.fill_ms, aw.n_locations - nchest);
                        check("(h1) every chest placement lands: receipt byte "
                              "0..75, skipped == 0 (apply = model)",
                              nskip == 0 && noob == 0 && napplied == nchest &&
                              napplied > 0, detail);
                    }
                    {
                        char detail[320];
                        snprintf(detail, sizeof(detail),
                                 "sword/glove/bow/shield/mail/magic tiers in "
                                 "copy-index order: %s%s",
                                 chain_bad ? "NO - " : "yes", bad_detail);
                        check("(h2) progressive chains write concrete tiers "
                              "in copy order", !chain_bad, detail);
                    }

                    /* (h3) determinism: same seed twice -> byte-identical
                     * placement JSON (the shape the engine writes as
                     * rando_placement.json; stats omitted because fill_ms
                     * is wall time, not placement data) */
                    {
                        char p1[1024], p2[1024], detail[128];
                        RandoApplyCounts counts;
                        int beat1, beat2, same = 0;
                        counts.chest_locations = nchest;
                        counts.applied = napplied;
                        counts.skipped = nskip;
                        counts.non_chest_locations = aw.n_locations - nchest;
                        beat1 = Rando_CanBeat(&aw, apl);
                        snprintf(p1, sizeof(p1), "%s\\rando_det_a.json", dir);
                        snprintf(p2, sizeof(p2), "%s\\rando_det_b.json", dir);
                        Rando_FillWriteJson(&aw, apl, aelig, fill_seed, NULL,
                                            beat1, p1, &counts);
                        if (Rando_FillWorldEx(&aw, dir, fill_seed, apl2, aelig,
                                              &afs, aerr, sizeof(aerr),
                                              "Secret Passage",
                                              "Hookshot") == 0) {
                            size_t l1 = 0, l2 = 0;
                            char *t1, *t2;
                            beat2 = Rando_CanBeat(&aw, apl2);
                            Rando_FillWriteJson(&aw, apl2, aelig, fill_seed,
                                                NULL, beat2, p2, &counts);
                            t1 = test_read_all(p1, &l1);
                            t2 = test_read_all(p2, &l2);
                            same = t1 && t2 && l1 == l2 &&
                                   memcmp(t1, t2, l1) == 0 && beat1 == beat2;
                            free(t1); free(t2);
                        }
                        remove(p1); remove(p2);
                        snprintf(detail, sizeof(detail),
                                 "seed %u filled twice -> placement JSON "
                                 "byte-identical: %s (can_beat_game %d==%d)",
                                 fill_seed, same ? "yes" : "NO", beat1,
                                 same ? beat1 : -1);
                        check("(h3) placement JSON determinism "
                              "(byte-identical)", same, detail);
                    }
                }
            }
            free(apl); free(apl2); free(aelig); free(chain_seen); free(addr);
            Rando_FreeWorld(&aw);
        }
    }

    /* ---- (i) corrupt dump: out-of-range location region fails cleanly ---
     * rando_state.c indexes reachable[] by location->region; a corrupt
     * dump must fail the LOAD (rando_load.c range check), never crash a
     * sweep. */
    {
        static const char *const kFiles[6] = {
            "regions.json", "edges.json", "locations.json",
            "items.json", "rules.json", "meta.json",
        };
        char cdir[1024], src[1024], dst[1024], detail[256];
        RandoWorld cw;
        char cerr[512];
        int i, rc, made = 1;
#ifdef _WIN32
        _mkdir("temp_rando_corrupt");
#else
        mkdir("temp_rando_corrupt", 0777);
#endif
        snprintf(cdir, sizeof(cdir), "temp_rando_corrupt");
        for (i = 0; i < 6; i++) {
            snprintf(src, sizeof(src), "%s\\%s", dir, kFiles[i]);
            snprintf(dst, sizeof(dst), "%s\\%s", cdir, kFiles[i]);
            if (test_copy_file(src, dst) != 0) { made = 0; break; }
        }
        if (!made) {
            snprintf(detail, sizeof(detail), "copy failed (from %s)", dir);
            check("(i) corrupt-dump region rejected", 0, detail);
        } else {
            /* blow up the first location's region id (rebuild the number
             * as 99999 - a single-digit overwrite would stay in range) */
            snprintf(dst, sizeof(dst), "%s\\locations.json", cdir);
            {
                char *text = test_read_all(dst, NULL);
                char *p;
                int mutated = 0;
                if (text) {
                    p = strstr(text, "\"region\": ");
                    if (p) {
                        p += 10;
                        if (*p >= '0' && *p <= '9') {
                            char *q = p;
                            while (*q >= '0' && *q <= '9') q++;
                            {
                                size_t head = (size_t)(p - text);
                                size_t tail = strlen(q);
                                char *out =
                                    (char *)malloc(head + 5 + tail + 1);
                                if (out) {
                                    FILE *f;
                                    memcpy(out, text, head);
                                    memcpy(out + head, "99999", 5);
                                    memcpy(out + head + 5, q, tail + 1);
                                    f = fopen(dst, "wb");
                                    fwrite(out, 1, head + 5 + tail, f);
                                    fclose(f);
                                    free(out);
                                    mutated = 1;
                                }
                            }
                        }
                    }
                    free(text);
                }
                if (!mutated) made = 0;
            }
            if (!made) {
                check("(i) corrupt-dump region rejected", 0, "mutation failed");
            } else {
                rc = Rando_LoadWorld(cdir, &cw, cerr, sizeof(cerr));
                snprintf(detail, sizeof(detail),
                         "region 99999... load rc=%d (%s), err: %s", rc,
                         rc != 0 ? "clean failure" : "LOADED?!", cerr);
                check("(i) corrupt-dump region rejected",
                      rc != 0 && strstr(cerr, "region") != NULL, detail);
            }
        }
        for (i = 0; i < 6; i++) {
            snprintf(dst, sizeof(dst), "%s\\%s", cdir, kFiles[i]);
            remove(dst);
        }
#ifdef _WIN32
        _rmdir(cdir);
#else
        rmdir(cdir);
#endif
    }

    Rando_StateFree(st);
    Rando_FreeWorld(&w);
}

int main(int argc, char **argv)
{
    int i;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("rando_test — phase-1 logic engine scaffold (zelda3 native randomizer)\n");
    if (argc > 1) {
        for (i = 1; i < argc; i++)
            test_seed(argv[i]);
    } else {
        test_seed("randomizer_ref\\out\\seed_1234");
        test_seed("randomizer_ref\\out\\seed_5678");
    }
    printf("==================================================================\n");
    printf("SUMMARY: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif /* RANDO_TEST */

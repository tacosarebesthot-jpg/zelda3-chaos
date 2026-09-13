// One-off diagnostic: for fill seeds [lo, hi], report which item the fill
// puts at a given dump location (default: 'Sanctuary' - the single chest in
// the chapter-1 reference save's spawn room).  Used to pick a photogenic
// seed for the phase-B gameplay gold test.
//
// Build (repo root):
//   cl /nologo /std:c11 /W0 /O2 /I. tools\rando_pick_seed.c src\rando\*.c ^
//      /Fe:tools\rando_pick_seed.exe /Fo:tools\rando_test_build\
// Usage:
//   tools\rando_pick_seed.exe [location] [lo] [hi] [dump_dir]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/rando/rando_load.h"
#include "src/rando/rando_fill.h"

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *want = argc > 1 ? argv[1] : "Sanctuary";
  unsigned lo = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
  unsigned hi = argc > 3 ? (unsigned)atoi(argv[3]) : 60;
  const char *dir = argc > 4 ? argv[4] : "randomizer_ref/out/seed_1234";

  RandoWorld w;
  char err[256];
  if (Rando_LoadWorld(dir, &w, err, sizeof(err)) != 0) {
    printf("load failed: %s\n", err);
    return 1;
  }
  int loc = Rando_LocationId(&w, want);
  if (loc < 0) {
    printf("location not found: %s\n", want);
    return 1;
  }
  int *pl = malloc((size_t)w.n_locations * sizeof(int));
  for (unsigned seed = lo; seed <= hi; seed++) {
    if (Rando_FillWorld(&w, dir, seed, pl, NULL, NULL, err, sizeof(err)) != 0) {
      printf("seed %u: fill failed (%s)\n", seed, err);
      continue;
    }
    int item = pl[loc];
    printf("seed %4u: %-12s <- %s\n", seed, want,
           item >= 0 ? w.items[item].name : "(none)");
  }
  free(pl);
  Rando_FreeWorld(&w);
  return 0;
}

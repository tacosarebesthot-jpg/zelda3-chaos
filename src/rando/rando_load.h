/* rando_load.h — loads the six phase-0 JSON dumps into a RandoWorld. */
#ifndef RANDO_LOAD_H
#define RANDO_LOAD_H

#include <stddef.h>

#include "rando_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load regions.json / edges.json / locations.json / items.json /
 * rules.json / meta.json from directory `dir` into `w`.
 * Returns 0 on success; on failure returns -1 and fills `err`. */
int  Rando_LoadWorld(const char *dir, RandoWorld *w, char *err, size_t errsz);
void Rando_FreeWorld(RandoWorld *w);

#ifdef __cplusplus
}
#endif

#endif /* RANDO_LOAD_H */

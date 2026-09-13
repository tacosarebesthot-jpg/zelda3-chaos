#ifndef ENEMIZER_H
#define ENEMIZER_H

#include "types.h"

// Per-save-file enemy stat randomizer, driven by the two RANDOMIZER page
// settings the file was made with: ENEMY HEALTH ("enemyhp": NORMAL / EASY /
// SHUFFLED / HARD / EXPERT) and ENEMY DAMAGE ("enemydmg": NORMAL / SHUFFLED /
// RANDOM).  Everything is derived from the file's seed, so the same seed
// always yields the same stats and nothing extra has to be stored in the save.
//
// The vanilla const tables in sprite.c (kSpriteInit_Health,
// kSpriteInit_BumpDamage) are never modified - this module keeps a runtime
// copy of each and the sprite spawn path (SpritePrep_LoadProperties) reads the
// copies.  A vanilla / non-randomized file leaves the copies byte-identical to
// the originals, so the engine behaves exactly as before.

#define kEnemizerTypes 243

// (Re)build the runtime tables from the loaded file's settings + seed.  Cheap
// and idempotent: safe to call whenever the loaded file may have changed
// (Randomizer_ApplyFromSave does, on every file load).
void Enemizer_Apply(void);

// Spawn-time values for a sprite type; identical to the vanilla tables unless
// a randomized file asked for something else.
uint8 Enemizer_Health(int type);
uint8 Enemizer_BumpDamage(int type);

// ENEMIES ("enemyshuffle"): the sprite type a 3-byte sprite record should
// spawn.  |rec| must point at the record inside the kDungeonSprites /
// kOverworldSprites asset itself; anything else (a boss shuffle override, a
// hand-built record) returns rec[2] untouched, and so does a file that does
// not shuffle enemies.  Positions are never moved, only the type byte.
uint8 Enemizer_SwapUW(const uint8 *rec);
uint8 Enemizer_SwapOW(const uint8 *rec);

// ENEMY COLOR ("enemypalette"): the sprite palette rows the loader should
// upload.  |src| must point inside kPalette_MainSpr / kPalette_SpriteAux1;
// the same pointer comes back for a vanilla or non-randomized file.  Link's,
// the HUD's and the message palettes live elsewhere and are never remapped.
const uint16 *Enemizer_PalMainSpr(const uint16 *src);
const uint16 *Enemizer_PalAux1Spr(const uint16 *src);

#endif  // ENEMIZER_H

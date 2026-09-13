#ifndef ZELDA3_SPRITES_H_
#define ZELDA3_SPRITES_H_

// Config-driven Link sprite selection (v1: boot apply + in-game cycling +
// one SETTINGS menu row).
//
// Reads `sprites.ini` in the working directory:
//   name=Meatwad            ; resolved against sprites/library.json
//   file=path/to/sprite.zspr ; optional direct path (overrides name=)
//
// Missing sprites.ini = feature off, zero cost (the call returns before
// touching anything). Applies the chosen ALTTPR .zspr by replacing the
// engine's Link graphics buffer (kLinkGraphics) and, when the zspr carries
// a palette block, the first four armor palettes (kPalette_ArmorAndGloves)
// plus the gloves color (kGlovesColor). Must be called after LoadAssets()
// and after LoadLinkGraphics(). Every failure mode is non-fatal: it logs to
// stderr and leaves vanilla Link in place.
//
// When sprites.ini exists, this also loads the generated catalog
// sprites/sprite_library.ini (tools/fetch_sprites.py --all) for in-game
// cycling via Sprite_Select_Cycle().
void Sprite_Select_Init(void);

// Cycle through the sprite library: dir=+1 next sprite, -1 previous. Bound
// to F11 (next) / Shift+F11 (previous) from main.c. Applies the candidate
// zspr with the same swap as boot - live (NMI re-DMA's kLinkGraphics every
// frame; the armor palette rows are pushed via Palette_Load_LinkArmorAndGloves)
// - and persists the pick by rewriting sprites.ini (name=<picked>) so it
// survives restart. Fail-open: a missing/corrupt zspr is skipped (stderr
// note), a catalog-less install just logs; the currently applied sprite is
// never replaced by a half-applied one.
void Sprite_Select_Cycle(int dir);

// Name of the currently applied catalog sprite, uppercased, at most 28
// chars; "VANILLA" when nothing from the catalog is applied. Shared by the
// settings-screen row and the pause-menu MODS page (hud.c). Points at a
// static buffer: copy it if you need it past the next call.
const char *Sprite_Select_CurrentName(void);

#endif

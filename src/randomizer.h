#ifndef RANDOMIZER_H
#define RANDOMIZER_H

#include "types.h"

// Native in-engine randomizer, phase 1 (scaffolding).
// Reads randomizer.ini { enabled, seed }, and when enabled shuffles the
// dungeon chest-item table (kDungeonRoomChests asset) in memory after assets
// load. Later phases port the full ALttPDoorRandomizer fill/logic algorithm.

// Call once after LoadAssets() in main().
void Randomizer_Init(void);

// Per-frame hook, called from ZeldaRunFrame() (zelda_rtl.c).  Cheap no-op
// unless F12 was just pressed; F12 toggles the SHARED in-game settings
// screen (SettingsMenu_Toggle, settings_menu.h - this module registers its
// rows there lazily).  F12 because F1-F10 are
// the zelda3.ini save/load/replay slots (F9/F8 included) and F11 is the
// sprite-library cycle key.
void Randomizer_SettingsTick(void);

// ---- Settings-screen entries (the shared menu's rows bind to these) ----
// Pending values: what Randomizer_SettingsCommit() writes to
// randomizer.ini.  The fill runs once at boot, so nothing here re-fills
// the running game - commit rewrites the ini atomically (every other line
// preserved byte-for-byte) and everything applies NEXT BOOT.
int Randomizer_SettingsEnabled(void);            // 0/1
int Randomizer_SettingsSeed(void);               // 1..999999999, 0 = time-derived
void Randomizer_SettingsToggleEnabled(void);
void Randomizer_SettingsSeedAdjust(int delta);   // clamped to 1..999999999
// Logic tier (randomizer.ini logic=): index into the five reference tiers
// noglitches, minorglitches, owglitches, hybridglitches, nologic.  Each has
// its own rule-set folder randomizer_ref/out/logic_<key>/ loaded at boot.
#define kLogicCount 5
int Randomizer_SettingsLogic(void);                // 0..kLogicCount-1
const char *Randomizer_SettingsLogicLabel(void);   // "NO GLITCHES" etc (menus)
const char *Randomizer_SettingsLogicShort(void);   // <=8 chars (file select)
const char *Randomizer_LogicKey(int idx);          // ini key for an index
void Randomizer_SettingsLogicCycle(int delta);     // +1 next / -1 previous, wraps
// Atomically rewrite randomizer.ini (temp file + MoveFileEx replace).
// Returns 1 on success, 0 on failure (file not writable).
int Randomizer_SettingsCommit(void);

// Per-file randomizer settings (stream build). Each save slot's enabled/seed/
// logic live in saves/rando_slots.ini next to the SRAM; the slot's byte 0x4E0
// is 1 when they apply. Set once when the file is made, fixed after that.
void Randomizer_ApplyFromSave(void);           // after CopySaveToWRAM: (re)fill for the loaded file
void Randomizer_NewFileStamp(int slot);        // pending settings -> the slot's entry (seed 0 rolls one)
void Randomizer_SlotErased(int slot);
void Randomizer_SlotCopied(int dst, int src);
int Randomizer_SlotInfo(int slot, int *enabled, int *seed, int *logic);  // 0 = no entry
int Randomizer_FileLocked(void);               // loaded file has fixed settings
int Randomizer_FileEnabled(void);
int Randomizer_FileSeed(void);
int Randomizer_FileLogic(void);
// START (Lane 09-12 "I use open world"): 0 OPEN = Link's house, Zelda already
// safe, castle gate open (what the logic dumps assume); 1 STANDARD = the
// vanilla intro (its own logic dumps, logic_<tier>_standard).
int Randomizer_SettingsStart(void);
void Randomizer_SettingsStartCycle(void);
const char *Randomizer_SettingsStartLabel(void);   // "OPEN" / "STANDARD"
int Randomizer_FileStart(void);
// Extra setting of the loaded file by ini key ("keys", "bigkeys", "maps",
// "compasses", "enemyhp", "enemydmg", "goal"...): the value index (0 = the
// reference default) or the reference's value string.
int Randomizer_FileOptValue(const char *inikey);
// Extra receipt codes of the fill (0x80 = half magic, 0x81..0x85 = the bomb
// and arrow capacity upgrades, 0x86 = silver arrows, 0x87 = triforce piece):
// decode in Link_ReceiveItem, effect in misc.c/ancilla.c.
int Randomizer_ExtraItemBegin(int item);
int Randomizer_TakeHalfMagic(void);
void Randomizer_HalfMagicDone(void);
// A pending capacity upgrade (extra codes 0x81..0x85) raises link_bomb_upgrades
// / link_arrow_upgrades here, once, from AncillaAdd_ItemReceipt.
void Randomizer_TakeCapacityUpgrade(void);
// SILVER ARROWS (extra code 0x86) and the plain Bow receipt (0x0b): keeps
// link_item_bow / link_has_silvers consistent when the reference's `silvers`
// bow mode splits the bow and its silver arrows into two pool items.  Called
// once per receipt from AncillaAdd_ItemReceipt with the receipt id.
void Randomizer_TakeBowUpgrade(int j);
// TRIFORCE HUNT (extra code 0x87).  A piece is in flight from
// Link_ReceiveItem until the receipt animation ends: Pending() is the peek
// AncillaAdd_ItemReceipt uses to swap the sprite, Take() is the single
// consumption point (count the piece, check the goal) in ancilla.c.
int Randomizer_PendingTriforcePiece(void);
int Randomizer_TakeTriforcePiece(void);
// How many pieces this file needs; 0 when the GOAL row is not TRIFORCE.
int Randomizer_TriforceGoal(void);
// The win a finished triforce hunt asked for, deferred to a safe frame:
// called from Module_MainRouting, enters the triforce room when one is due.
void Randomizer_CheckPendingWin(void);

// ---- BOMB BAG (reference `bombbag`) ------------------------------------
// 1 while the loaded file plays the bomb-bag rule: bombs cannot be carried
// at all until the first bomb capacity upgrade ("the bomb bag") is found.
extern uint8 g_rando_bomb_bag;
// Link's bomb capacity: kMaxBombsForLevel[link_bomb_upgrades] as in vanilla,
// 0 while a bomb-bag file has not found the bag yet.  Every engine site that
// asks "is the bomb count full" goes through this.
int Randomizer_MaxBombs(void);

// ---- RETRO (reference `retro`) -----------------------------------------
// 1 while the loaded file plays retro: arrows are bought, never dropped.
extern uint8 g_rando_retro;

// ---- HEART COLOR (reference `heartcolor`) -------------------------------
// Resolved once per file load (RANDOM included): 0 red (vanilla, no-op),
// 1 blue, 2 green, 3 yellow.  Applied by Randomizer_ApplyHeartColor, called
// from Palette_Load_HUD (load_gfx.c) every time the HUD palette reloads.
extern uint8 g_rando_heart_color;
void Randomizer_ApplyHeartColor(void);

// ---- HEART BEEP (reference `heartbeep`) ---------------------------------
// Frames between low-health beeps for the loaded file (32 = vanilla), 0 =
// row set to OFF (never beeps).  hud.c's low-health beep reload reads this.
int Randomizer_HeartBeepInterval(void);

// ---- MENU SPEED (reference `fastmenu`) ----------------------------------
// How many 8-unit BG3VOFS steps hud.c's item-menu slide should take in one
// frame (1 = vanilla), or -1 for HALF (hud.c takes one step every other
// frame instead).  See the comment above the definition for why the step
// size itself never changes.
int Randomizer_MenuSpeedSteps(void);

// ---- MUSIC (reference `disablemusic`) -----------------------------------
// 1 while the loaded file has MUSIC set to OFF. nmi.c's audio funnel reads
// this to substitute the pause control byte for every real track request.
int Randomizer_MusicDisabled(void);

// ---- FLASHING (reference `reduce_flashing`) -----------------------------
// 1 while the loaded file has FLASHING set to REDUCED. zelda_rtl.c ORs the
// engine's existing DimFlashes feature bit in when this is set.
int Randomizer_FlashingReduced(void);

// ---- DUNGEON COUNT / COLLECT RATE --------------------------------------
// Unopened seed chests left in the dungeon Link is standing in, or -1 when
// the HUD should draw nothing (row OFF, not in a dungeon, PICKUP without the
// compass, or no fill data).
int Randomizer_DungeonItemsLeft(void);
// "collected/total" over the seed's chest placements; 0 = the row is off.
int Randomizer_CollectionRate(int *got, int *total);
const char *Randomizer_FileOptRefValue(const char *inikey);
// The RANDOMIZER page rows (select_file.c draws them): RANDOMIZER, SEED, LOGIC,
// START, every extra logic setting, BEGIN. Extra settings go to the reference
// randomizer as --setting key=value; a rule folder per combination is built on
// demand by python_embed (Randomizer_PrepareRules) when a new file begins.
enum { kRPKind_Enum, kRPKind_Seed, kRPKind_Begin };
int Randomizer_PageRows(void);
int Randomizer_PageRow(int i, const char **label, char *val, size_t cap, const char **d1, const char **d2);
void Randomizer_PageRowCycle(int i, int delta);
int Randomizer_RulesReady(void);       // the pending settings' rule folder exists (or a build was tried)
void Randomizer_PrepareRules(void);    // build it now (blocks a few seconds)
// New file: after the vanilla SRAM init, the pending START's bytes.
void Randomizer_NewFileInitSram(unsigned char *sram);
// What the fill put in engine chest record |rec| (kDungeonRoomChests index / 3):
// item name and whether it is progression. 0 = unknown (vanilla / not filled).
int Randomizer_ChestRecordInfo(int rec, char *item, size_t cap, int *progression);
// NPC / pedestal / tablet gifts: the engine item code the fill placed at the
// reference randomizer location |location|, or |vanilla| when the file is
// not randomized or the spot holds something the engine cannot hand out.
int Randomizer_GiftItem(const char *location, int vanilla);
// The reference randomizer's name for the dungeon Link is in ("Eastern Palace"), or "".
const char *Randomizer_CurrentDungeonName(void);
// Sprite spots that have no chest record (randomizer.c holds the tables and
// the reasoning).  Each returns the reference randomizer's name for the spot,
// to feed straight into Randomizer_GiftItem, or NULL when the engine cannot
// name it (then the sprite keeps its vanilla item).
//   HeartPiece  |indoors| = player_is_indoors; outdoors it is keyed on the
//               overworld area, indoors on the room plus (sprite_x_hi & 1).
//   ShopSlot    |slot| 0/1/2 = Left/Middle/Right; NULL unless the loaded file
//               has SHOPS = SHUFFLED, so vanilla shops are untouched.  The
//               name is built in one shared static buffer - use it before
//               the next call.
const char *Randomizer_HeartPieceLocation(int indoors, int area, int room, int xbit);
const char *Randomizer_GenerousGuyLocation(int room);
const char *Randomizer_ShopSlot(int room, int area, int slot);
// KEY DROPS = SHUFFLED: the reference's fourteen "... Key Drop" locations -
// the enemies that drop a dungeon's small key, plus Hyrule Castle's big key.
// A drop is named by the underworld room plus the sprite's LOAD SLOT
// (Dungeon_LoadSingleSprite's k, alive in sprite_N[k]); randomizer.c holds
// the table and the reasoning.  Both calls are no-ops unless the loaded file
// shuffles drops, so vanilla files keep the vanilla key drop exactly.
//   Spawned  SpriteDeath, on the "drops a key" branch: remembers what the
//            fill put on this enemy for as long as the dropped key lives.
//            |slot| is sprite_N[k], |type| the enemy's sprite id (checked
//            against the dump before anything is substituted).
//   Receipt  Sprite_HandleAbsorptionByPlayer case 12 / 13: the receipt code
//            to hand Link instead of the vanilla key, or -1 for "vanilla".
//            |slot| is sprite_subtype[k], where SpriteDeath parked the load
//            slot; |vanilla| is the code the drop would have given anyway.
void Randomizer_KeyDropSpawned(int k, int room, int slot, int type);
int Randomizer_KeyDropReceipt(int k, int room, int slot, int vanilla);
// POTS = SHUFFLED: the nineteen "... Pot Key" locations.  Spawned from
// sprite.c Sprite_SpawnSecret (item 8 -> sprite 0xe4), collected in
// Sprite_HandleAbsorptionByPlayer case 12.  |j| is the key's sprite slot,
// |room| is dungeon_room_index.
void Randomizer_PotKeySpawned(int j, int room);
int Randomizer_PotKeyReceipt(int j, int room, int vanilla);
// BONK DROPS = SHUFFLED: the reference's 42 "bonk prize" locations - the
// prizes hidden in trees, bonk rocks and statues that a boots dash (or the
// Quake spell) shakes loose.  Each one is an ordinary overworld sprite of a
// prize type sitting in kOverworldSprites, hidden until Entity_ApplyRumbleToSprites
// clears its sprite_E; randomizer.c holds the (area, block) table and the
// reasoning.  All four calls are no-ops unless the loaded file shuffles them.
//   Reset    Overworld_LoadSprites / Dungeon_LoadSprites: a new room or area
//            is being populated, so every armed sprite slot is stale.
//   Spawned  Overworld_LoadProximaSpriteIfAlive (outdoors) and
//            Dungeon_LoadSingleSprite (the one indoor slot): arms sprite slot
//            |k| when this really is a shuffled bonk prize.  Returns 1 when
//            the caller must replace the sprite with the green-rupee
//            placeholder 0xd9, which is the only prize shape the engine can
//            reliably hand an arbitrary item out of.
//   Receipt  Sprite_HandleAbsorptionByPlayer: the receipt code to hand Link
//            instead of the vanilla pickup, or -1 for "do exactly vanilla".
void Randomizer_BonkDropsReset(void);
int Randomizer_BonkDropSpawned(int k, int area, int blk, int type);
int Randomizer_BonkDropSpawnedUW(int k, int room, int slot, int type);
int Randomizer_BonkDropReceipt(int k);

// Keysanity (SMALL KEYS / BIG KEYS / MAPS / COMPASSES on ANYWHERE): the fill
// puts dungeon-targeted receipt codes 0x4C..0x7F in chest records and in the
// gift table, one per (class, dungeon).  The layout and the reasoning live in
// src/rando/rando_fill.c; the engine handles them in two steps.
// Link_ReceiveItem calls Begin on every item: a dungeon-targeted code becomes
// the PLAIN vanilla receipt it returns (0x24 small key / 0x32 big key /
// 0x33 map / 0x25 compass, so the pickup looks and sounds vanilla) and the
// target dungeon is remembered; anything else is returned unchanged.
int Randomizer_DungeonItemBegin(int item);
// AncillaAdd_ItemReceipt calls Grant for those four receipts: 1 = the pickup
// was dungeon-targeted and has been credited to the dungeon the fill named,
// 0 = do the vanilla "dungeon Link is standing in" write.
int Randomizer_DungeonItemGrant(int j);

// SMALL KEYS = UNIVERSAL (the reference's keyshuffle=universal): one shared
// small-key pool instead of a counter per dungeon.  1 while the LOADED FILE
// asks for it - decided once per file load in Randomizer_ApplyFromSave, never
// re-derived from the settings, because the engine reads it from hot paths
// (the HUD rebuild, the door-opening check).  0 keeps every key path
// byte-for-byte vanilla.
extern uint8 g_rando_universal_keys;

// Universal keys: keep link_num_keys (0xF36F) as the ONE live key counter and
// keep the saved pool (link_num_keys_universal, 0xF38B) equal to it.  Called
// at exactly the sites where vanilla would swap the live counter for a
// per-dungeon one - dungeon entry, dungeon exit / save, death, overworld and
// indoor HUD rebuilds, key doors and SaveGameFile.
void Randomizer_UniversalKeysHold(void);
const char *Randomizer_LogicShortFor(int idx);   // 8-char tier code for any tier
const char *Randomizer_LogicLabelFor(int idx);   // menu label for any tier
void Randomizer_SettingsSeedToggleRandom(void);   // RANDOM <-> last number (page, A on SEED)
int Randomizer_SettingsSeedAdjustFrom(int delta);

// ---- BOSSES (boss shuffle) ----
// The loaded file's bosses.json is applied as per-room overrides of two
// read-only assets; these are the readers' escape hatches.  All are cheap
// no-ops (linear scan of at most 14 entries) when boss shuffle is off.
// Runtime replacement for the room's kDungeonSprites list (sort byte, 3-byte
// (y, x, type) records, 0xff terminator), or NULL to use the asset.
const uint8 *Randomizer_BossRoomSprites(int room);
// The room's 14-byte header with the placed boss' sprite sheet in byte 3, or
// |vanilla| unchanged.
const uint8 *Randomizer_BossRoomHeader(int room, const uint8 *vanilla);
// Blind was placed in this room by boss shuffle: start the fight without the
// Thieves Town maiden.
int Randomizer_BossBlindNoMaiden(int room);
// Arrghus was placed outside Swamp Palace: let him move over dry floor.
int Randomizer_BossArrghusOnLand(void);
// A Trinexx stands outside Turtle Rock: his ice breath must not rewrite the
// room tilemap with frozen-floor tiles.
int Randomizer_BossTrinexxLoose(void);

#endif  // RANDOMIZER_H

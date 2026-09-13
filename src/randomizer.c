// Native randomizer for zelda3 - phase B: the fill drives the chest table.
//
// With randomizer.ini enabled=1 the boot flow is now
//
//   LoadWorld (phase-0 JSON dumps in randomizer_ref/out/seed_1234/)
//     -> FillWorld (phase-A frontier fill, deterministic from `seed=`)
//       -> apply the placement table to the ENGINE's real item home:
//          the dungeon chest table (kDungeonRoomChests asset), so opened
//          chests hand out the shuffled items.  Same boot timing as the
//          old phase-1 shuffle: after LoadAssets(), before ZeldaInitialize().
//
// The placement -> engine-record mapping (see tools/RANDO_FILL.md and the
// tables below): the fork's Location.address is a PC offset into the US 1.0
// ROM; addresses in [0xE96E, 0xEB66) are item bytes of the 168 3-byte chest
// records that kDungeonRoomChests also holds (the asset was extracted from
// that very table, so record CONTENT matches 1:1, only the scan ORDER
// differs - kForkChestRoomWords embeds the ROM order so each address can be
// resolved to (room, big-chest, k-th chest of the room) and then to the
// kDungeonRoomChests record).  ALTTPR item names map to engine chest bytes
// via Rando_ApplyChestCode (rando_fill.c): dungeon keys/maps/compasses use
// the vanilla native codes, and Progressive chain items are remapped to
// their k-th CONCRETE engine receipt by pool copy index (k-th Progressive
// Sword copy -> sword tier k), so every pool item lands in a chest record
// and the fill's can-beat proof carries over to the engine world.  Names
// with no engine receipt (>= 76) outside the chains - e.g. Magic Upgrade
// (1/4), which no vanilla receipt grants - keep the record's vanilla item
// and are logged.  Non-chest locations (pedestals, sprite drops, shops,
// pots, events) have no engine chest home and stay vanilla (phase C): the
// boot banner reports applied/skipped/non-chest counts so "can-beat
// verified" is only claimed unqualified when every chest placement landed.
//
// Fallback: if the dumps are missing or the fill fails, the old phase-1
// class-preserving chest shuffle runs instead (vanilla-compatible).
//
// Config (randomizer.ini, same directory as the executable):
//   enabled=1   0 = vanilla (default)
//   seed=12345  0 = derive from system time
//   log=1       write randomizer_log.txt with the full placement mapping
//   dir=...     optional world-dump directory (default
//               randomizer_ref/out/seed_1234)
//   pin_location=Secret Passage   demo-chest pin: one location forced to
//   pin_item=Hookshot             one item BEFORE the fill shuffles the rest
//               (empty value = pin off).  The pinned slot is excluded from
//               the shuffle and consumes the item's pool copy, so the world
//               stays can-beat validated.  Defaults honor the pinned demo
//               chest on every seed.
//
// In-game settings (no ini editing by hand): F12 (or F10) opens the SHARED
// in-game settings screen (settings_menu.h - SettingsMenu_Toggle), whose
// RANDOMIZER / SEED rows this module registers lazily (rows exist even when
// the randomizer is disabled, so the menu can enable it) and drives through
// its entry API (Randomizer_SettingsEnabled/Seed/ToggleEnabled/SeedAdjust/
// Commit in randomizer.h).  The fill runs at
// boot, so edits can NOT re-fill the running game - Commit rewrites
// randomizer.ini atomically (temp file + MoveFileEx replace; every other
// line, e.g. pin_location / pin_item / log / dir, is preserved
// byte-for-byte) and every change banner says "(applies next boot)".  Leaving
// the menu commits via the SettingsMenu_RegisterOnClose hook (->
// Randomizer_SettingsCommit).  The menu is modal: main.c feeds every key to
// SettingsMenu_Input while it is open and the engine sees none.
// The F12 hook works even when the randomizer is disabled (one
// GetAsyncKeyState(F12) poll per frame).  Key choice: F1-F10 are the
// zelda3.ini save/load/replay slots (F9/F8 included), F11 is the
// sprite-library cycle key, F12 is unbound.
//
// Every seed also gets a deterministic silly nickname ("The Legend of Soggy
// Cucco"), shown in the console banner and as the first line of the log. It
// draws from its own RNG stream so placements stay byte-identical.

// rando_fill.c's timing helper wants <windows.h>; include it before anything
// else so the later one in the unity TU is a no-op (windows.h must precede
// the engine's generated headers, not follow them).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "randomizer.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "settings_menu.h"  // SettingsMenu_Toggle: F12 opens the shared settings screen
#include "twitch.h"         // Twitch_TestMode: the F12 overlay is harness-only
#include "assets.h"
#include "dialogue_override.h"   // hints: progression placements
#include "enemizer.h"            // enemy health / damage from this file's settings
#include "hud.h"                 // Hud_RefreshIcon after a capacity upgrade
#include "sprite.h"              // kPrizeItems: RETRO rewrites the enemy prize packs

// The rando scaffold is engine-independent (no SDL, no g_ram, no zelda3
// headers) but lives in src/rando/, which build_msvc.cmd's non-recursive
// src\*.c glob does not pick up.  Pull it in as a unity translation unit
// instead of touching the build script.  rando_test.c stays out.
#include "rando/rando_json.c"
#include "rando/rando_rules.c"
#include "rando/rando_state.c"
#include "rando/rando_load.c"
#include "rando/rando_fill.c"
#include "rando/rando_inverted.c"
#include "rando/rando_entrance.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32 g_rnd_state;
static uint32 RndNext(void) {
  // xorshift32
  uint32 x = g_rnd_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rnd_state = x;
  return x;
}

// ---- Logic tiers (the streamer's ask: player-selectable glitch logic) ----
// The reference randomizer implements these; randomizer_ref/dump_logic.py
// --logic <key> emits one rule-set folder per tier, kept at
// randomizer_ref/out/logic_<key>/.  The ini key `logic=` picks the folder
// the fill loads at boot (an explicit `dir=` still overrides).
static const char *const kLogicKeys[kLogicCount] = {
  "noglitches", "minorglitches", "owglitches", "hybridglitches", "nologic",
};
static const char *const kLogicLabels[kLogicCount] = {   // menu text, caps
  "NO GLITCHES", "MINOR GLITCHES", "OW GLITCHES", "HYBRID GLITCHES", "NO LOGIC",
};
static const char *const kLogicShort[kLogicCount] = {    // file select, 8 max
  "NOGLITCH", "MINORGL", "OWGLITCH", "HYBRIDGL", "NOLOGIC",
};

// start modes (randomizer.ini start=, rando_slots.ini slotN.start=).  This is
// the reference's `--mode`, so it is the WORLD row: there is no `mode` row in
// kRandoOpts and there must not be one, or RulesBuild would put --mode on the
// command line twice.  The reference's third value, `inverted`, is NOT offered
// - see src/rando/rando_inverted.c for the survey and for the exact piece that
// blocks it.  Adding it here without that piece would hand the fill an
// inverted logic dump (933 regions but 260 new edges, 256 gone and 1579 rules
// changed) to prove a seed winnable in a world the engine is not running.
enum { kStartOpen = 0, kStartStandard = 1 };
static const char *const kStartKeys[2] = { "open", "standard" };
static const char *const kStartLabels[2] = { "OPEN", "STANDARD" };
static int s_ini_start;                // start= as read by LoadRandomizerConfig

// ---- extra logic settings (owner 09-12: every option of the reference
// randomizer changeable in the game). Each row's value is what the reference
// accepts for --setting key=value; index 0 is always the reference default,
// so a file with every row at 0 uses the plain logic_<tier> folders and any
// other combination gets its own rule folder, built on demand by
// python_embed\python.exe randomizer_ref\dump_logic.py (about six seconds).
#define kRandoOptCount 38
typedef struct RandoOpt {
  const char *inikey, *refkey, *label;
  const char *const *vals, *const *names;
  int n;
  const char *d1, *d2;
  // 1 = the reference takes this setting as a BARE FLAG, not key=value: the
  // dump tool declares e.g. shopsanity with argparse action="store_true", so
  // "--setting shopsanity=shuffled" dies with "ignored explicit argument".
  // RulesBuild emits "--setting <refkey>" when the value index is 1 and
  // nothing when it is 0.  Rows that leave this 0 keep the key=value form.
  uint8 boolflag;
  // 1 = the reference only CHANGES THE ROM with this setting, never the logic
  // dump: DUNGEON COUNT and COLLECT RATE are pure HUD presentation (verified
  // by dumping with and without them - identical region/edge/location/item
  // and rule counts).  Such a row is left out of the rule-folder signature
  // and out of the dump command line, so flipping it costs no six-second
  // rebuild and does not orphan a folder somebody already has.
  uint8 presentation;
} RandoOpt;
static const char *const kV_goal[] = {"ganon", "pedestal", "dungeons", "crystals", "triforcehunt"};
static const char *const kN_goal[] = {"GANON", "PEDESTAL", "DUNGEONS", "CRYSTALS", "TRIFORCE"};
static const char *const kV_cry[] = {"7", "6", "5", "4", "3", "2", "1", "0", "random"};
static const char *const kN_cry[] = {"SEVEN", "SIX", "FIVE", "FOUR", "THREE", "TWO", "ONE", "ZERO", "RANDOM"};
static const char *const kV_nhe[] = {"normal", "hard", "expert"};
static const char *const kN_nhe[] = {"NORMAL", "HARD", "EXPERT"};
static const char *const kV_acc[] = {"items", "locations", "none"};
static const char *const kN_acc[] = {"ITEMS", "EVERYTHING", "NONE"};
static const char *const kV_sw[] = {"random", "assured", "vanilla"};
static const char *const kN_sw[] = {"RANDOM", "ASSURED", "VANILLA"};
static const char *const kV_pyr[] = {"auto", "yes", "no"};
static const char *const kN_pyr[] = {"AUTO", "OPEN", "CLOSED"};
static const char *const kV_prog[] = {"on", "off", "random"};
static const char *const kN_prog[] = {"ON", "OFF", "RANDOM"};
static const char *const kV_rbi[] = {"none", "mapcompass", "dungeon"};
static const char *const kN_rbi[] = {"ANYTHING", "NO MAP CMP", "NO DUNGEON"};
static const char *const kV_fl[] = {"normal", "active"};
static const char *const kN_fl[] = {"NORMAL", "ACTIVE"};
static const char *const kV_bow[] = {"progressive", "silvers"};
static const char *const kN_bow[] = {"PROGRESSIVE", "SILVERS"};
// SMALL KEYS / BIG KEYS / MAPS / COMPASSES.  `nearby` is the reference's
// middle setting between OWN DUNGEON and ANYWHERE: the item still never joins
// the shuffled pool (ItemList.py keeps it out for both `none` and `nearby`),
// but fill_dungeons_restrictive may put it anywhere in the DISTRICT its own
// dungeon sits in - Desert Ledge for a Desert Palace key, Ether Tablet for a
// Tower of Hera big key.  It therefore arrives as a FIXED placement out of
// locations.json exactly as an OWN DUNGEON one does, and the only thing the
// engine has to do differently is hand it the dungeon-TARGETED receipt code
// (0x4C..0x7F) instead of the plain "credit the dungeon Link is standing in"
// one - which is exactly what ANYWHERE already needs.  APPENDED, never
// inserted: the rule-folder signature is one letter per row (RulesDirFor), so
// a value's INDEX is baked into every folder anybody already has.
static const char *const kV_keys[] = {"none", "wild", "universal", "nearby"};
static const char *const kN_keys[] = {"OWN DUNGEON", "ANYWHERE", "UNIVERSAL", "NEARBY"};
static const char *const kV_wild[] = {"none", "wild", "nearby"};
static const char *const kN_wild[] = {"OWN DUNGEON", "ANYWHERE", "NEARBY"};
static const char *const kV_eh[] = {"default", "easy", "normal", "hard", "expert"};
static const char *const kN_eh[] = {"NORMAL", "EASY", "SHUFFLED", "HARD", "EXPERT"};
static const char *const kV_ed[] = {"default", "shuffled", "random"};
static const char *const kN_ed[] = {"NORMAL", "SHUFFLED", "RANDOM"};
// BOSSES.  `unique` is only a different PLACEMENT ALGORITHM in Bosses.py (no
// boss is used twice outside Ganons Tower); it writes the same bosses.json -
// thirteen {dungeon, spot, boss} rows drawn from the same ten boss names and
// the same thirteen spots - that simple/full/random already write, and
// ApplyBossShuffle reads it by name with no per-mode knowledge at all.
static const char *const kV_boss[] = {"none", "simple", "full", "random", "unique"};
static const char *const kN_boss[] = {"NORMAL", "SIMPLE", "FULL", "RANDOM", "UNIQUE"};
// shopsanity is a boolean flag in the reference (see RandoOpt.boolflag); these
// value strings only ever reach randomizer.ini / rando_slots.ini.
static const char *const kV_shop[] = {"normal", "shuffled"};
static const char *const kN_shop[] = {"NORMAL", "SHUFFLED"};
// KEY DROPS.  The reference has THREE names for this corner: dropshuffle
// (the 14 enemy "... Key Drop" slots), pottery (the 19 "... Pot Key" slots)
// and keydropshuffle, which is not a setting of its own at all - CLI.py turns
// it into dropshuffle=keys AND pottery=keys.  Each half has its own row here
// (KEY DROPS -> dropshuffle, POTS -> pottery) so either can be turned on
// alone; `keydropshuffle` itself is never sent.
static const char *const kV_drop[] = {"none", "keys"};
static const char *const kN_drop[] = {"NORMAL", "SHUFFLED"};
// POTS.  The reference's pottery is a key=value setting (choices none / keys /
// dungeon / cave / cavekeys / reduced / clustered / nonempty / lottery), not a
// bare flag.  Only `keys` is offered: it turns exactly the nineteen "... Pot
// Key" locations into slots and leaves every other pot vanilla, and those
// nineteen are the only pots this engine can hand an item out of (each is a
// key secret record in kDungeonSecrets; see the POT KEYS block further down).
// The wider modes rearrange hundreds of ordinary pots, most of which have no
// secret record at all, and a pot the engine cannot deliver makes a seed
// unwinnable - it is all-or-nothing per mode.
static const char *const kV_pots[] = {"none", "keys"};
static const char *const kN_pots[] = {"NORMAL", "SHUFFLED"};
// BONK DROPS.  The reference's bonk_drops is a BARE FLAG (see RandoOpt.boolflag
// and the BONK DROPS block further down), so these value strings only ever
// reach randomizer.ini / rando_slots.ini.  All 42 rows of Regions.py
// bonk_prize_table or none: a bonk slot the engine cannot deliver could hold
// a progression item, so the load-time cross-check is all-or-nothing.
static const char *const kV_bonk[] = {"off", "on"};
static const char *const kN_bonk[] = {"NORMAL", "SHUFFLED"};
// bombbag / retro / collection_rate are boolean flags too, so kV_onoff only
// ever reaches the ini files.  dungeon_counters is a real key=value setting -
// but a presentation-only one, so it never reaches the dump either.
static const char *const kV_onoff[] = {"off", "on"};
static const char *const kN_onoff[] = {"OFF", "ON"};
static const char *const kV_dcnt[] = {"default", "on", "off", "pickup"};
static const char *const kN_dcnt[] = {"NORMAL", "ON", "OFF", "PICKUP"};
// HEART COLOR / MENU SPEED / HEART BEEP (Adjuster.py's heartcolor / fastmenu /
// heartbeep): cosmetics, presentation-only - see the RandoOpt comment. Value
// strings mirror the reference's own choices so they read straight off
// Adjuster.py even though these three never reach --setting.
static const char *const kV_heartcolor[] = {"red", "blue", "green", "yellow", "random"};
static const char *const kN_heartcolor[] = {"RED", "BLUE", "GREEN", "YELLOW", "RANDOM"};
static const char *const kV_menuspeed[] = {"normal", "instant", "double", "triple", "quadruple", "half"};
static const char *const kN_menuspeed[] = {"NORMAL", "INSTANT", "DOUBLE", "TRIPLE", "QUADRUPLE", "HALF"};
// `double` (Rom.py 0x180033 = 0x10, i.e. half the vanilla 0x20 interval) is
// appended rather than slotted in front of `half` where the reference lists
// it, for the same index-stability reason as the keysanity rows - though HEART
// BEEP is presentation-only, so it never reaches the folder signature at all.
static const char *const kV_heartbeep[] = {"normal", "half", "quarter", "off", "double"};
static const char *const kN_heartbeep[] = {"NORMAL", "HALF", "QUARTER", "OFF", "DOUBLE"};
// ENEMIES / ENEMY COLOR: engine-side, presentation-only rows - see the big
// comment at the top of the ENEMIES block in src/enemizer.c.  The reference's
// own `shuffleenemies` (choices none / shuffled) DOES change the logic dump,
// so it is deliberately NOT passed through: the engine cannot reproduce the
// reference's placement, and shipping its loosened shutter-door rules would
// make seeds unwinnable.  The value strings mirror the reference's choices
// anyway, so the row reads straight off args.json; they only ever reach
// randomizer.ini / rando_slots.ini.  ENEMY COLOR has no reference setting at
// all (ow_palettes / uw_palettes are BACKGROUND palettes, not enemies).
static const char *const kV_enem[] = {"none", "shuffled"};
static const char *const kN_enem[] = {"NORMAL", "SHUFFLED"};
static const char *const kV_ecol[] = {"default", "shuffled", "random"};
static const char *const kN_ecol[] = {"NORMAL", "SHUFFLED", "RANDOM"};
// OW COLORS / UW COLORS (reference `ow_palettes` / `uw_palettes`): presentation
// -only, engine-side, in the same style as ENEMY COLOR above - see the big
// comment above OwMainSrc/UwMainSrc in load_gfx.c. Value strings mirror the
// reference's own choice list (Adjuster.py/args.json: default/random/blackout
// - there is no "grayscale"/"negative" in the actual reference, only these
// three), so they only ever reach randomizer.ini / rando_slots.ini.
static const char *const kV_bgpal[] = {"default", "random", "blackout"};
static const char *const kN_bgpal[] = {"NORMAL", "RANDOM", "BLACKOUT"};
// SFX SHUFFLE (reference `shuffle_sfx`): presentation-only - see the big
// comment above AudioSfx_Remap1/2 in audio.c. A bare flag in the reference
// (Adjuster.py store_true), but never reaches --setting anyway (presentation
// rows never do), so the ON/OFF value strings only ever reach the ini files.
static const char *const kV_sfxshuf[] = {"off", "on"};
static const char *const kN_sfxshuf[] = {"OFF", "ON"};
// MUSIC (reference `disablemusic`): presentation-only - see the gate in
// nmi.c's Interrupt_NMI_AudioParts_Locked. ON (index 0) is the reference
// default (music plays) so it is listed first, matching the HEART BEEP /
// MENU SPEED convention of index 0 == today's behaviour.
static const char *const kV_music[] = {"on", "off"};
static const char *const kN_music[] = {"ON", "OFF"};
// FLASHING (reference `reduce_flashing`): presentation-only - see
// Randomizer_FlashingReduced and the resync it feeds in zelda_rtl.c. It only
// ever ADDS the engine's existing global DimFlashes feature bit on top of
// whatever zelda3.ini already says, so NORMAL (index 0, the reference
// default) never touches anything.
static const char *const kV_flash[] = {"normal", "reduced"};
static const char *const kN_flash[] = {"NORMAL", "REDUCED"};
// ENTRANCES (the reference's `shuffle`).  Its full choice list is vanilla,
// simple, restricted, full, lite, lean, district, swapped, crossed, insanity,
// dungeonsfull, dungeonssimple; a mode that is in this list is one the engine
// wires end to end.  Only `lite` and `district` are left out, and only because
// they do not GENERATE: both die in Fill.py with "No more spots to place Small
// Key (Turtle Rock)" at the dump's own seed 1234.
// `lean` and `swapped` were held back by the entrance round for want of an
// eyeball and are in now on the paper the loader itself checks:
//   swapped  is EntranceShuffle2.py's `crossed` mode_def with undefined='swap'
//            instead of 'shuffle' - same pools, same keep_drops_together, same
//            cross_world, not decoupled - and dumps the same shape as the
//            shipped crossed: 128 doors, 12 holes, 60 exit rows, 0 unhandled.
//   lean     is a partial shuffle: 87 of the 128 doors move, the rest stay
//            vanilla and are simply not listed.  RandoEntrance_Load handles a
//            subset already (it only requires the LISTED slots' vanilla ids to
//            be a permutation of their targets), and they are: checked against
//            EntranceShuffle2.entrance_map/drop_map/single_entrance_map plus
//            exit_ids, the same check passes for lean, swapped, crossed and
//            full alike.  Both dump at 100% rule flattening.
// See src/rando/rando_entrance.c.  APPENDED for index stability (RulesDirFor).
static const char *const kV_ent[] = {"vanilla", "simple", "restricted", "full",
                                     "crossed", "insanity", "dungeonssimple", "dungeonsfull",
                                     "lean", "swapped"};
static const char *const kN_ent[] = {"NORMAL", "SIMPLE", "RESTRICT", "FULL",
                                     "CROSSED", "INSANITY", "DUNG SIMP", "DUNG FULL",
                                     "LEAN", "SWAPPED"};
static const RandoOpt kRandoOpts[kRandoOptCount] = {
  { "goal", "goal", "GOAL", kV_goal, kN_goal, 5, "WHAT ENDS THE GAME", "GANON IS THE USUAL" },
  { "crystals_gt", "crystals_gt", "GT CRYSTALS", kV_cry, kN_cry, 9, "CRYSTALS TO ENTER", "GANONS TOWER" },
  { "crystals_ganon", "crystals_ganon", "GANON CRYST", kV_cry, kN_cry, 9, "CRYSTALS TO HURT", "GANON" },
  { "pool", "difficulty", "ITEM POOL", kV_nhe, kN_nhe, 3, "HARD AND EXPERT TAKE", "UPGRADES OUT OF POOL" },
  { "functionality", "item_functionality", "ITEM POWER", kV_nhe, kN_nhe, 3, "HARD AND EXPERT MAKE", "ITEMS WEAKER" },
  { "access", "accessibility", "ACCESS", kV_acc, kN_acc, 3, "ITEMS  ALL REACHABLE", "NONE  ONLY THE WIN" },
  { "swords", "swords", "SWORDS", kV_sw, kN_sw, 3, "ASSURED  START WITH IT", "VANILLA  USUAL SPOTS" },
  { "pyramid", "openpyramid", "PYRAMID HOLE", kV_pyr, kN_pyr, 3, "OPEN  THE HOLE STARTS", "AUTO FOLLOWS THE GOAL" },
  { "progressive", "progressive", "PROGRESSIVE", kV_prog, kN_prog, 3, "UPGRADES IN ORDER OR", "ANY ORDER" },
  { "bossitems", "restrict_boss_items", "BOSS ITEMS", kV_rbi, kN_rbi, 3, "WHAT A BOSS MAY DROP", "IN ITS OWN DUNGEON" },
  { "flute", "flute_mode", "FLUTE", kV_fl, kN_fl, 2, "ACTIVE  THE FLUTE", "WORKS RIGHT AWAY" },
  { "bow", "bow_mode", "BOW", kV_bow, kN_bow, 2, "SILVERS  ONE SILVER", "BOW INSTEAD OF TWO" },
  { "keys", "keyshuffle", "SMALL KEYS", kV_keys, kN_keys, 4, "ANYWHERE  ANY DUNGEON", "NEARBY  NEAR ITS OWN" },
  { "bigkeys", "bigkeyshuffle", "BIG KEYS", kV_wild, kN_wild, 3, "BIG KEYS ANYWHERE OR", "NEAR THEIR DUNGEON" },
  { "maps", "mapshuffle", "MAPS", kV_wild, kN_wild, 3, "MAPS ANYWHERE OR", "NEAR THEIR DUNGEON" },
  { "compasses", "compassshuffle", "COMPASSES", kV_wild, kN_wild, 3, "COMPASSES ANYWHERE OR", "NEAR THEIR DUNGEON" },
  { "enemyhp", "enemy_health", "ENEMY HEALTH", kV_eh, kN_eh, 5, "ENEMY HIT POINTS", "PER SEED" },
  { "enemydmg", "enemy_damage", "ENEMY DAMAGE", kV_ed, kN_ed, 3, "SHUFFLED OR RANDOM", "ENEMY DAMAGE PER SEED" },
  { "shopsanity", "shopsanity", "SHOPS", kV_shop, kN_shop, 2, "SHOP STOCK JOINS", "THE ITEM SHUFFLE", 1 },
  { "keydrops", "dropshuffle", "KEY DROPS", kV_drop, kN_drop, 2, "THE 14 KEYS ENEMIES", "DROP JOIN THE SHUFFLE" },
  { "pots", "pottery", "POTS", kV_pots, kN_pots, 2, "THE 19 KEYS UNDER", "POTS JOIN THE SHUFFLE" },
  { "bombbag", "bombbag", "BOMB BAG", kV_onoff, kN_onoff, 2, "NO BOMBS AT ALL", "UNTIL THE BAG DROPS", 1 },
  { "retro", "retro", "RETRO", kV_onoff, kN_onoff, 2, "NO ARROW DROPS  BUY", "THEM  KEYS GO SHARED", 1 },
  { "counters", "dungeon_counters", "DUNG COUNT", kV_dcnt, kN_dcnt, 4, "CHESTS LEFT IN THIS", "DUNGEON ON THE HUD", 0, 1 },
  { "collectrate", "collection_rate", "COLLECT RATE", kV_onoff, kN_onoff, 2, "ITEMS FOUND OF THE", "SEED ON THE MODS PAGE", 1, 1 },
  { "heartcolor", "heartcolor", "HEART COLOR", kV_heartcolor, kN_heartcolor, 5, "COLOR OF THE HEART", "METER ON THE HUD", 0, 1 },
  { "menuspeed", "fastmenu", "MENU SPEED", kV_menuspeed, kN_menuspeed, 6, "HOW FAST THE ITEM", "MENU SLIDES OPEN", 0, 1 },
  { "heartbeep", "heartbeep", "HEART BEEP", kV_heartbeep, kN_heartbeep, 5, "LOW HEALTH BEEP", "RATE  OFF IS SILENT", 0, 1 },
  { "enemyshuffle", "shuffleenemies", "ENEMIES", kV_enem, kN_enem, 2, "ENEMIES SWAP PLACES", "INSIDE THEIR OWN CLASS", 0, 1 },
  { "enemypalette", "enemy_palette", "ENEMY COLOR", kV_ecol, kN_ecol, 3, "ENEMY PALETTES SWAP", "OR GO RANDOM PER SEED", 0, 1 },
  // OW COLORS / UW COLORS / SFX SHUFFLE / MUSIC / FLASHING: the remaining
  // presentation-only rows of the bundled reference (owner 09-12) - none of
  // these change the logic dump, so they never reach the dump command line
  // or the rule-folder signature.  See the hook comments in load_gfx.c
  // (OwMainSrc/OwAux12Src/OwAux3Src/UwMainSrc), audio.c (AudioSfx_Remap1/2)
  // and zelda_rtl.c (the enhanced_features0 resync) for exactly what each one
  // touches.
  { "ow_palettes", "ow_palettes", "OW COLORS", kV_bgpal, kN_bgpal, 3, "OVERWORLD BG COLORS", "RANDOM OR BLACKED OUT", 0, 1 },
  { "uw_palettes", "uw_palettes", "UW COLORS", kV_bgpal, kN_bgpal, 3, "DUNGEON BG COLORS", "RANDOM OR BLACKED OUT", 0, 1 },
  { "shuffle_sfx", "shuffle_sfx", "SFX SHUFFLE", kV_sfxshuf, kN_sfxshuf, 2, "SOUND EFFECT IDS SWAP", "NEVER CROSSES CHANNELS", 0, 1 },
  { "disablemusic", "disablemusic", "MUSIC", kV_music, kN_music, 2, "OFF MUTES THE SONGS", "SOUND EFFECTS STAY ON", 0, 1 },
  { "reduce_flashing", "reduce_flashing", "FLASHING", kV_flash, kN_flash, 2, "REDUCED DIMS SPELL AND", "SCREEN FLASH EFFECTS", 0, 1 },
  { "bonkdrops", "bonk_drops", "BONK DROPS", kV_bonk, kN_bonk, 2, "TREE AND BONK PRIZES", "JOIN THE ITEM SHUFFLE", 1 },
  // ENTRANCES rewires the overworld doors, so it changes the dump (edges.json
  // as well as the new entrances.json): a real logic row, never presentation.
  { "entrances", "shuffle", "ENTRANCES", kV_ent, kN_ent, 10, "WHICH DOOR LEADS WHERE", "CAVES AND DUNGEONS MIX" },
  // Keep BOSSES LAST: kBossOptIndex below is this row's index in the table.
  { "bosses", "shufflebosses", "BOSSES", kV_boss, kN_boss, 5, "WHO GUARDS EACH", "DUNGEON  THE SEED" },
};
// Boss placement is rolled by the reference generator from the seed, so a
// non-NORMAL BOSSES value makes the rule folder (which now also carries
// bosses.json) seed-specific - see RulesDirFor.
#define kBossOptIndex (kRandoOptCount - 1)
// ENTRANCES sits directly in front of BOSSES; like the boss placement, the
// door wiring is rolled from the generator's RNG, so a non-NORMAL value makes
// the rule folder seed-specific (see OptsSeedSpecific).
#define kEntranceOptIndex (kRandoOptCount - 2)
static int s_ini_opt[kRandoOptCount];
// the extra settings the CURRENT fill ran with (boot's, or the loaded
// file's): the apply loop needs the keysanity rows, and they arrive as an
// array, not through the save slots
static int s_fill_opt[kRandoOptCount];
static int FillOptValue(const char *inikey) {
  for (int o = 0; o < kRandoOptCount; o++)
    if (!strcmp(kRandoOpts[o].inikey, inikey)) return s_fill_opt[o];
  return 0;
}
static int OptIndexFromValue(int o, const char *v) {
  for (int i = 0; i < kRandoOpts[o].n; i++)
    if (!strcmp(v, kRandoOpts[o].vals[i])) return i;
  return 0;
}

static int LogicIndexFromKey(const char *s) {
  for (int i = 0; i < kLogicCount; i++)
    if (!strcmp(s, kLogicKeys[i]))
      return i;
  return -1;
}

static void LoadRandomizerConfig(int *enabled, int *seed, int *log_enabled,
                                 int *logic,
                                 char *dir, size_t dirsz,
                                 char *pin_location, size_t pinsz,
                                 char *pin_item, size_t pinisz) {
  *enabled = 0;
  *seed = 0;
  *log_enabled = 1;
  *logic = 0;
  dir[0] = 0;                          // resolved from the tier after parsing
  snprintf(pin_location, pinsz, "Secret Passage");
  snprintf(pin_item, pinisz, "Hookshot");
  FILE *f = fopen("randomizer.ini", "r");
  if (!f) {
    snprintf(dir, dirsz, "randomizer_ref/out/logic_%s", kLogicKeys[0]);
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    // Exact key match.  Was an unchecked strncmp prefix per key, so
    // "seedfoo=7" set the seed, "loglevel=0" disabled logging and
    // "direction=x" overwrote the dump dir.  Case-sensitive like the rest
    // of the parser; surrounding whitespace on the key is tolerated.
    char *key = line, *val = eq + 1;
    while (*key == ' ' || *key == '\t')
      key++;
    char *keyend = key + strlen(key);
    while (keyend > key &&
           (keyend[-1] == ' ' || keyend[-1] == '\t'))
      *--keyend = 0;
    if (!strcmp(key, "enabled")) *enabled = atoi(val);
    else if (!strcmp(key, "seed")) *seed = atoi(val);
    else if (!strcmp(key, "log")) *log_enabled = atoi(val);
    else if (!strcmp(key, "dir")) {
      char *nl = strpbrk(val, "\r\n");
      if (nl) *nl = 0;
      snprintf(dir, dirsz, "%s", val);
    }
    else if (!strcmp(key, "pin_location")) {
      char *nl = strpbrk(val, "\r\n");
      if (nl) *nl = 0;
      snprintf(pin_location, pinsz, "%s", val);
    }
    else if (!strcmp(key, "pin_item")) {
      char *nl = strpbrk(val, "\r\n");
      if (nl) *nl = 0;
      snprintf(pin_item, pinisz, "%s", val);
    }
    else if (!strcmp(key, "start")) {
      while (*val == ' ' || *val == '\t') val++;
      s_ini_start = !strncmp(val, "standard", 8) ? kStartStandard : kStartOpen;
    }
    else if (strcmp(key, "logic") != 0) {   // every other key: one of the extra settings?
      for (int o = 0; o < kRandoOptCount; o++) {
        if (strcmp(key, kRandoOpts[o].inikey)) continue;
        char *nl = strpbrk(val, "\r\n");
        if (nl) *nl = 0;
        while (*val == ' ' || *val == '\t') val++;
        s_ini_opt[o] = OptIndexFromValue(o, val);
      }
    }
    else if (!strcmp(key, "logic")) {
      char *nl = strpbrk(val, "\r\n");
      if (nl) *nl = 0;
      while (*val == ' ' || *val == '\t') val++;
      int idx = LogicIndexFromKey(val);
      if (idx < 0)
        printf("[randomizer] unknown logic=%s in randomizer.ini, using %s\n",
               val, kLogicKeys[0]);
      else
        *logic = idx;
    }
  }
  fclose(f);
  if (!dir[0])                         // no explicit dir=: the tier's folder
    snprintf(dir, dirsz, "randomizer_ref/out/logic_%s", kLogicKeys[*logic]);
}

// -----------------------------------------------------------------------
// In-game settings (F12 -> the shared settings screen)
// -----------------------------------------------------------------------
// The fill runs once at boot, so this never re-fills the running game:
// the entry functions move the pending enabled/seed values and
// Randomizer_SettingsCommit() rewrites randomizer.ini atomically.  See
// the file header and randomizer.h.

static int s_cfg_enabled, s_cfg_seed;  // pending values (ini state at boot)
static int s_cfg_logic;                // pending logic tier index
static int s_cfg_start, s_boot_start;   // START: open / standard
static int s_cfg_opt[kRandoOptCount], s_boot_opt[kRandoOptCount];   // the extra settings, pending / boot
static int s_cfg_loaded;               // Randomizer_Init has stashed them
static int s_boot_logic;               // tier THIS boot filled with
static int s_boot_seed_used;           // the seed the boot fill really used (ini 0 -> clock)
static int s_boot_enabled, s_boot_seed;  // what THIS boot actually used; the
                                         // rows compare pending vs boot to
                                         // show "NEXT BOOT: ..." hints while
                                         // an uncommitted edit differs

// Does this raw ini line (still holding its newline) set `key`?
// Matches the main parser: exact key, surrounding key whitespace tolerated.
static int SettingsLineIsKey(const char *line, const char *key) {
  while (*line == ' ' || *line == '\t')
    line++;
  size_t n = strlen(key);
  if (strncmp(line, key, n) != 0)
    return 0;
  line += n;
  while (*line == ' ' || *line == '\t')
    line++;
  return *line == '=';
}

// Newline style of the raw line being replaced (CRLF stays CRLF).
static const char *SettingsLineEol(const char *line) {
  size_t n = strlen(line);
  return (n >= 2 && line[n - 2] == '\r') ? "\r\n" : "\n";
}

// Rewrite randomizer.ini with enabled=/seed= replaced and every other line
// (pin_location/pin_item/log/dir, comments, ordering) byte-for-byte kept.
// Atomic: write a temp file next to the ini, then MoveFileEx-replace it
// over the original (same volume -> atomic swap).
static int SettingsSaveIni(void) {
  FILE *in = fopen("randomizer.ini", "r");
  FILE *out = fopen("randomizer.ini.tmp", "w");
  if (!out) {
    if (in)
      fclose(in);
    return 0;
  }
  int wrote_enabled = 0, wrote_seed = 0, wrote_logic = 0, wrote_start = 0;
  int wrote_opt[kRandoOptCount] = { 0 };
  if (in) {
    char line[512];
    // (an ini line longer than 511 chars would be chunked by fgets and only
    // its first chunk rewritten; real lines here are tiny)
    while (fgets(line, sizeof(line), in)) {
      if (!wrote_enabled && SettingsLineIsKey(line, "enabled")) {
        fprintf(out, "enabled=%d%s", s_cfg_enabled, SettingsLineEol(line));
        wrote_enabled = 1;
      } else if (!wrote_seed && SettingsLineIsKey(line, "seed")) {
        fprintf(out, "seed=%d%s", s_cfg_seed, SettingsLineEol(line));
        wrote_seed = 1;
      } else if (!wrote_logic && SettingsLineIsKey(line, "logic")) {
        fprintf(out, "logic=%s%s", kLogicKeys[s_cfg_logic], SettingsLineEol(line));
        wrote_logic = 1;
      } else if (!wrote_start && SettingsLineIsKey(line, "start")) {
        fprintf(out, "start=%s%s", kStartKeys[s_cfg_start], SettingsLineEol(line));
        wrote_start = 1;
      } else {
        int hit = -1;
        for (int o = 0; o < kRandoOptCount; o++)
          if (!wrote_opt[o] && SettingsLineIsKey(line, kRandoOpts[o].inikey)) { hit = o; break; }
        if (hit >= 0) {
          fprintf(out, "%s=%s%s", kRandoOpts[hit].inikey, kRandoOpts[hit].vals[s_cfg_opt[hit]], SettingsLineEol(line));
          wrote_opt[hit] = 1;
        } else {
          fputs(line, out);
        }
      }
    }
    fclose(in);
  }
  // ini missing a key?  add it rather than silently dropping the edit
  if (!wrote_enabled)
    fprintf(out, "enabled=%d\n", s_cfg_enabled);
  if (!wrote_seed)
    fprintf(out, "seed=%d\n", s_cfg_seed);
  if (!wrote_logic)
    fprintf(out, "logic=%s\n", kLogicKeys[s_cfg_logic]);
  if (!wrote_start)
    fprintf(out, "start=%s\n", kStartKeys[s_cfg_start]);
  for (int o = 0; o < kRandoOptCount; o++)
    if (!wrote_opt[o])
      fprintf(out, "%s=%s\n", kRandoOpts[o].inikey, kRandoOpts[o].vals[s_cfg_opt[o]]);
  fclose(out);
#if defined(_WIN32)
  if (!MoveFileExA("randomizer.ini.tmp", "randomizer.ini",
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    remove("randomizer.ini.tmp");
    return 0;
  }
#else
  // portable fallback (tiny non-atomic window between the two calls)
  remove("randomizer.ini");
  if (rename("randomizer.ini.tmp", "randomizer.ini") != 0)
    return 0;
#endif
  return 1;
}

static void SettingsBanner(const char *suffix) {
  printf("[randomizer] settings: enabled=%d seed=%d logic=%s start=%s%s\n",
         s_cfg_enabled, s_cfg_seed, kLogicKeys[s_cfg_logic], kStartKeys[s_cfg_start], suffix);
}

// Getters/actions must work even if the menu somehow runs before
// Randomizer_Init: read the ini so the editor starts from disk truth.
static void SettingsEnsureLoaded(void) {
  if (s_cfg_loaded)
    return;
  {
    int log_enabled;
    char dir[256], pin_loc[64], pin_item[64];
    LoadRandomizerConfig(&s_cfg_enabled, &s_cfg_seed, &log_enabled,
                         &s_cfg_logic,
                         dir, sizeof(dir), pin_loc, sizeof(pin_loc),
                         pin_item, sizeof(pin_item));
    s_boot_enabled = s_cfg_enabled;      // first contact = disk truth = what
    s_boot_seed = s_cfg_seed;            // the current boot started from
    s_boot_logic = s_cfg_logic;
    s_cfg_start = s_ini_start;
    s_boot_start = s_cfg_start;
    memcpy(s_cfg_opt, s_ini_opt, sizeof s_cfg_opt);
    memcpy(s_boot_opt, s_ini_opt, sizeof s_boot_opt);
    s_cfg_loaded = 1;
  }
}

int Randomizer_SettingsStart(void) { SettingsEnsureLoaded(); return s_cfg_start; }
void Randomizer_SettingsStartCycle(void) { SettingsEnsureLoaded(); s_cfg_start ^= 1; }
const char *Randomizer_SettingsStartLabel(void) { SettingsEnsureLoaded(); return kStartLabels[s_cfg_start & 1]; }

int Randomizer_SettingsLogic(void) {
  SettingsEnsureLoaded();
  return s_cfg_logic;
}

const char *Randomizer_SettingsLogicLabel(void) {
  SettingsEnsureLoaded();
  return kLogicLabels[s_cfg_logic];
}

const char *Randomizer_LogicLabelFor(int idx) {
  return kLogicLabels[(idx >= 0 && idx < kLogicCount) ? idx : 0];
}
const char *Randomizer_LogicShortFor(int idx) {
  return kLogicShort[(idx >= 0 && idx < kLogicCount) ? idx : 0];
}
const char *Randomizer_SettingsLogicShort(void) {
  SettingsEnsureLoaded();
  return kLogicShort[s_cfg_logic];
}

const char *Randomizer_LogicKey(int idx) {
  return (idx >= 0 && idx < kLogicCount) ? kLogicKeys[idx] : kLogicKeys[0];
}

void Randomizer_SettingsLogicCycle(int delta) {
  SettingsEnsureLoaded();
  if (delta == 0)
    delta = 1;
  s_cfg_logic = (s_cfg_logic + (delta > 0 ? 1 : kLogicCount - 1)) % kLogicCount;
  SettingsBanner(" (applies next boot)");
}

int Randomizer_SettingsEnabled(void) {
  SettingsEnsureLoaded();
  return s_cfg_enabled;
}

int Randomizer_SettingsSeed(void) {
  SettingsEnsureLoaded();
  return s_cfg_seed;
}

void Randomizer_SettingsToggleEnabled(void) {
  SettingsEnsureLoaded();
  s_cfg_enabled = !s_cfg_enabled;
  SettingsBanner(" (applies next boot)");
}

void Randomizer_SettingsSeedAdjust(int delta) {
  SettingsEnsureLoaded();
  // 1..999999999, always an explicit seed (0 = derive from time)
  int s = s_cfg_seed + delta;
  if (s < 1)
    s = 1;
  if (s > 999999999)
    s = 999999999;
  if (s != s_cfg_seed) {
    s_cfg_seed = s;
    SettingsBanner(" (applies next boot)");
  }
}

int Randomizer_SettingsCommit(void) {
  SettingsEnsureLoaded();
  if (SettingsSaveIni()) {
    SettingsBanner(" saved to randomizer.ini (applies next boot)");
    return 1;
  }
  printf("[randomizer] settings SAVE FAILED (randomizer.ini not writable)\n");
  return 0;
}

// ---- Shared settings-menu rows (settings_menu.h) ----
// Registered lazily (works pre-Init: the getters lazily load the ini), so
// the rows exist even when the randomizer is disabled and F12 can open the
// menu to enable it.  Value strings stay short + uppercase: the menu draws
// with a 5x7 uppercase bitmap font.

static const char *RandoRowEnabledValue(void) {
  int e = Randomizer_SettingsEnabled();
  if (e != s_boot_enabled)
    return e ? "NEXT BOOT: ON" : "NEXT BOOT: OFF";
  return e ? "ON" : "OFF";
}

static void RandoRowEnabledAdjust(int delta) {
  (void)delta;                         // left and right both toggle
  Randomizer_SettingsToggleEnabled();
}

static char s_seed_text[32];           // menu is single-threaded per frame
static const char *RandoRowSeedValue(void) {
  int seed = Randomizer_SettingsSeed();
  if (seed <= 0)
    return "RANDOM";                   // 0 = derive from time (ini default)
  if (seed != s_boot_seed)
    snprintf(s_seed_text, sizeof(s_seed_text), "%d NEXT BOOT", seed);
  else
    snprintf(s_seed_text, sizeof(s_seed_text), "%d", seed);
  return s_seed_text;
}

static void RandoRowSeedAdjust(int delta) {
  // LEFT = -1, RIGHT = +1; RETURN (delta 0) clamps to a no-op
  Randomizer_SettingsSeedAdjust(delta);
}

static char s_logic_text[40];
static const char *RandoRowLogicValue(void) {
  if (Randomizer_SettingsLogic() != s_boot_logic)
    snprintf(s_logic_text, sizeof(s_logic_text), "%s NEXT BOOT",
             Randomizer_SettingsLogicLabel());
  else
    snprintf(s_logic_text, sizeof(s_logic_text), "%s",
             Randomizer_SettingsLogicLabel());
  return s_logic_text;
}

static void RandoRowLogicAdjust(int delta) {
  Randomizer_SettingsLogicCycle(delta);
}

// One-time registration of the two rows + the commit-on-close hook.  Called
// from Randomizer_Init AND from every Randomizer_SettingsTick, so the menu
// is buildable on the very first F12 no matter which runs first.
static void SettingsMenuEnsureRegistered(void) {
  static int done;
  if (done)
    return;
  {
    static const SettingsRow kRowEnabled = {
      "RANDOMIZER", RandoRowEnabledValue, RandoRowEnabledAdjust };
    static const SettingsRow kRowSeed = {
      "SEED", RandoRowSeedValue, RandoRowSeedAdjust };
    static const SettingsRow kRowLogic = {
      "LOGIC", RandoRowLogicValue, RandoRowLogicAdjust };
    SettingsMenu_RegisterRow(&kRowEnabled);
    SettingsMenu_RegisterRow(&kRowSeed);
    SettingsMenu_RegisterRow(&kRowLogic);
  }
  SettingsMenu_RegisterOnClose(Randomizer_SettingsCommit);
  done = 1;
}

void Randomizer_SettingsTick(void) {
  SettingsMenuEnsureRegistered();      // rows exist even on the first F12
#if defined(_WIN32)
  // Cheap path: one GetAsyncKeyState poll per frame, so F12 is seen even
  // when the randomizer itself is disabled.  Edge-detected (the
  // "pressed since last call" bit of GetAsyncKeyState is unreliable), so
  // holding F12 toggles exactly once; the shared screen's own toggle then
  // owns open/close.
  // Harness-only (twitch_config.txt test=1): players use the pause-menu
  // MODS page and the file-select rows; the overlay is not a player surface.
  static int was_f12;
  int f12 = Twitch_TestMode() && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
  int edge = f12 && !was_f12;
  was_f12 = f12;
  if (edge)
    SettingsMenu_Toggle();
#endif
}

// ---- Seed nickname (cosmetic, deterministic per seed) ----
// Rando-community-style silly titles. PG-13: the silliness is in the word
// combos, not the words.
static const char *const kNickAdjectives[] = {
  "Bulging", "Soggy", "Wobbly", "Turbo", "Sparkly", "Grumpy", "Sneaky",
  "Crispy", "Buttered", "Haunted", "Chunky", "Zesty", "Suspicious",
  "Wiggly", "Feral", "Blessed", "Sweaty", "Spicy", "Lumpy", "Polite",
};
static const char *const kNickNouns[] = {
  "Wallet", "Cucco", "Boomerang", "Bottle", "Hammer", "Lamp", "Ocarina",
  "Rupee", "Flippers", "Pendant", "Shovel", "Cape", "Boots", "Mushroom",
  "Powder", "Honeycomb", "Bell", "Sword", "Shield", "Chest",
};

static uint32 NickNext(uint32 *state) {
  // same xorshift32 as the shuffle, but on a private state
  uint32 x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static void MakeSeedNickname(int seed, char *out, size_t out_size) {
  uint32 st = (uint32)seed * 2654435761u ^ 0x9e3779b9u;
  if (st == 0)
    st = 0xBAD5EED;  // xorshift hates a zero state; this seed is the one that makes one
  const int pattern = (int)(NickNext(&st) % 4);
  const char *adj = kNickAdjectives[NickNext(&st) % (sizeof(kNickAdjectives) / sizeof(kNickAdjectives[0]))];
  const char *noun = kNickNouns[NickNext(&st) % (sizeof(kNickNouns) / sizeof(kNickNouns[0]))];
  switch (pattern) {
    case 0: snprintf(out, out_size, "The Legend of %s %s", adj, noun); break;
    case 1: snprintf(out, out_size, "%s's %s Quest", adj, noun); break;
    case 2: snprintf(out, out_size, "A Link to the %s", noun); break;
    default: snprintf(out, out_size, "The Quest for the %s %s", adj, noun); break;
  }
}

// -----------------------------------------------------------------------
// Phase B: mapping tables (generated, see tools/rando_tables.inc).
// -----------------------------------------------------------------------

// The US 1.0 ROM chest table lives at PC 0xE96E, 168 records of 3 bytes
// { uint16 room (bit15 = big chest), uint8 item }.  kDungeonRoomChests holds
// exactly these records re-sorted by room, so the fork's location addresses
// resolve through the ROM order below.  Range check:
//   kForkChestBase <= addr < kForkChestBase + 3*168, (addr - base) % 3 == 0
#define kForkChestBase 0xE96E
#define kForkChestRecords 168

// kForkChestRoomWords[168], kRandoItemCodes[] - guarded because the unity
// TU already pulled the table in through rando/rando_fill.c
// (regenerate with: python tools/gen_rando_tables.py)
#ifndef RANDO_TABLES_INCLUDED
#define RANDO_TABLES_INCLUDED
#include "rando_tables.inc"
#endif

// Read a whole file.  Returns malloc'd NUL-terminated buffer or NULL.
static char *ReadAll(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[got] = 0;
  return buf;
}

// Parse locations.json enough to recover each location's ROM address
// (-1 when absent).  The scaffold's loader drops the field, and
// src/randomizer.c is the only engine file phase B may touch.
static int *ParseLocationAddresses(const char *dump_dir, int n_locations) {
  char path[512];
  snprintf(path, sizeof(path), "%s/locations.json", dump_dir);
  char *text = ReadAll(path);
  if (!text)
    return NULL;
  char err[128];
  JsonValue *root = Json_Parse(text, err, sizeof(err));
  free(text);
  if (!root || root->type != JSON_ARRAY) {
    printf("[randomizer] bad locations.json: %s\n", err);
    Json_Free(root);
    return NULL;
  }
  int *addr = (int *)malloc((size_t)n_locations * sizeof(int));
  if (!addr) {
    Json_Free(root);
    return NULL;
  }
  for (int i = 0; i < n_locations; i++)
    addr[i] = -1;
  for (const JsonValue *v = root->child; v; v = v->next) {
    const JsonValue *id = Json_Get(v, "id");
    const JsonValue *address = Json_Get(v, "address");
    if (!id || !address)
      continue;
    int loc_id = Json_AsInt(id);
    if (loc_id < 0 || loc_id >= n_locations || address->type != JSON_STRING)
      continue;
    const char *s = Json_AsString(address);
    if (strncmp(s, "0x", 2) != 0)
      continue;
    addr[loc_id] = (int)strtoul(s + 2, NULL, 16);
  }
  Json_Free(root);
  return addr;
}

// Phase B boot path: fill a world from the dumps and write the chest
// placements into kDungeonRoomChests.  Returns 1 when the table was
// applied; 0 = caller falls back to the phase-1 shuffle.
// `lf` (optional) receives the full placement log.
// `pin_location`/`pin_item`: demo-chest pin (empty strings = no pin).
// ---- rule folders: one per settings combination, built on demand ---------
static bool OptsAllDefault(const int *opt) {
  // presentation-only rows never reach the dump, so they cannot make a
  // combination "non default" as far as the rule folder is concerned
  for (int o = 0; o < kRandoOptCount; o++) if (opt[o] && !kRandoOpts[o].presentation) return false;
  return true;
}
// BOSSES != NORMAL makes the dump seed-specific: the reference rolls the boss
// of every dungeon spot with the generator's own RNG, so bosses.json (and the
// access rules that depend on which boss has to be beaten where) describe THIS
// seed only.  Every other combination stays seed-independent and keeps the
// folder name it always had, so folders built earlier are still found.
// ENTRANCES joins it for the same reason: EntranceShuffle2.py pairs the doors
// with the generator's own RNG, so entrances.json (and the edges.json built on
// top of that pairing) describe THIS seed only.
static bool OptsSeedSpecific(const int *opt) {
  return opt[kBossOptIndex] != 0 || opt[kEntranceOptIndex] != 0;
}
static void RulesDirFor(int logic, int start, const int *opt, int seed, char *dir, size_t sz) {
  if (logic < 0 || logic >= kLogicCount) logic = 0;
  if (OptsAllDefault(opt)) {
    snprintf(dir, sz, "randomizer_ref/out/logic_%s%s", kLogicKeys[logic], start == kStartStandard ? "_standard" : "");
    return;
  }
  char sig[kRandoOptCount + 1];
  int nsig = 0;
  for (int o = 0; o < kRandoOptCount; o++)
    if (!kRandoOpts[o].presentation) sig[nsig++] = (char)('a' + opt[o]);
  sig[nsig] = 0;
  char sfx[24];
  sfx[0] = 0;
  if (OptsSeedSpecific(opt))
    snprintf(sfx, sizeof sfx, "_s%d", seed);
  snprintf(dir, sz, "randomizer_ref/out/rules_%s_%s_%s%s", kLogicKeys[logic], start == kStartStandard ? "standard" : "open", sig, sfx);
}
// The full set of files a rule folder must carry.  RulesExist() requires
// every one of these before a folder counts as ready, and RulesBuild() moves
// exactly this set out of the dump - one array so the two cannot drift apart
// (a folder short even one file, such as the five that rode in after the
// original six, used to read as "ready" off rules.json alone and never got
// topped up; see the tolerated-if-absent comments below RulesBuild).
static const char *const kRuleFiles[] = { "bonks.json", "bosses.json", "drops.json", "edges.json", "entrances.json", "items.json", "locations.json", "meta.json", "pots.json", "regions.json", "rules.json" };
#define kRuleFileCount (sizeof kRuleFiles / sizeof kRuleFiles[0])
static int RulesExist(const char *dir) {
  for (size_t i = 0; i < kRuleFileCount; i++) {
    char probe[320];
    snprintf(probe, sizeof probe, "%s/%s", dir, kRuleFiles[i]);
    FILE *f = fopen(probe, "rb");
    if (!f) return 0;
    fclose(f);
  }
  return 1;
}
// python_embed\python.exe randomizer_ref\dump_logic.py ... into <dir>.build,
// then the eleven json files move into <dir>. Blocks for about six seconds.
static int RulesBuild(int logic, int start, const int *opt, int seed, const char *dir) {
  FILE *pe = fopen("python_embed/python.exe", "rb");
  if (!pe) {
    printf("[randomizer] no python_embed folder: cannot build %s%c", dir, 10);
    return 0;
  }
  fclose(pe);
  if (logic < 0 || logic >= kLogicCount) logic = 0;
  // Seed-independent combinations keep dumping at the historical seed 1234 so
  // their folders stay reproducible; a seed-specific one has to be generated
  // with the file's real seed or its boss placement would not be the file's.
  int dump_seed = OptsSeedSpecific(opt) ? seed : 1234;
  char cmd[1400];
  int n = snprintf(cmd, sizeof cmd,
                   "\"\"python_embed\\python.exe\" \"randomizer_ref\\dump_logic.py\" --repo randomizer_ref\\ALttPDoorRandomizer "
                   "--out \"%s.build\" --seed %d --logic %s --mode %s",
                   dir, dump_seed, kLogicKeys[logic], start == kStartStandard ? "standard" : "open");
  for (int o = 0; o < kRandoOptCount && n < (int)sizeof cmd - 64; o++)
    if (opt[o] && !kRandoOpts[o].presentation)
      n += kRandoOpts[o].boolflag
               ? snprintf(cmd + n, sizeof cmd - n, " --setting %s", kRandoOpts[o].refkey)
               : snprintf(cmd + n, sizeof cmd - n, " --setting %s=%s", kRandoOpts[o].refkey, kRandoOpts[o].vals[opt[o]]);
  n += snprintf(cmd + n, sizeof cmd - n, " > randomizer_ref\\out\\dump_last.log 2>&1\"");
  printf("[randomizer] building rules: %s%c", cmd, 10);
  fflush(stdout);
  int rc = system(cmd);
  CreateDirectoryA(dir, NULL);
  // bosses.json rides along with the other six.  Folders built before boss
  // shuffle existed simply have none; the move fails silently and
  // ApplyBossShuffle then leaves every boss vanilla.
  // drops.json rides along the same way bosses.json does: folders built before
  // KEY DROPS existed simply have none, the move fails silently, and the fill
  // then leaves every enemy key drop vanilla.
  // pots.json rides along exactly like drops.json: a folder built before the
  // POTS row existed has none, the move fails silently, and the pot keys then
  // stay vanilla (LoadPotTable tolerates the absence and refuses the shuffle).
  // entrances.json is the fourth of these tolerated-if-absent files (ENTRANCES).
  // bonks.json rides along exactly like pots.json: a folder built before the
  // BONK DROPS row existed has none, the move fails silently, and the bonk
  // prizes then stay vanilla (LoadBonkTable tolerates the absence and refuses
  // the shuffle).
  for (size_t i = 0; i < kRuleFileCount; i++) {
    char src[360], dst[360];
    snprintf(src, sizeof src, "%s.build/seed_%d/%s", dir, dump_seed, kRuleFiles[i]);
    snprintf(dst, sizeof dst, "%s/%s", dir, kRuleFiles[i]);
    MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING);
  }
  {
    char tmp[360];
    snprintf(tmp, sizeof tmp, "%s.build/seed_%d", dir, dump_seed);
    RemoveDirectoryA(tmp);
    snprintf(tmp, sizeof tmp, "%s.build", dir);
    RemoveDirectoryA(tmp);
  }
  int ok = RulesExist(dir);
  printf("[randomizer] rules %s: %s (python exit %d)%c", dir, ok ? "ready" : "FAILED, see randomizer_ref/out/dump_last.log", rc, 10);
  fflush(stdout);
  return ok;
}
// the folder to fill from: the combination's own, built if needed, else the plain tier
static void RulesDirEnsure(int logic, int start, const int *opt, int seed, char *dir, size_t sz) {
  RulesDirFor(logic, start, opt, seed, dir, sz);
  if (RulesExist(dir)) return;
  if (RulesBuild(logic, start, opt, seed, dir)) return;
  if (logic < 0 || logic >= kLogicCount) logic = 0;
  printf("[randomizer] falling back to the plain %s rules%c", kLogicKeys[logic], 10);
  snprintf(dir, sz, "randomizer_ref/out/logic_%s", kLogicKeys[logic]);
}
static int s_rules_tried;
// The seed the NEXT file will be stamped with.  `seed=0` means RANDOM, and
// with BOSSES on the rule folder is per seed - so the number has to be rolled
// BEFORE the folder is built (Randomizer_PrepareRules, from the BEGIN row) and
// then reused by Randomizer_NewFileStamp, or the file would play a seed whose
// bosses were never dumped.  Cleared once a file is stamped, and whenever an
// edit on the RANDOMIZER page invalidates the pending combination.
static int s_pending_seed_rolled;
static int PendingFileSeed(void) {
  SettingsEnsureLoaded();
  if (s_cfg_seed) return s_cfg_seed;
  if (!s_pending_seed_rolled) s_pending_seed_rolled = (int)(time(NULL) & 0x7fffffff);
  return s_pending_seed_rolled;
}
int Randomizer_RulesReady(void) {
  SettingsEnsureLoaded();
  if (s_rules_tried) return 1;
  char dir[300];
  RulesDirFor(s_cfg_logic, s_cfg_start, s_cfg_opt, PendingFileSeed(), dir, sizeof dir);
  return RulesExist(dir);
}
void Randomizer_PrepareRules(void) {
  SettingsEnsureLoaded();
  s_rules_tried = 1;
  char dir[300];
  RulesDirEnsure(s_cfg_logic, s_cfg_start, s_cfg_opt, PendingFileSeed(), dir, sizeof dir);
}

// ---- boss shuffle (BOSSES row / bosses.json) ----------------------------
//
// The reference randomizer patches boss shuffle into the ROM by rewriting the
// underworld sprite list of each boss super-tile (source/enemizer/Bossmizer.py
// boss_adjust + boss_writes): drop the vanilla boss' sprites from the front of
// the room's list, insert the new boss' sprites, cap the list at 15 entries,
// and point the room header at the new boss' sprite sheet.  The C port has the
// same two tables as read-only assets (kDungeonSprites/kDungeonSpriteOffs and
// kDungeonRoomHeaders/kDungeonRoomHeadersOffs), and both are built with
// overlapping/deduplicated runs, so editing them in place would corrupt other
// rooms.  Instead every changed room gets a small runtime copy here and the two
// asset readers (Dungeon_LoadSprites in sprite.c, GetRoomHeaderPtr in
// dungeon.c) prefer it when one exists.
//
// Everything that makes a boss room "done" is keyed off the ROOM, not the boss:
// the death explosion spawns the heart container (sprite 0xEA) from
// SpriteModule_Explode once the screen is clear, taking it sets
// dung_savegame_state_bits |= 0x8000, and the room tag 0x25
// (RoomTag_GetHeartForPrize) then drops the pendant/crystal for
// cur_palace_index.  None of that is touched here, so any boss finishes any
// dungeon.

enum {
  kBossArmos = 0, kBossLanmolas, kBossMoldorm, kBossHelmasaur, kBossArrghus,
  kBossMothula, kBossBlind, kBossKholdstare, kBossVitreous, kBossTrinexx,
  kBossCount
};
static const char *const kBossRefNames[kBossCount] = {
  "Armos Knights", "Lanmolas", "Moldorm", "Helmasaur King", "Arrghus",
  "Mothula", "Blind", "Kholdstare", "Vitreous", "Trinexx",
};
// Sprite records are the ROM's own 3-byte (y, x, type) form, exactly what
// Dungeon_LoadSprites walks: y = layer<<7 | (subtype&0x18)<<2 | tile_y,
// x = (subtype&7)<<5 | tile_x, and x >= 0xe0 (subtype 7) marks an overlord.
// The coordinates below are Bossmizer's add_*_to_list, which are also the
// vanilla positions of each boss in its home room.
static const uint8 kBossSpr_Armos[] = {
  0x05, 0x04, 0x53,  0x05, 0x07, 0x53,  0x05, 0x0a, 0x53,
  0x08, 0x0a, 0x53,  0x08, 0x07, 0x53,  0x08, 0x04, 0x53,
  0x08, 0xe7, 0x19,   // overlord 0x19: the Armos Knights' formation driver
};
static const uint8 kBossSpr_Lanmolas[] = {
  0x07, 0x06, 0x54,  0x07, 0x09, 0x54,  0x09, 0x07, 0x54,
};
static const uint8 kBossSpr_Moldorm[] = { 0x09, 0x09, 0x09 };
static const uint8 kBossSpr_Helmasaur[] = { 0x06, 0x07, 0x92 };
static const uint8 kBossSpr_Arrghus[] = {
  0x07, 0x07, 0x8c,
  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,
  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,
  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,  0x07, 0x07, 0x8d,
  0x07, 0x07, 0x8d,   // 13 puffs; SpritePrep_Arrghi keys the last one off k==13
};
static const uint8 kBossSpr_Mothula[] = { 0x06, 0x08, 0x88 };
static const uint8 kBossSpr_Blind[] = { 0x05, 0x09, 0xce };
static const uint8 kBossSpr_Kholdstare[] = {
  0x05, 0x07, 0xa3,   // shell
  0x05, 0x07, 0xa4,   // falling ice
  0x05, 0x07, 0xa2,   // Kholdstare himself
};
static const uint8 kBossSpr_Vitreous[] = { 0x05, 0x07, 0xbd };
static const uint8 kBossSpr_Trinexx[] = {
  0x05, 0x07, 0xcb,  0x05, 0x07, 0xcc,  0x05, 0x07, 0xcd,   // rock/fire/ice head
};
typedef struct BossRecipe { const uint8 *rec; uint8 n; uint8 sheet; } BossRecipe;
// sheet = SpriteSheets.required_boss_sheets, i.e. room header byte 3.
static const BossRecipe kBossRecipe[kBossCount] = {
  { kBossSpr_Armos,      7, 9 },
  { kBossSpr_Lanmolas,   3, 11 },
  { kBossSpr_Moldorm,    1, 12 },
  { kBossSpr_Helmasaur,  1, 21 },
  { kBossSpr_Arrghus,   14, 20 },
  { kBossSpr_Mothula,    1, 26 },
  { kBossSpr_Blind,      1, 32 },
  { kBossSpr_Kholdstare, 3, 22 },
  { kBossSpr_Vitreous,   1, 22 },
  { kBossSpr_Trinexx,    3, 23 },
};

#define kBossRoomCount 13
typedef struct BossRoom {
  uint8 room;          // underworld super-tile (RoomList.boss_rooms)
  uint8 def_boss;      // the boss vanilla puts there
  uint8 remove;        // vanilla records to drop off the front of the list
  const char *dungeon;
  const char *spot;    // Ganon's Tower level, NULL for a normal dungeon
} BossRoom;
// `remove` is Bossmizer.boss_room_remove_data, except Turtle Rock: the
// reference removes 2 of Trinexx' 3 head sprites there, which would leave a
// stray ice head (0xCD) behind - and a live sprite in the room stops
// Sprite_CheckIfScreenIsClear, so the heart container would never spawn.  3.
static const BossRoom kBossRooms[kBossRoomCount] = {
  { 0xc8, kBossArmos,      7,  "Eastern Palace",     NULL },
  { 0x33, kBossLanmolas,   3,  "Desert Palace",      NULL },
  { 0x07, kBossMoldorm,    1,  "Tower of Hera",      NULL },
  { 0x5a, kBossHelmasaur,  1,  "Palace of Darkness", NULL },
  { 0x06, kBossArrghus,   14,  "Swamp Palace",       NULL },
  { 0x29, kBossMothula,    1,  "Skull Woods",        NULL },
  { 0xac, kBossBlind,      1,  "Thieves Town",       NULL },
  { 0xde, kBossKholdstare, 3,  "Ice Palace",         NULL },
  { 0x90, kBossVitreous,   1,  "Misery Mire",        NULL },
  { 0xa4, kBossTrinexx,    3,  "Turtle Rock",        NULL },
  { 0x1c, kBossArmos,      7,  "Ganons Tower",       "bottom" },
  { 0x6c, kBossLanmolas,   3,  "Ganons Tower",       "middle" },
  { 0x4d, kBossMoldorm,    1,  "Ganons Tower",       "top" },
};
#define kBossMaidenRoom 0x45   // Thieves Town attic: the maiden that wakes Blind
#define kBossMaxRec 15         // the reference's cap; the engine has 16 slots
#define kBossOvrMax (kBossRoomCount + 1)

typedef struct BossOvr {
  int room;
  int boss;                             // -1 for the maiden-room edit
  uint8 spr[1 + kBossMaxRec * 3 + 1];   // sort byte + records + 0xff
  int spr_len;
  int has_hdr;
  uint8 hdr[14];
} BossOvr;
static BossOvr s_boss_ovr[kBossOvrMax];
static int s_boss_ovr_n;
static int s_boss_arrghus_on_land;   // Arrghus placed outside Swamp Palace
static int s_boss_trinexx_loose;     // Trinexx placed outside Turtle Rock

static void BossClearOverrides(void) {
  s_boss_ovr_n = 0;
  s_boss_arrghus_on_land = 0;
  s_boss_trinexx_loose = 0;
}

// The room's vanilla sprite list, unpacked into 3-byte records.
static int BossReadVanillaList(int room, uint8 *sort, uint8 *rec, int max) {
  const uint8 *src = kDungeonSprites + kDungeonSpriteOffs[room];
  int n = 0;
  *sort = *src++;
  for (; *src != 0xff && n < max; src += 3, n++)
    memcpy(rec + n * 3, src, 3);
  return n;
}

static BossOvr *BossNewOverride(int room, int boss) {
  if (s_boss_ovr_n >= kBossOvrMax) return NULL;
  BossOvr *o = &s_boss_ovr[s_boss_ovr_n++];
  memset(o, 0, sizeof *o);
  o->room = room;
  o->boss = boss;
  return o;
}

static void BossPackList(BossOvr *o, uint8 sort, const uint8 *rec, int n) {
  if (n > kBossMaxRec) n = kBossMaxRec;   // reference: del sprite_list[15:]
  o->spr[0] = sort;
  memcpy(o->spr + 1, rec, (size_t)n * 3);
  o->spr[1 + n * 3] = 0xff;
  o->spr_len = 1 + n * 3 + 1;
}

// Build the runtime sprite list + header for one boss room.  Mirrors
// Bossmizer.boss_adjust.
static void BossBuildRoom(const BossRoom *br, int boss) {
  uint8 rec[48 * 3], sort;
  int n = BossReadVanillaList(br->room, &sort, rec, 48);
  if (n > br->remove) {
    memmove(rec, rec + br->remove * 3, (size_t)(n - br->remove) * 3);
    n -= br->remove;
  } else {
    n = 0;
  }
  if (boss == kBossVitreous) {
    // add_vitreous_to_list: Vitreous does not share the super-tile with any
    // other sprite, only overlords (x >= 0xe0) survive.
    int m = 0;
    for (int i = 0; i < n; i++)
      if (rec[i * 3 + 1] >= 0xe0)
        memmove(rec + m++ * 3, rec + i * 3, 3);
    n = m;
  } else if (boss == kBossHelmasaur && br->room == 0x29) {
    n = 0;   // add_helmasaur_king_to_list: keeps the Skull Woods helma-copter out
  }
  const BossRecipe *r = &kBossRecipe[boss];
  if (n + r->n > (int)(sizeof rec / 3)) n = (int)(sizeof rec / 3) - r->n;
  memmove(rec + r->n * 3, rec, (size_t)n * 3);
  memcpy(rec, r->rec, (size_t)r->n * 3);
  n += r->n;

  BossOvr *o = BossNewOverride(br->room, boss);
  if (!o) return;
  BossPackList(o, sort, rec, n);
  memcpy(o->hdr, kDungeonRoomHeaders + kDungeonRoomHeadersOffs[br->room], sizeof o->hdr);
  o->hdr[3] = r->sheet;   // sprite_graphics_index = hdr[3] + 0x40
  o->has_hdr = 1;
}

// Thieves Town without Blind: the maiden that would follow Link into the boss
// room has nothing to wake, so drop her (Bossmizer deletes room 0x45's first
// sprite for the same reason).
static void BossDropThievesMaiden(void) {
  uint8 rec[48 * 3], sort;
  int n = BossReadVanillaList(kBossMaidenRoom, &sort, rec, 48);
  if (n < 1) return;
  memmove(rec, rec + 3, (size_t)(n - 1) * 3);
  n--;
  BossOvr *o = BossNewOverride(kBossMaidenRoom, -1);
  if (o) BossPackList(o, sort, rec, n);
}

static int BossIndexByName(const char *name) {
  for (int i = 0; i < kBossCount; i++)
    if (!strcmp(kBossRefNames[i], name)) return i;
  return -1;
}

// Read <dir>/bosses.json (written by randomizer_ref/dump_logic.py) and put
// every non-vanilla boss into its room.  Missing file = nothing to do, which
// is also what a rule folder built before boss shuffle existed gives.
static void ApplyBossShuffle(const char *dir) {
  BossClearOverrides();
  char path[512];
  snprintf(path, sizeof path, "%s/bosses.json", dir);
  char *text = ReadAll(path);
  if (!text) return;
  char err[128];
  JsonValue *root = Json_Parse(text, err, sizeof err);
  free(text);
  const JsonValue *spots = root ? Json_Get(root, "spots") : NULL;
  if (!spots || spots->type != JSON_ARRAY) {
    printf("[bosses] bad %s%c", path, 10);
    Json_Free(root);
    return;
  }
  int tt_boss = kBossBlind;
  for (const JsonValue *v = spots->child; v; v = v->next) {
    const JsonValue *jd = Json_Get(v, "dungeon"), *js = Json_Get(v, "spot"), *jb = Json_Get(v, "boss");
    if (!jd || !jb || jd->type != JSON_STRING || jb->type != JSON_STRING) continue;
    const char *dungeon = Json_AsString(jd);
    const char *spot = (js && js->type == JSON_STRING) ? Json_AsString(js) : NULL;
    int boss = BossIndexByName(Json_AsString(jb));
    const BossRoom *br = NULL;
    for (int i = 0; i < kBossRoomCount; i++) {
      const BossRoom *c = &kBossRooms[i];
      if (strcmp(c->dungeon, dungeon)) continue;
      if ((c->spot == NULL) != (spot == NULL)) continue;
      if (c->spot && strcmp(c->spot, spot)) continue;
      br = c;
      break;
    }
    if (!br || boss < 0) {
      printf("[bosses] unknown spot %s%s%s%s = %s%c", dungeon, spot ? " (" : "", spot ? spot : "",
             spot ? ")" : "", Json_AsString(jb), 10);
      continue;
    }
    printf("[bosses] %s%s%s%s = %s%c", dungeon, spot ? " (" : "", spot ? spot : "", spot ? ")" : "",
           kBossRefNames[boss], 10);
    if (br->room == 0xac) tt_boss = boss;
    if (boss == br->def_boss) continue;   // vanilla here, leave the asset alone
    BossBuildRoom(br, boss);
    // The two ROM fixes Bossmizer.boss_writes applies for a boss that left
    // home; both are global there, so they are global here too.
    if (boss == kBossArrghus && br->room != 0x06) s_boss_arrghus_on_land = 1;
    if (boss == kBossTrinexx && br->room != 0xa4) s_boss_trinexx_loose = 1;
  }
  if (tt_boss != kBossBlind) BossDropThievesMaiden();
  Json_Free(root);
  fflush(stdout);
}

// ENTRANCE SHUFFLE: arm the door wiring from this folder's entrances.json.
// A folder built before this round simply has none and the doors stay vanilla
// - harmless when the file did not ask for a shuffle, but LOUD when it did:
// the edges.json in that same folder was built on top of the shuffled wiring,
// so the fill already trusts a graph the engine would not be playing.
static void ApplyEntranceShuffle(const char *dir, int want) {
  RandoEntrance_Load(dir);
  if (want && !RandoEntrance_Active())
    printf("[entrances] %s asked for but this rule folder has no usable "
           "entrances.json: doors stay vanilla and this seed may not be "
           "winnable - delete %s and let it rebuild%c",
           kRandoOpts[kEntranceOptIndex].vals[want], dir, 10);
}

static const BossOvr *BossFindOverride(int room) {
  for (int i = 0; i < s_boss_ovr_n; i++)
    if (s_boss_ovr[i].room == room) return &s_boss_ovr[i];
  return NULL;
}

const uint8 *Randomizer_BossRoomSprites(int room) {
  const BossOvr *o = BossFindOverride(room);
  return o && o->spr_len ? o->spr : NULL;
}

const uint8 *Randomizer_BossRoomHeader(int room, const uint8 *vanilla) {
  const BossOvr *o = BossFindOverride(room);
  return (o && o->has_hdr) ? o->hdr : vanilla;
}

// Blind normally only appears once the Thieves Town maiden has been led into
// room 0xAC (dung_savegame_state_bits & 0x2000).  Wherever boss shuffle put
// him there is no maiden to lead, so the fight starts on entry - the same hole
// the reference plugs with its "blind boss door flag" ROM write.
int Randomizer_BossBlindNoMaiden(int room) {
  const BossOvr *o = BossFindOverride(room);
  return o && o->boss == kBossBlind;
}

// Arrghus only moves over water tiles (kSpriteInit_Flags5[0x8C] & 0x40).  The
// reference clears that byte when he is placed outside Swamp Palace.
int Randomizer_BossArrghusOnLand(void) { return s_boss_arrghus_on_land; }

// Trinexx' ice breath rewrites the room tilemap with a frozen-floor tile,
// which only Turtle Rock's boss room is built for; the reference NOPs the call
// when a Trinexx stands anywhere else.
int Randomizer_BossTrinexxLoose(void) { return s_boss_trinexx_loose; }


// non-chest locations (uncle, pedestal, NPCs, tablets...) -> the fill's item
// code, handed out by the sprite code through Randomizer_GiftItem
// One entry per non-chest reference location that resolves to an engine
// receipt.  A shopsanity dump has 203 non-chest locations (367 total, 164 of
// them chest records), so this has to clear 203 with room to spare for the
// pot / drop rounds.
#define kGiftMax 288
static struct { char loc[48]; int code; } s_gift[kGiftMax];
static int s_gift_n;

int Randomizer_GiftItem(const char *location, int vanilla) {
  if (!location || !Randomizer_FileEnabled()) return vanilla;
  for (int i = 0; i < s_gift_n; i++)
    if (!strcmp(s_gift[i].loc, location)) {
      printf("[randomizer] gift %s: item %d (vanilla %d)\n", location, s_gift[i].code, vanilla);
      return s_gift[i].code;
    }
  return vanilla;
}

// ---- sprite spots that are NOT chests: heart pieces, shop stock, gifts ----
//
// The fill hands every reference location an engine item code; the ones with
// a chest record are written into kDungeonRoomChests, everything else waits
// in the gift table for the sprite that owns it to ask (Randomizer_GiftItem).
// This block is the "where am I standing" half of that: it turns what the
// ENGINE knows when the sprite runs into the reference's name for the spot.
//
// HEART PIECES.  All 16 of them are engine sprite 0xEB (Sprite_HeartPiece);
// the sprite carries no identity of its own, so it is identified exactly the
// way the vanilla save flag identifies it (HeartUpgrade_SetObtainedFlag,
// sprite_main.c):
//   outdoors  overworld_area_index - the index the sprite list itself was
//             loaded from (GetOverworldSpritePtr), so it is the same value
//             from every quadrant of a large area and cannot drift.
//   indoors   dungeon_room_index plus (sprite_x_hi & 1), the bit the engine
//             uses to pick between a room's two heart-piece save flags
//             (0x2000 / 0x4000) - room 0x11b really does hold two pieces,
//             Cave 45 on the low half and Graveyard Cave on the high one.
// The two tables were read out of the engine's own asset file: every 0xEB in
// kOverworldSprites (9) and kDungeonSprites (7), nothing guessed.  Boss heart
// containers are a different sprite (0xEA with sprite_A set) and already
// follow the fill as "<Dungeon> - Boss".
//
// SHOPS (the SHOPS row on SHUFFLED).  A shop is a room plus the overworld
// area its door sits on: vanilla ALttP gives all four dark-world shops the
// same room (0x010f) AND the same entrance id (0x60), and Lake Hylia / Dark
// Death Mountain share room 0x0112 and entrance 0x58, so which_entrance
// cannot tell them apart.  The overworld area behind the door can, and it is
// what the engine's own overworld entrance table keys on
// (kOverworld_Entrance_Area vs overworld_area_index, overworld.c:264);
// overworld_area_index is not written while Link is indoors, so it still
// names the door he came through.  The three stock slots are the item
// sprite's own subtype2, which is distinct per slot in every shop layout
// (SpritePrep_Shopkeeper).
static const struct { uint8 area; const char *loc; } kRandoOwHearts[] = {
  { 0x03, "Spectacle Rock" },      { 0x05, "Floating Island" },
  { 0x28, "Maze Race" },           { 0x30, "Desert Ledge" },
  { 0x35, "Lake Hylia Island" },   { 0x3b, "Sunken Treasure" },
  { 0x4a, "Bumper Cave Ledge" },   { 0x5b, "Pyramid" },
  { 0x81, "Zora's Ledge" },
};
static const struct { uint16 room; uint8 xbit; const char *loc; } kRandoUwHearts[] = {
  { 0x0e1, 1, "Lost Woods Hideout" },
  { 0x0e2, 1, "Lumberjack Tree" },
  { 0x0ea, 0, "Spectacle Rock Cave" },
  { 0x11b, 0, "Cave 45" },
  { 0x11b, 1, "Graveyard Cave" },
  { 0x126, 1, "Checkerboard Cave" },
  { 0x127, 0, "Peg Cave" },
};

const char *Randomizer_HeartPieceLocation(int indoors, int area, int room, int xbit) {
  size_t i;
  if (!indoors) {
    for (i = 0; i < sizeof kRandoOwHearts / sizeof kRandoOwHearts[0]; i++)
      if (kRandoOwHearts[i].area == (uint8)area)
        return kRandoOwHearts[i].loc;
    return NULL;
  }
  for (i = 0; i < sizeof kRandoUwHearts / sizeof kRandoUwHearts[0]; i++)
    if (kRandoUwHearts[i].room == (uint16)room && kRandoUwHearts[i].xbit == (uint8)(xbit & 1))
      return kRandoUwHearts[i].loc;
  return NULL;
}

// The generous guys are one sprite (NiceThiefWithGift) in three rooms; the
// reference names two of them.  Keyed on the full 16-bit room, because the
// engine's own dispatch only compares the low byte.
const char *Randomizer_GenerousGuyLocation(int room) {
  switch (room & 0xfff) {
  case 0x11e: return "Hype Cave - Generous Guy";
  case 0x123: return "Mini Moldorm Cave - Generous Guy";
  default: return NULL;
  }
}

// room low byte + overworld area (0xff = any area) -> reference shop name.
// Paradox Shop sits inside Paradox Cave and has no overworld door of its own,
// but its room 0x00ff is unique, so the area is a wildcard there.  The Potion
// Shop's three cauldrons are not shop-item sprites; the reference counts them
// as shop slots all the same.  Capacity Upgrade (the Lake Hylia happiness
// pond) is deliberately absent - see the round report.
static const struct { uint8 room, area; const char *shop; } kRandoShops[] = {
  { 0x0f, 0x42, "Dark Lumberjack Shop" },
  { 0x0f, 0x56, "Dark Potion Shop" },
  { 0x0f, 0x58, "Village of Outcasts Shop" },
  { 0x0f, 0x75, "Dark Lake Hylia Shop" },
  { 0x10, 0x5a, "Red Shield Shop" },
  { 0x12, 0x35, "Lake Hylia Shop" },
  { 0x12, 0x45, "Dark Death Mountain Shop" },
  { 0x1f, 0x18, "Kakariko Shop" },
  { 0xff, 0xff, "Paradox Shop" },
  { 0x09, 0xff, "Potion Shop" },
};

// "<Shop> - Left|Middle|Right" for a stock slot, or NULL when this file does
// not shuffle shops - then every shop behaves exactly as vanilla, including
// its "you already have one" and "you need an empty bottle" refusals.  The
// name is assembled in one shared static buffer: every caller uses it (or
// only tests it for NULL) before asking again, which the sprite handlers do.
const char *Randomizer_ShopSlot(int room, int area, int slot) {
  static const char *const kSlotName[3] = { "Left", "Middle", "Right" };
  static char buf[64];
  size_t i;
  if (slot < 0 || slot > 2)
    return NULL;
  if (!Randomizer_FileEnabled() || Randomizer_FileOptValue("shopsanity") != 1)
    return NULL;
  for (i = 0; i < sizeof kRandoShops / sizeof kRandoShops[0]; i++) {
    if (kRandoShops[i].room != (uint8)room)
      continue;
    if (kRandoShops[i].area != 0xff && kRandoShops[i].area != (uint8)area)
      continue;
    snprintf(buf, sizeof buf, "%s - %s", kRandoShops[i].shop, kSlotName[slot]);
    return buf;
  }
  return NULL;
}

// ---- KEY DROPS = SHUFFLED: the fourteen enemy key drops -------------------
//
// The reference calls them "<Dungeon> - <Enemy> Key Drop": fourteen enemies
// that drop a small key (Hyrule Castle's ball-and-chain trooper drops the big
// key) instead of the usual random prize.  With the KEY DROPS row on SHUFFLED
// the dump is built with `--setting dropshuffle=keys`, which makes those
// fourteen real item slots: the fill may put anything there, and the keys
// they used to hold go into the pool and turn up anywhere else.
//
// HOW A DROP IS NAMED.  The engine never sees a location name; when an enemy
// dies it knows the underworld super-tile it is standing in and which entry
// of that room's sprite list the enemy was loaded from.  That pair is exactly
// the reference's own identity for a drop: PotShuffle.key_drop_data stores
// (snes_address, super_tile, sprite_index).  drops.json (dump_logic.py
// dump_drops) carries the pair per location, with the reference's raw index
// already converted to the engine's LOAD SLOT - Dungeon_LoadSingleSprite
// gives a slot only to real sprites, so overlord records (x >= 0xe0) in front
// of the enemy do not count, and the 0xe4 "drops a key" marker is not a list
// entry of the reference's at all.
//
// The engine's slot is what sprite_N[k] holds while the enemy is alive, and
// SpriteDeath stashes it in sprite_subtype[k] before the dropped key takes
// the sprite slot over (vanilla does that so case 12 can restore sprite_N for
// the room's "this one is gone" bit) - so the identity survives from the kill
// to the pickup with nothing new to remember.  Cross-checked against the
// engine's own kDungeonSprites asset: all fourteen (room, slot) pairs carry
// the expected sprite type and die_action, and the asset holds exactly
// fourteen die_action markers in the whole game, so the set is closed.
//
// WHAT LINK GETS.  The dropped key sprite is spawned exactly as in vanilla
// (same physics, same graphics); the substitution happens when Link absorbs
// it, in Sprite_HandleAbsorptionByPlayer case 12 / 13, where the fill's
// receipt code goes through Link_ReceiveItem instead of the silent
// link_num_keys++ / Link_ReceiveItem(0x32).  A drop the fill left holding its
// own vanilla item (code 0x24 / 0x32 - very common, because with SMALL KEYS
// on OWN DUNGEON the keys are not in the shuffled pool at all) takes the
// vanilla path untouched, so nothing gets a receipt popup it did not earn.
//
// Everything here is dead unless the LOADED FILE has KEY DROPS = SHUFFLED and
// its rule folder had a drops.json: s_keydrops_live gates both engine calls,
// and it is only ever set at file-load time, in ApplyFillPlacements.

#define kDropMax 24
static struct {
  char loc[48];   // the reference's name for the location
  uint16 room;    // underworld super-tile the enemy stands in
  uint8 slot;     // engine sprite slot (Dungeon_LoadSingleSprite's k)
  uint8 kind;     // sprite type the engine must find there, 0 = unknown
  uint8 big;      // vanilla drops the big key (die_action 2), not a small one
  int code;       // the fill's receipt code for this slot, or -1
} s_drops[kDropMax];
static int s_drop_n;
static int s_keydrops_live;   // the loaded file shuffles drops AND has a table

// Read <dir>/drops.json.  A missing or unreadable file is not an error: the
// folder was built before KEY DROPS existed, and the drops stay vanilla.
static void LoadDropTable(const char *dir) {
  s_drop_n = 0;
  s_keydrops_live = 0;
  char path[512];
  snprintf(path, sizeof path, "%s/drops.json", dir);
  char *text = ReadAll(path);
  if (!text) return;
  char err[128];
  JsonValue *root = Json_Parse(text, err, sizeof err);
  free(text);
  const JsonValue *arr = root ? Json_Get(root, "drops") : NULL;
  if (!arr || arr->type != JSON_ARRAY) {
    printf("[keydrops] bad %s%c", path, 10);
    Json_Free(root);
    return;
  }
  for (const JsonValue *v = arr->child; v && s_drop_n < kDropMax; v = v->next) {
    const char *name = Json_AsString(Json_Get(v, "name"));
    const JsonValue *jr = Json_Get(v, "room"), *js = Json_Get(v, "slot");
    const JsonValue *jk = Json_Get(v, "kind");
    if (!name || !jr || !js || jr->type != JSON_NUMBER || js->type != JSON_NUMBER)
      continue;
    snprintf(s_drops[s_drop_n].loc, sizeof s_drops[0].loc, "%s", name);
    s_drops[s_drop_n].room = (uint16)Json_AsInt(jr);
    s_drops[s_drop_n].slot = (uint8)Json_AsInt(js);
    s_drops[s_drop_n].kind = (jk && jk->type == JSON_NUMBER) ? (uint8)Json_AsInt(jk) : 0;
    s_drops[s_drop_n].big = (uint8)Json_AsBool(Json_Get(v, "big"));
    s_drops[s_drop_n].code = -1;
    s_drop_n++;
  }
  Json_Free(root);
}

// Fill in every drop's receipt code from the gift table the apply loop just
// built (a drop has no chest record, so its code lands there like an NPC
// gift) and arm the engine hooks.  Called at the end of ApplyFillPlacements.
static void ResolveDropCodes(int shuffled) {
  int named = 0;
  for (int i = 0; i < s_drop_n; i++) {
    s_drops[i].code = -1;
    for (int g = 0; g < s_gift_n; g++)
      if (!strcmp(s_gift[g].loc, s_drops[i].loc)) { s_drops[i].code = s_gift[g].code; break; }
    if (s_drops[i].code >= 0) named++;
  }
  s_keydrops_live = shuffled && s_drop_n > 0;
  if (s_keydrops_live)
    printf("[keydrops] SHUFFLED: %d drops, %d resolved to a receipt%c", s_drop_n, named, 10);
  else if (shuffled)
    printf("[keydrops] SHUFFLED but this rule folder has no drops.json: "
           "enemy drops stay vanilla%c", 10);
}

// What the enemy that just died in sprite slot |k| was holding, remembered
// until Link walks into the dropped key.  Indexed by sprite slot, because the
// drop takes the dying enemy's slot over; the (room, slot) pair is stored too
// so a stale entry can never be mistaken for this room's drop.
static struct { int room, slot, code; } s_drop_live[16];

// SpriteDeath, on the "this enemy drops a key" path.  |slot| is sprite_N[k],
// the load slot, read before vanilla recycles it into sprite_subtype.
void Randomizer_KeyDropSpawned(int k, int room, int slot, int type) {
  if ((unsigned)k >= 16) return;
  s_drop_live[k].room = -1;
  s_drop_live[k].slot = -1;
  s_drop_live[k].code = -1;
  // every drop lives in an underworld room; outdoors dungeon_room_index2 is
  // whatever dungeon Link was in last, so it must not be trusted
  if (!s_keydrops_live || !player_is_indoors) return;
  for (int i = 0; i < s_drop_n; i++) {
    if (s_drops[i].room != (uint16)room || s_drops[i].slot != (uint8)slot) continue;
    // the sprite type is the sanity check on the whole (room, slot) mapping:
    // if the dump and the engine's sprite list ever disagree, leave the drop
    // alone rather than hand out somebody else's item
    if (s_drops[i].kind && s_drops[i].kind != (uint8)type) {
      printf("[keydrops] %s: room %03x slot %d holds sprite %02x, expected %02x - "
             "left vanilla%c", s_drops[i].loc, room, slot, type, s_drops[i].kind, 10);
      return;
    }
    s_drop_live[k].room = room;
    s_drop_live[k].slot = slot;
    s_drop_live[k].code = s_drops[i].code;
    return;
  }
}

// Sprite_HandleAbsorptionByPlayer, case 12 (small key) / 13 (big key): the
// receipt code to hand Link instead of the vanilla key, or -1 for "do exactly
// what vanilla does".  |slot| is sprite_subtype[k], where SpriteDeath parked
// the load slot.  A code that IS the vanilla receipt returns -1 too, so a
// drop the fill did not actually change keeps its silent vanilla pickup.
int Randomizer_KeyDropReceipt(int k, int room, int slot, int vanilla) {
  if (!s_keydrops_live || (unsigned)k >= 16) return -1;
  if (s_drop_live[k].room != room || s_drop_live[k].slot != slot) return -1;
  int code = s_drop_live[k].code;
  if (code < 0 || code == vanilla) return -1;
  printf("[keydrops] room %03x slot %d: item %d (vanilla %d)%c", room, slot, code, vanilla, 10);
  return code;
}

// ---- POT KEYS (POTS row / pots.json) -------------------------------------
//
// The reference calls them "<Dungeon> - <Spot> Pot Key": nineteen small keys
// hidden under a pot (one of them, "Ice Palace - Hammer Block Key Drop", is
// under a block - it is spelled like an enemy drop but the reference types it
// Pot, so it belongs to this set, not to the fourteen above).  With the POTS
// row on SHUFFLED the dump is built with `--setting pottery=keys`, which makes
// those nineteen real item slots and puts their keys back into the pool.
//
// Only `keys` is offered.  The wider pottery modes (lottery, cave, dungeon,
// reduced, clustered, nonempty, cavekeys) turn hundreds of ORDINARY pots into
// slots and rewrite the room data so the contents move around; this engine
// cannot deliver those - see WHERE THE ENGINE KEEPS POT CONTENTS below - and a
// pot the engine cannot deliver makes a seed unwinnable, so it is all or
// nothing per mode.
//
// WHERE THE ENGINE KEEPS POT CONTENTS.  Not in a per-pot item table: lifting
// or smashing a pot runs RevealPotItem (dungeon.c), which walks the room's
// entry in the kDungeonSecrets asset - a list of 3-byte records
// (x, y | 0x20 for the lower region, item) terminated by 0xffff, whose first
// two bytes are compared as one little-endian word against the object's
// dung_object_tilemap_pos.  The matching record's item byte goes into
// dung_secrets_unk1, and Sprite_SpawnSecret turns it into
// kSpawnSecretItems[item - 1] via Sprite_SpawnDynamically.  Item 8 is the one
// that spawns sprite 0xe4, the small key.  A pot with nothing under it has no
// record at all, which is why the wider modes are undeliverable without
// rewriting the asset.
//
// HOW A POT KEY IS NAMED.  PotShuffle.key_drop_data gives a Pot location only
// its ROOM (e.g. 0x37); dump_pots (randomizer_ref/dump_logic.py) resolves the
// pot inside it as the single vanilla_pots entry whose item is PotItem.Key and
// writes its room plus its record word ("pos", straight out of the fork's own
// Pot.pot_data()).  Checked against this engine's kDungeonSecrets asset: the
// whole game holds exactly NINETEEN item==8 records, one in each of exactly
// these nineteen rooms, and every one sits at the word the dump names.  So the
// ROOM alone already identifies a pot key uniquely - the engine matches on the
// room and uses "pos" only as the load-time cross-check (LoadPotTable), the
// way drops.json's "kind" cross-checks the fourteen enemy slots.
//
// WHAT LINK GETS.  The key sprite is spawned exactly as in vanilla (same look,
// same physics); the substitution happens when Link absorbs it, in
// Sprite_HandleAbsorptionByPlayer case 12, where the fill's receipt code goes
// through Link_ReceiveItem instead of the silent link_num_keys++.  A pot the
// fill left holding its own vanilla key answers -1 and takes the untouched
// vanilla path, so nothing gets a receipt popup it did not earn.
//
// Everything here is dead unless the LOADED FILE has POTS = SHUFFLED, its rule
// folder had a pots.json, all nineteen entries agreed with kDungeonSecrets and
// every Pot location in the dumped world is one of them: s_potkeys_live gates
// both engine calls, and the fill is only allowed near a pot at all when the
// same checks passed (Rando_SetPotShuffle, before Rando_FillWorldEx).

#define kPotMax 24
#define kPotKeyCount 19   // the closed set; see the asset cross-check above
static struct {
  char loc[48];   // the reference's name for the location
  uint16 room;    // underworld super-tile the pot stands in
  uint16 pos;     // its kDungeonSecrets record word (x | y << 8)
  int code;       // the fill's receipt code for this slot, or -1
} s_pots[kPotMax];
static int s_pot_n;
static int s_pot_table_ok;   // pots.json is complete and agrees with the asset
static int s_potkeys_live;   // the loaded file shuffles pots AND may use them

// The word of the single item==8 (key) secret record in |room|, or -1 when the
// room has none, has more than one, or the asset is not the shape we expect.
static int RoomKeySecretPos(int room) {
  const uint8 *base = kDungeonSecrets;
  int size = (int)kDungeonSecrets_SIZE;
  if (room < 0 || room >= 320 || size < room * 2 + 2)
    return -1;
  int off = base[room * 2] | (base[room * 2 + 1] << 8);
  int found = -1;
  for (;;) {
    if (off < 0 || off + 2 > size) return -1;
    int pos = base[off] | (base[off + 1] << 8);
    if (pos == 0xffff) return found;
    if (off + 3 > size) return -1;
    if (base[off + 2] == 8) {
      if (found >= 0) return -1;   // two keys in one room: not the closed set
      found = pos;
    }
    off += 3;
  }
}

// Read <dir>/pots.json.  A missing or unreadable file is not an error: the
// folder was built before POTS existed, and the pot keys stay vanilla.
static void LoadPotTable(const char *dir) {
  s_pot_n = 0;
  s_pot_table_ok = 0;
  s_potkeys_live = 0;
  char path[512];
  snprintf(path, sizeof path, "%s/pots.json", dir);
  char *text = ReadAll(path);
  if (!text) return;
  char err[128];
  JsonValue *root = Json_Parse(text, err, sizeof err);
  free(text);
  const JsonValue *arr = root ? Json_Get(root, "pots") : NULL;
  if (!arr || arr->type != JSON_ARRAY) {
    printf("[pots] bad %s%c", path, 10);
    Json_Free(root);
    return;
  }
  for (const JsonValue *v = arr->child; v && s_pot_n < kPotMax; v = v->next) {
    const char *name = Json_AsString(Json_Get(v, "name"));
    const JsonValue *jr = Json_Get(v, "room"), *jp = Json_Get(v, "pos");
    if (!name || !jr || !jp || jr->type != JSON_NUMBER || jp->type != JSON_NUMBER)
      continue;
    snprintf(s_pots[s_pot_n].loc, sizeof s_pots[0].loc, "%s", name);
    s_pots[s_pot_n].room = (uint16)Json_AsInt(jr);
    s_pots[s_pot_n].pos = (uint16)Json_AsInt(jp);
    s_pots[s_pot_n].code = -1;
    s_pot_n++;
  }
  Json_Free(root);

  // Cross-check every entry against the engine's own asset before anything is
  // allowed to depend on it.  One disagreement disables the whole set: a pot
  // the engine would not deliver could swallow a progression item.
  int ok = (s_pot_n == kPotKeyCount);
  if (!ok && s_pot_n)
    printf("[pots] pots.json has %d entries, expected %d%c", s_pot_n, kPotKeyCount, 10);
  for (int i = 0; i < s_pot_n; i++) {
    int p = RoomKeySecretPos(s_pots[i].room);
    if (p < 0 || (uint16)p != s_pots[i].pos) {
      printf("[pots] %s: room %03x key secret is %d, dump says %d - "
             "pot shuffle refused%c", s_pots[i].loc, s_pots[i].room, p, s_pots[i].pos, 10);
      ok = 0;
    }
  }
  s_pot_table_ok = ok;
}

// Every Pot location the dumped world holds has to be one this engine can hand
// out, or the fill must not be allowed to use pots at all.  Called after the
// world is loaded and BEFORE the fill.
static int PotTableCoversWorld(const RandoWorld *w) {
  if (!s_pot_table_ok) return 0;
  for (int i = 0; i < w->n_locations; i++) {
    if (w->locations[i].type != RANDO_LOC_POT) continue;
    int j;
    for (j = 0; j < s_pot_n; j++)
      if (!strcmp(s_pots[j].loc, w->locations[i].name)) break;
    if (j == s_pot_n) {
      printf("[pots] the dump has a pot location pots.json does not name (%s) - "
             "pot shuffle refused%c", w->locations[i].name, 10);
      return 0;
    }
  }
  return 1;
}

// Fill in every pot's receipt code from the gift table the apply loop just
// built (a pot has no chest record, so its code lands there like an NPC gift)
// and arm the engine hooks.  Called at the end of ApplyFillPlacements.
static void ResolvePotCodes(int shuffled) {
  int named = 0;
  for (int i = 0; i < s_pot_n; i++) {
    s_pots[i].code = -1;
    for (int g = 0; g < s_gift_n; g++)
      if (!strcmp(s_gift[g].loc, s_pots[i].loc)) { s_pots[i].code = s_gift[g].code; break; }
    if (s_pots[i].code >= 0) named++;
  }
  s_potkeys_live = shuffled && s_pot_table_ok && s_pot_n > 0;
  if (s_potkeys_live)
    printf("[pots] SHUFFLED: %d pot keys, %d resolved to a receipt%c", s_pot_n, named, 10);
  else if (shuffled)
    printf("[pots] SHUFFLED but this rule folder has no usable pots.json: "
           "pot keys stay vanilla%c", 10);
}

// What the pot that was just opened in this room was holding, remembered until
// Link walks into the key sprite.  Indexed by the SPRITE SLOT the key took, so
// a key left lying around cannot be confused with anything else.
static struct { int room, code; } s_pot_live[16];

// Sprite_SpawnSecret, on the "this secret is a small key" path (item 8 ->
// sprite 0xe4).  |room| is dungeon_room_index, the room whose kDungeonSecrets
// list the key just came out of; that alone names the pot (see above).
void Randomizer_PotKeySpawned(int j, int room) {
  if ((unsigned)j >= 16) return;
  s_pot_live[j].room = -1;
  s_pot_live[j].code = -1;
  // every pot key lives in an underworld room; outdoors the same secret path
  // runs for bushes and rocks and the room index means nothing
  if (!s_potkeys_live || !player_is_indoors) return;
  for (int i = 0; i < s_pot_n; i++) {
    if (s_pots[i].room != (uint16)room) continue;
    s_pot_live[j].room = room;
    s_pot_live[j].code = s_pots[i].code;
    return;
  }
}

// Sprite_HandleAbsorptionByPlayer case 12, after the enemy-drop question came
// back -1: the receipt code to hand Link instead of the vanilla small key, or
// -1 for "do exactly what vanilla does".  A code that IS the vanilla receipt
// returns -1 too, so a pot the fill did not actually change keeps its silent
// vanilla pickup.  The armed slot is consumed either way.
int Randomizer_PotKeyReceipt(int j, int room, int vanilla) {
  if (!s_potkeys_live || (unsigned)j >= 16) return -1;
  if (s_pot_live[j].room != room) return -1;
  int code = s_pot_live[j].code;
  s_pot_live[j].room = -1;
  s_pot_live[j].code = -1;
  if (code < 0 || code == vanilla) return -1;
  printf("[pots] room %03x: item %d (vanilla %d)%c", room, code, vanilla, 10);
  return code;
}

// ---- BONK DROPS = SHUFFLED: the 42 bonk / tree-pull prizes ----------------
//
// WHAT THE REFERENCE HAS.  `bonk_drops` is a BARE FLAG (CLI.py lists it with
// the store_true settings; resources/app/cli/args.json gives it
// {"action": "store_true"}), so RulesBuild sends "--setting bonk_drops".
// With it on, ItemList.create_dynamic_bonkdrop_locations turns the 42 rows of
// Regions.py bonk_prize_table into real slots of LocationType.Bonk and
// add_bonkdrop_contents throws their vanilla contents (bee traps, apples,
// hearts, rupees, bombs, fairies) into the pool; Rules.py gates each one on
// can_collect_bonkdrops = BOOTS or (SWORD and QUAKE), plus Agahnim dead for
// the rows flagged aga_required.  Two of the plain "<region> Bonk Drop"
// farmable events disappear when it is on, which is why a bonk_drops dump has
// 407 locations instead of 367.
//
// The ~40 "<Area> Bonk Drop / Bush Drop / Rock Drop / Tree Pull" events every
// dump already carried are NOT this set - those are the farmable rupee/bomb
// spots (Farmable Bombs / Farmable Rupees, locked and pinned, type Logical).
// The real set is bonk_prize_table, and dump_bonks (randomizer_ref/
// dump_logic.py) writes it to bonks.json on every dump.
//
// WHAT THE ENGINE HAS.  The reference's location address (0x2abb00 + id*6 + 3)
// points into a custom ROM table this engine does not have, so the dump can
// only give the NAME.  The engine coordinates are the table below, derived
// from the engine's own assets.  Every one of these prizes is an ordinary
// overworld sprite of a prize type - 0x79 bee, 0xac apples, 0xd8..0xe3 the
// absorbables - sitting in kOverworldSprites; SpritePrep_OverworldBonkItem /
// SpritePrep_Absorbable start it HIDDEN outdoors (sprite_E != 0, see the
// comment in SpriteDraw_AbsorbableTransient) and the only thing in the whole
// engine that clears sprite_E is Entity_ApplyRumbleToSprites, reached from
// RepelDash (a boots dash that bonks) and from Ancilla1C_QuakeSpell.  That is
// can_collect_bonkdrops exactly, which is the proof that this sprite set is
// the reference's bonk set.
//
// HOW THE SET WAS CLOSED.  Walking kOverworldSpriteOffs / kOverworldSprites
// over all three progress stages finds 44 distinct (area, block) spots whose
// sprite type is a prize type.  42 of them are the table below (41 locations;
// Flute Boy Approach South owns two blocks because the pre-Zelda sprite list
// puts it one 16px column further east).  The two left over are
//   area 0x1a block 0x0577 - the reference's own commented-out
//                            'Forgotten Forest Southeast Tree', and
//   area 0x34 block 0x043e - 'Statues Area', a farmable post-Aga tree pull
//                            (ItemList.post_aga_tree_pulls), never a slot.
// Per-area counts match bonk_prize_table's per-region counts one for one, and
// every row the reference marks aga_required is exactly a sprite that only
// exists in the post-Agahnim stage-2 sprite list - Kakariko Pond, Bonk Rock
// Ledge, River Bend West, Eastern Palace, both Flute Boy trees, both Tree
// Line trees.  Where two rows share an area their names are directional
// (west/east, tree 2/3/4, southwest/central/southeast) and the sprite
// coordinates decide; every pair whose two rows sit in DIFFERENT reference
// regions (Lost Woods Pass West vs East Top, Bonk Rock Ledge vs Sanctuary,
// River Bend vs River Bend East Bank, Qirn Jump vs Qirn Jump East Bank, Dark
// Graveyard vs Dark Graveyard North) is settled by geometry or by the
// post-Agahnim test, so no pair that the logic could tell apart is guessed.
//
// The 42nd row, 'Cold Fairy Statue', is not outdoors at all: its region is
// 'Good Bee Cave (back)'.  That is kDungeonSprites room 0x120, the only room
// in the game holding a Good Bee (0xb2); it also holds two fairies, and the
// reference names one bonk slot there.  Both fairies are armed for that one
// location and whichever Link takes first consumes it, so the item is always
// delivered no matter which of the two the reference meant.
//
// WHAT LINK GETS.  Unlike the pot keys and the key drops, a bonk prize CANNOT
// keep its vanilla sprite: a bee and apples are not absorbable at all, and a
// hidden 0xd8 heart is turned into a live bomb by Entity_ApplyRumbleToSprites
// itself.  So a slot the fill actually changed is respawned as sprite 0xd9,
// the green rupee - the same placeholder the reference writes into the ROM
// sprite table - which hides and reveals exactly the same way, and
// Sprite_HandleAbsorptionByPlayer hands out the fill's receipt instead of the
// rupee.  A slot the fill left holding its own vanilla content is not touched
// at all: either the code IS the vanilla prize's receipt, or there is no code
// because the item has no engine receipt (Bee Trap, Apples, Fairy, Chicken,
// Good Bee, Big Magic, Arrows (5) - all of them junk the reference only ever
// adds to the pool because of this very setting).
//
// ONE SHOT.  An overworld prize respawns every time Link re-enters the screen
// (that is what makes trees farmable), so a delivered bonk drop has to be
// remembered in the SAVE.  save_ow_event_info (SRAM $7EF280) has bits 0x04,
// 0x08, 0x10 and 0x80 free - the engine only ever tests 0x01, 0x02, 0x20 and
// 0x40 - which is also why the reference picked 0x10 / 0x08 / 0x04.  Each
// location owns one of those three bits on its AREA byte (no area holds more
// than three bonk spots); the single indoor slot uses bit 0x80 on byte
// (room & 0x7f) = 0x20, and no outdoor spot ever uses 0x80.
//
// Everything here is dead unless the LOADED FILE has BONK DROPS = SHUFFLED,
// its rule folder had a bonks.json, all 42 names matched the table below and
// every spot in the table was found in the engine's own asset:
// s_bonkdrops_live gates all four engine calls, and the fill is only allowed
// near a bonk slot when the same checks passed (Rando_SetBonkShuffle, before
// Rando_FillWorldEx).

#define kBonkLocCount 42   // the reference's closed set (bonk_prize_table)
#define kBonkMax 48

// area = overworld area index, blk = the sprite_where_in_overworld block the
// engine computes from the sprite record's own y/x bytes in
// Overworld_LoadSprites - which is exactly the value
// Overworld_LoadProximaSpriteIfAlive parks in sprite_N_word[k].
// flag = the save_ow_event_info[area] bit that remembers "already taken".
static const struct { const char *loc; uint8 area; uint16 blk; uint8 flag; } kBonkOwSpots[] = {
  { "Lost Woods Hideout Tree",          0x00, 0x0a59, 0x10 },
  { "Death Mountain Bonk Rocks",        0x05, 0x0704, 0x10 },
  { "Mountain Pass Pull Tree",          0x0a, 0x004e, 0x10 },
  { "Mountain Pass Southeast Tree",     0x0a, 0x05a9, 0x08 },
  { "Lost Woods Pass West Tree",        0x10, 0x00c7, 0x10 },
  { "Kakariko Portal Tree",             0x10, 0x01f7, 0x08 },
  { "Fortune Bonk Rocks",               0x11, 0x0408, 0x10 },
  { "Kakariko Pond Tree",               0x12, 0x01a4, 0x10 },
  { "Bonk Rocks Tree",                  0x13, 0x00c7, 0x10 },
  { "Sanctuary Tree",                   0x13, 0x0198, 0x08 },
  { "River Bend West Tree",             0x15, 0x04a4, 0x10 },
  { "River Bend East Tree",             0x15, 0x01fb, 0x08 },
  { "Blinds Hideout Tree",              0x18, 0x01a8, 0x10 },
  { "Kakariko Welcome Tree",            0x18, 0x0f36, 0x08 },
  { "Forgotten Forest Southwest Tree",  0x1a, 0x048a, 0x10 },
  { "Forgotten Forest Central Tree",    0x1a, 0x041d, 0x08 },
  { "Hyrule Castle Tree",               0x1b, 0x0546, 0x10 },
  { "Wooden Bridge Tree",               0x1d, 0x006b, 0x10 },
  { "Eastern Palace Tree",              0x1e, 0x0e72, 0x10 },
  { "Flute Boy South Tree",             0x2a, 0x048f, 0x10 },
  { "Flute Boy East Tree",              0x2a, 0x0545, 0x08 },
  { "Central Bonk Rocks Tree",          0x2b, 0x01d6, 0x10 },
  { "Tree Line Tree 2",                 0x2e, 0x009c, 0x10 },
  { "Tree Line Tree 4",                 0x2e, 0x01b4, 0x08 },
  // the pre-Zelda sprite list puts this tree one column east of where the
  // later two put it; both blocks are the same location
  { "Flute Boy Approach South Tree",    0x32, 0x0528, 0x10 },
  { "Flute Boy Approach South Tree",    0x32, 0x0529, 0x10 },
  { "Flute Boy Approach North Tree",    0x32, 0x019a, 0x08 },
  { "Dark Lumberjack Tree",             0x42, 0x0466, 0x10 },
  { "Dark Fortune Bonk Rocks (Drop 1)", 0x51, 0x0408, 0x10 },
  { "Dark Fortune Bonk Rocks (Drop 2)", 0x51, 0x0409, 0x08 },
  { "Dark Graveyard West Bonk Rocks",   0x54, 0x00b5, 0x10 },
  { "Dark Graveyard North Bonk Rocks",  0x54, 0x01b9, 0x08 },
  { "Dark Graveyard Tomb Bonk Rocks",   0x54, 0x00ef, 0x04 },
  { "Qirn Jump West Tree",              0x55, 0x04aa, 0x10 },
  { "Qirn Jump East Tree",              0x55, 0x01fb, 0x08 },
  { "Dark Witch Tree",                  0x56, 0x00e4, 0x10 },
  { "Pyramid Tree",                     0x5b, 0x09a7, 0x10 },
  { "Palace of Darkness Tree",          0x5e, 0x0f00, 0x10 },
  { "Dark Tree Line Tree 2",            0x6e, 0x008c, 0x10 },
  { "Dark Tree Line Tree 3",            0x6e, 0x0190, 0x08 },
  { "Dark Tree Line Tree 4",            0x6e, 0x01a4, 0x04 },
  { "Hype Cave Statue",                 0x74, 0x044e, 0x10 },
};
#define kBonkOwSpotCount ((int)(sizeof kBonkOwSpots / sizeof kBonkOwSpots[0]))

// The one indoor slot: the Good Bee Cave (room 0x120) holds two fairies and
// the reference names one bonk location there, so both are armed for it.
static const struct { const char *loc; uint16 room; uint8 slot; } kBonkUwSpots[] = {
  { "Cold Fairy Statue", 0x120, 1 },
  { "Cold Fairy Statue", 0x120, 2 },
};
#define kBonkUwSpotCount ((int)(sizeof kBonkUwSpots / sizeof kBonkUwSpots[0]))
#define kBonkUwFlagByte  (0x120 & 0x7f)   // no outdoor bonk spot uses this bit
#define kBonkUwFlagBit   0x80

static struct {
  char loc[48];   // the reference's name for the location
  int code;       // the fill's receipt code for this slot, or -1
} s_bonks[kBonkMax];
static int s_bonk_n;
static int s_bonk_table_ok;   // bonks.json is complete and agrees with the assets
static int s_bonkdrops_live;  // the loaded file shuffles bonk drops AND may use them
static int s_bonk_ow_loc[kBonkOwSpotCount];   // spot -> s_bonks index
static int s_bonk_uw_loc[kBonkUwSpotCount];

// The vanilla prize sprite types a bonk spot can hold: 0x79 bee, 0xac apples
// and the absorbables 0xd8..0xe3.
static int BonkPrizeSpriteType(int t) {
  return t == 0x79 || t == 0xac || (t >= 0xd8 && t <= 0xe3);
}

// The prize sprite the engine's own kOverworldSprites asset puts at
// (area, blk) in ANY of the three progress stages, or -1 for "not there".
static int OwPrizeSpriteAt(int area, int blk) {
  const uint8 *p = kOverworldSprites;
  int size = (int)kOverworldSprites_SIZE;
  int found = -1;
  if (area < 0 || area >= 144 || (int)(kOverworldSpriteOffs_SIZE / 2) < 144 * 3)
    return -1;
  for (int stage = 0; stage < 3; stage++) {
    int off = kOverworldSpriteOffs[stage * 144 + area];
    for (;;) {
      if (off < 0 || off >= size || p[off] == 0xff || off + 3 > size) break;
      int y = p[off], x = p[off + 1], t = p[off + 2];
      int b = (((x >> 4) + ((y >> 4) << 2)) << 8) | ((x & 0xf) | ((y << 4) & 0xff));
      if (b == blk && BonkPrizeSpriteType(t)) found = t;
      off += 3;
    }
  }
  return found;
}

// The sprite type kDungeonSprites gives LOAD SLOT |slot| of |room|, or -1.
// Slot numbering is Dungeon_LoadSingleSprite's k: overlord records (x >= 0xe0)
// and the 0xe4 "the record before me drops a key" markers take no slot.
static int UwSpriteAt(int room, int slot) {
  const uint8 *p = kDungeonSprites;
  int size = (int)kDungeonSprites_SIZE;
  int off, k = 0;
  if (room < 0 || room >= 320 || (int)(kDungeonSpriteOffs_SIZE / 2) <= room)
    return -1;
  off = kDungeonSpriteOffs[room];
  if (off < 0 || off >= size) return -1;
  off++;   // sort_sprites_setting
  for (;;) {
    if (off >= size || p[off] == 0xff || off + 3 > size) return -1;
    int y = p[off], x = p[off + 1], t = p[off + 2];
    off += 3;
    if (t == 0xe4 && (y == 0xfe || y == 0xfd)) continue;
    if (t != 0xe4 && x >= 0xe0) continue;
    if (k == slot) return t;
    k++;
  }
}

static int BonkIndexOf(const char *name) {
  for (int i = 0; i < s_bonk_n; i++)
    if (!strcmp(s_bonks[i].loc, name)) return i;
  return -1;
}

// Read <dir>/bonks.json.  A missing or unreadable file is not an error: the
// folder was built before BONK DROPS existed, and the prizes stay vanilla.
static void LoadBonkTable(const char *dir) {
  s_bonk_n = 0;
  s_bonk_table_ok = 0;
  s_bonkdrops_live = 0;
  char path[512];
  snprintf(path, sizeof path, "%s/bonks.json", dir);
  char *text = ReadAll(path);
  if (!text) return;
  char err[128];
  JsonValue *root = Json_Parse(text, err, sizeof err);
  free(text);
  const JsonValue *arr = root ? Json_Get(root, "bonks") : NULL;
  if (!arr || arr->type != JSON_ARRAY) {
    printf("[bonks] bad %s%c", path, 10);
    Json_Free(root);
    return;
  }
  for (const JsonValue *v = arr->child; v && s_bonk_n < kBonkMax; v = v->next) {
    const char *name = Json_AsString(Json_Get(v, "name"));
    if (!name) continue;
    snprintf(s_bonks[s_bonk_n].loc, sizeof s_bonks[0].loc, "%s", name);
    s_bonks[s_bonk_n].code = -1;
    s_bonk_n++;
  }
  Json_Free(root);

  // Cross-check the whole thing before anything is allowed to depend on it.
  // One disagreement disables the set: a bonk slot the engine would not hand
  // out could swallow a progression item (seed 1234 with the flag on puts the
  // Hookshot, the Ice Rod, the Cane of Somaria and both boomerangs on trees).
  int ok = (s_bonk_n == kBonkLocCount);
  if (!ok && s_bonk_n)
    printf("[bonks] bonks.json has %d entries, expected %d%c", s_bonk_n, kBonkLocCount, 10);
  for (int s = 0; s < kBonkOwSpotCount; s++) {
    int t = OwPrizeSpriteAt(kBonkOwSpots[s].area, kBonkOwSpots[s].blk);
    s_bonk_ow_loc[s] = BonkIndexOf(kBonkOwSpots[s].loc);
    if (s_bonk_ow_loc[s] < 0 || t < 0) {
      printf("[bonks] %s: area %02x block %04x %s - bonk shuffle refused%c",
             kBonkOwSpots[s].loc, kBonkOwSpots[s].area, kBonkOwSpots[s].blk,
             t < 0 ? "holds no prize sprite" : "is not in bonks.json", 10);
      ok = 0;
    }
  }
  for (int s = 0; s < kBonkUwSpotCount; s++) {
    int t = UwSpriteAt(kBonkUwSpots[s].room, kBonkUwSpots[s].slot);
    s_bonk_uw_loc[s] = BonkIndexOf(kBonkUwSpots[s].loc);
    if (s_bonk_uw_loc[s] < 0 || t != 0xe3) {
      printf("[bonks] %s: room %03x slot %d holds sprite %d, expected 227 - "
             "bonk shuffle refused%c",
             kBonkUwSpots[s].loc, kBonkUwSpots[s].room, kBonkUwSpots[s].slot, t, 10);
      ok = 0;
    }
  }
  // and the other way round: every name the dump knows must have a spot here
  for (int i = 0; i < s_bonk_n && ok; i++) {
    int seen = 0;
    for (int s = 0; s < kBonkOwSpotCount; s++) if (s_bonk_ow_loc[s] == i) seen = 1;
    for (int s = 0; s < kBonkUwSpotCount; s++) if (s_bonk_uw_loc[s] == i) seen = 1;
    if (!seen) {
      printf("[bonks] bonks.json names %s, which this engine cannot place - "
             "bonk shuffle refused%c", s_bonks[i].loc, 10);
      ok = 0;
    }
  }
  s_bonk_table_ok = ok;
}

// Every Bonk location the dumped world holds has to be one this engine can
// hand out, or the fill must not be allowed to use bonk slots at all.
// Called after the world is loaded and BEFORE the fill.
static int BonkTableCoversWorld(const RandoWorld *w) {
  if (!s_bonk_table_ok) return 0;
  for (int i = 0; i < w->n_locations; i++) {
    if (w->locations[i].type != RANDO_LOC_BONK) continue;
    if (BonkIndexOf(w->locations[i].name) < 0) {
      printf("[bonks] the dump has a bonk location bonks.json does not name (%s) - "
             "bonk shuffle refused%c", w->locations[i].name, 10);
      return 0;
    }
  }
  return 1;
}

// Fill in every bonk slot's receipt code from the gift table the apply loop
// just built (a bonk prize has no chest record, so its code lands there like
// an NPC gift) and arm the engine hooks.  Called at the end of
// ApplyFillPlacements.
static void ResolveBonkCodes(int shuffled) {
  int named = 0;
  for (int i = 0; i < s_bonk_n; i++) {
    s_bonks[i].code = -1;
    for (int g = 0; g < s_gift_n; g++)
      if (!strcmp(s_gift[g].loc, s_bonks[i].loc)) { s_bonks[i].code = s_gift[g].code; break; }
    if (s_bonks[i].code >= 0) named++;
  }
  s_bonkdrops_live = shuffled && s_bonk_table_ok && s_bonk_n > 0;
  Randomizer_BonkDropsReset();
  if (s_bonkdrops_live)
    printf("[bonks] SHUFFLED: %d bonk drops, %d resolved to a receipt%c", s_bonk_n, named, 10);
  else if (shuffled)
    printf("[bonks] SHUFFLED but this rule folder has no usable bonks.json: "
           "bonk drops stay vanilla%c", 10);
}

// The engine receipt a vanilla prize sprite already stands for, so a slot the
// fill left holding its own content keeps the untouched vanilla sprite.  The
// bee, the apples, the fairy, the heart (which the rumble turns into a bomb),
// the big magic and the 5-arrow refill have no engine receipt at all, so
// nothing can ever "equal vanilla" for them - those slots are only ever left
// alone because the fill's item has no code either.
static int BonkVanillaReceipt(int type) {
  switch (type) {
  case 0xd9: return 0x34;   // Rupee (1)
  case 0xda: return 0x35;   // Rupees (5)
  case 0xdb: return 0x36;   // Rupees (20)
  case 0xdc: return 0x27;   // Single Bomb
  case 0xdd: return 0x28;   // Bombs (3), the engine's receipt for a small pile
  case 0xde: return 0x31;   // Bombs (10), ditto for a big one
  case 0xdf: return 0x45;   // Small Magic
  case 0xe2: return 0x44;   // Arrows (10)
  default:   return -1;
  }
}

// The armed sprite slots.  tag_a is the overworld area, or -1 for the one
// indoor slot (and then tag_b is the room, not the block).
static struct { int loc, tag_a, tag_b, flag_byte, flag_bit, code; } s_bonk_live[16];

void Randomizer_BonkDropsReset(void) {
  for (int i = 0; i < 16; i++) {
    s_bonk_live[i].loc = -1;
    s_bonk_live[i].tag_a = -1;
    s_bonk_live[i].tag_b = -1;
    s_bonk_live[i].code = -1;
  }
}

// Shared tail of the two spawn hooks: arm slot |k| unless the save says this
// location is already taken or the fill left it vanilla.  Returns 1 when the
// caller must respawn the prize as the green-rupee placeholder.
static int BonkArm(int k, int loc, int tag_a, int tag_b, int flag_byte, int flag_bit,
                   int vanilla_type) {
  int code;
  if (loc < 0 || loc >= s_bonk_n) return 0;
  if (save_ow_event_info[flag_byte] & flag_bit) return 0;
  code = s_bonks[loc].code;
  if (code < 0 || code == BonkVanillaReceipt(vanilla_type)) return 0;
  s_bonk_live[k].loc = loc;
  s_bonk_live[k].tag_a = tag_a;
  s_bonk_live[k].tag_b = tag_b;
  s_bonk_live[k].flag_byte = flag_byte;
  s_bonk_live[k].flag_bit = flag_bit;
  s_bonk_live[k].code = code;
  return 1;
}

// Overworld_LoadProximaSpriteIfAlive, right after the sprite took slot |k|:
// |blk| is the sprite_where_in_overworld block it came from (and the value
// vanilla leaves in sprite_N_word[k]), |type| the vanilla sprite id.
int Randomizer_BonkDropSpawned(int k, int area, int blk, int type) {
  if ((unsigned)k >= 16) return 0;
  s_bonk_live[k].loc = -1;
  s_bonk_live[k].code = -1;
  if (!s_bonkdrops_live || player_is_indoors) return 0;
  if (area < 0 || area >= 0x80 || !BonkPrizeSpriteType(type)) return 0;
  for (int s = 0; s < kBonkOwSpotCount; s++) {
    if (kBonkOwSpots[s].area != (uint8)area || kBonkOwSpots[s].blk != (uint16)blk)
      continue;
    return BonkArm(k, s_bonk_ow_loc[s], area, blk, area, kBonkOwSpots[s].flag, type);
  }
  return 0;
}

// Dungeon_LoadSingleSprite, right after the sprite took load slot |k|.
int Randomizer_BonkDropSpawnedUW(int k, int room, int slot, int type) {
  if ((unsigned)k >= 16) return 0;
  s_bonk_live[k].loc = -1;
  s_bonk_live[k].code = -1;
  if (!s_bonkdrops_live) return 0;
  for (int s = 0; s < kBonkUwSpotCount; s++) {
    if (kBonkUwSpots[s].room != (uint16)room || kBonkUwSpots[s].slot != (uint8)slot)
      continue;
    if (type != 0xe3) return 0;
    return BonkArm(k, s_bonk_uw_loc[s], -1, room, kBonkUwFlagByte, kBonkUwFlagBit, type);
  }
  return 0;
}

// Sprite_HandleAbsorptionByPlayer: the receipt code to hand Link instead of
// the vanilla pickup, or -1 for "do exactly what vanilla does".  A sprite slot
// is recycled constantly, so the armed slot is verified against where Link
// actually is (sprite_N_word is the engine's own "which overworld block did I
// come from") before anything is handed out, and is then consumed - in the
// save as well, or the tree would grow the item back on the next screen
// transition.
int Randomizer_BonkDropReceipt(int k) {
  if (!s_bonkdrops_live || (unsigned)k >= 16) return -1;
  int code = s_bonk_live[k].code, loc = s_bonk_live[k].loc;
  if (code < 0 || loc < 0) return -1;
  if (s_bonk_live[k].tag_a >= 0) {
    if (player_is_indoors || BYTE(overworld_area_index) != s_bonk_live[k].tag_a ||
        sprite_N_word[k] != (uint16)s_bonk_live[k].tag_b)
      return -1;
  } else {
    if (!player_is_indoors || (int)dungeon_room_index2 != s_bonk_live[k].tag_b)
      return -1;
  }
  save_ow_event_info[s_bonk_live[k].flag_byte] |= (uint8)s_bonk_live[k].flag_bit;
  // one prize per location, even while two sprite slots are armed for it
  for (int i = 0; i < 16; i++)
    if (s_bonk_live[i].loc == loc) {
      s_bonk_live[i].loc = -1;
      s_bonk_live[i].code = -1;
    }
  printf("[bonks] %s: item %d%c", s_bonks[loc].loc, code, 10);
  return code;
}

const char *Randomizer_CurrentDungeonName(void) {
  switch (cur_palace_index_x2) {
  case 0x04: return "Eastern Palace";
  case 0x06: return "Desert Palace";
  case 0x0a: return "Swamp Palace";
  case 0x0c: return "Palace of Darkness";
  case 0x0e: return "Misery Mire";
  case 0x10: return "Skull Woods";
  case 0x12: return "Ice Palace";
  case 0x14: return "Tower of Hera";
  case 0x16: return "Thieves' Town";
  case 0x18: return "Turtle Rock";
  default: return "";
  }
}

// ---- keysanity: dungeon-targeted key / big key / map / compass ----------
// The fill writes the codes 0x4C..0x7F (the code-range comment in
// src/rando/rando_fill.c owns the layout) into chest records and into the
// gift table whenever the loaded file shuffles that class ANYWHERE.  The
// engine end is two calls:
//
//   Link_ReceiveItem (player.c) -> Randomizer_DungeonItemBegin
//       decodes the code to the PLAIN vanilla receipt (0x24 small key,
//       0x32 big key, 0x33 map, 0x25 compass) and remembers the dungeon,
//       so the receipt Link sees - graphics, pose, sound, message - is
//       byte-for-byte the vanilla pickup.
//   AncillaAdd_ItemReceipt (misc.c) -> Randomizer_DungeonItemGrant
//       does the actual SRAM write for the remembered dungeon instead of
//       the vanilla "whatever palace Link is standing in" write.
//
// Dungeon numbering is the game's palace index (cur_palace_index_x2 / 2):
//   0 Sewers            1 Hyrule Castle     2 Eastern Palace
//   3 Desert Palace     4 Castle Tower      5 Swamp Palace
//   6 Palace of Darkness 7 Misery Mire      8 Skull Woods
//   9 Ice Palace       10 Tower of Hera    11 Thieves' Town
//  12 Turtle Rock      13 Ganons Tower
// The reference's "Escape" is Hyrule Castle AND the sewers, and the engine
// runs those as palaces 1 and 0 sharing key slot 0 (SaveDungeonKeys folds
// palace 1 onto 0), so an Escape item credits key slot 0 and sets BOTH
// map/compass/big-key bits - otherwise the HUD would show the map in only
// one half of the same dungeon, depending on which bit the fill happened
// to pick.
//
// Small keys are a counter, not a bit: link_num_keys (0xF36F) is only the
// CURRENT dungeon's count, saved back into link_keys_earned_per_dungeon
// (0xF37C..) on every transition.  A key for the dungeon Link is in has to
// go to the live counter (and is mirrored into the array so a crash-free
// exit cannot lose it); a key for any other dungeon goes straight to that
// dungeon's array slot.  (SMALL KEYS = UNIVERSAL never gets here - it emits
// no dungeon-targeted codes at all; see g_rando_universal_keys below.)
static int s_pending_dungeon = -1;   // 1..13 while a decoded code is in flight
static int s_pending_plain;          // the vanilla receipt it decoded to
static int s_dungeon_codes_live;     // this file's tables hold such codes

// The key-counter slot for a palace index: SaveDungeonKeys (dungeon.c) maps
// Hyrule Castle (1) onto the sewers' slot 0, everything else is its own.
static int DungeonKeySlot(int dungeon) { return dungeon == 1 ? 0 : dungeon; }

// The map/compass/big-key bit for a palace index; the engine's own writes
// use 0x8000 >> palace_index (misc.c, hud.c, dungeon.c kUpperBitmasks).
// Hyrule Castle and the sewers get both bits - see above.
static uint16 DungeonItemBit(int dungeon) {
  return (dungeon <= 1) ? (uint16)0xc000 : (uint16)(0x8000 >> dungeon);
}

// Link_ReceiveItem's first act: turn a dungeon-targeted code into the plain
// vanilla receipt and remember who it is for.  Every other item passes
// through untouched (and clears any stale target).
// Extra codes above the vanilla receipts (chests take them because only 0xff
// means "no item" now): 0x80 = Magic Upgrade (1/2). It wears the shop
// heart's receipt (0x42) for the pickup pose; misc.c applies the upgrade and
// ancilla.c skips the heart while the flag is set.
// 0x81..0x85 are the bomb and arrow CAPACITY upgrades, which the pool really
// contains (shopsanity puts the Capacity Upgrade shop's stock in the item
// shuffle, and `bombbag` adds two "Bomb Upgrade (+10)" copies).  The fork's
// own codes for them - 0x51..0x54 - sit inside the dungeon-item range
// 0x4C..0x7F this file reserves, so they get extra codes instead.  They ride
// in on the plain bomb / arrow refill receipts for the pickup pose, sound and
// message (the vanilla engine has no capacity receipt at all: the only
// capacity path is the happiness pond writing link_bomb_upgrades directly,
// see Sprite_HappinessPond), and the capacity itself is applied below.
//
// The engine stores capacity as a LEVEL 0..7 into kMaxBombsForLevel
// {10,15,20,25,30,35,40,50} / kMaxArrowsForLevel {30,35,40,45,50,55,60,70},
// not as a number of bombs, so "+5" is one level and "+10" is two.  That is
// exact everywhere except the top of each table (level 6 -> 7 is a +10 step),
// where the clamp makes a late upgrade generous rather than short.
//
// 0x86 = "Silver Arrows" and 0x87 = "Triforce Piece" join them for the same
// reason (fork codes 0x58 and 0x6c are both inside 0x4C..0x7F); see
// Randomizer_TakeBowUpgrade and Randomizer_TakeTriforcePiece below.
static int s_pending_halfmagic;
static int s_pending_capacity;   // 0 = none, else the 0x81..0x85 code
static int s_pending_silvers;    // the 0x86 silver arrows are in flight
static int s_bow_before;         // link_item_bow as it was when they were
static int s_pending_triforce;   // an 0x87 triforce piece is in flight
int Randomizer_ExtraItemBegin(int item) {
  s_pending_halfmagic = 0;
  s_pending_capacity = 0;
  s_pending_silvers = 0;
  s_pending_triforce = 0;
  if (item == 0x80) { s_pending_halfmagic = 1; return 0x42; }
  if (item >= 0x81 && item <= 0x82) { s_pending_capacity = item; return 0x31; }  // bombs: "Bombs (10)" look
  if (item >= 0x83 && item <= 0x85) { s_pending_capacity = item; return 0x44; }  // arrows: "Arrows (10)" look
  // SILVER ARROWS: the engine's own silver-bow receipt 0x3b for the pose, the
  // sprite (kReceiveItemGfx[0x3b] = 0x2a, the silver bow) and the sound.  It
  // writes link_item_bow = 3 on its own; TakeBowUpgrade corrects that below.
  if (item == 0x86) { s_pending_silvers = 1; s_bow_before = link_item_bow; return 0x3b; }
  // TRIFORCE PIECE: 0x42, the receipt that writes nothing at all in the
  // vanilla engine (see the half-magic note above), with its heart-refill
  // completion suppressed in ancilla.c and its sprite swapped in misc.c.
  if (item == 0x87) { s_pending_triforce = 1; return 0x42; }
  return item;
}
int Randomizer_TakeHalfMagic(void) { return s_pending_halfmagic; }
void Randomizer_HalfMagicDone(void) { s_pending_halfmagic = 0; }

// ---- SILVER ARROWS (reference `bow_mode=silvers`) ------------------------
// The reference splits the bow into two pool items, "Bow" and "Silver
// Arrows", and they can be found in either order.  link_item_bow is the only
// place the vanilla engine records either (1 bow / 2 bow+arrows / 3 silver /
// 4 silver+arrows), so it cannot express "silvers but no bow"; link_has_silvers
// (0x7EF4E2, in the save block) does, and this reconciles the two on every
// receipt that touches the bow:
//   * the 0x86 pickup: the 0x3b receipt has just written bow = 3, which would
//     hand a bowless player a working bow - so put the byte back to 0 when
//     there was no bow, and to the silver tier when there was.
//   * a later plain Bow receipt (0x0b, which writes bow = 1): upgrade it
//     straight to the silver tier, because the silvers are already found.
// Odd/even is the engine's own "no arrows / has arrows" encoding (hud.c).
void Randomizer_TakeBowUpgrade(int j) {
  if (s_pending_silvers) {
    s_pending_silvers = 0;
    link_has_silvers = 1;
    link_item_bow = s_bow_before ? (link_num_arrows ? 4 : 3) : 0;
    Hud_RefreshIcon();
    return;
  }
  if (j == 0x0b && link_has_silvers && link_item_bow && link_item_bow < 3) {
    link_item_bow = link_num_arrows ? 4 : 3;
    Hud_RefreshIcon();
  }
}

// ---- TRIFORCE HUNT (reference `goal=triforcehunt`) -----------------------
// The reference puts 30 "Triforce Piece" items in the pool and ends the game
// at 20 of them (ItemList.set_default_triforce: goal 20, total 30; neither is
// exposed as a settings row here, and the port offers no `customitemarray`,
// so the pair is fixed).  Its own win is "carry the pieces to Murahdahla in
// the Hyrule Castle courtyard" (Rules.py adds the count rule to that NPC, and
// Rom.py writes 0x180194 = 1, 'must turn in'); this port has no Murahdahla
// sprite, so it implements the reference's OTHER documented triforce-hunt
// ending - the instant win, 0x180194 = 0 - and ends the game the moment the
// count reaches the goal.  Called once per piece, from the end of the receipt
// animation in ancilla.c.
#define kRandoTriforceGoalIndex 4     // kV_goal[4] == "triforcehunt"
#define kRandoTriforcePieces    20    // set_default_triforce's goal for it
static int s_win_pending;
int Randomizer_TriforceGoal(void) {
  if (!Randomizer_FileEnabled())
    return 0;
  if (Randomizer_FileOptValue("goal") != kRandoTriforceGoalIndex)
    return 0;
  return kRandoTriforcePieces;
}
int Randomizer_PendingTriforcePiece(void) { return s_pending_triforce; }
int Randomizer_TakeTriforcePiece(void) {
  int goal;
  if (!s_pending_triforce)
    return 0;
  s_pending_triforce = 0;
  if (link_triforce_pieces < 255)
    link_triforce_pieces++;
  goal = Randomizer_TriforceGoal();
  printf("[randomizer] triforce piece %d%s\n", link_triforce_pieces,
         goal ? (link_triforce_pieces >= goal ? " - GOAL" : "") : " (goal is not TRIFORCE)");
  if (goal && link_triforce_pieces >= goal)
    s_win_pending = 1;
  return 1;
}

// The win itself.  Entering the triforce room is exactly what the vanilla
// engine does when Link walks north out of dungeon room 0 (dungeon.c
// Dungeon_StartInterRoomTrans_Up), so the three module writes are copied from
// there.  Deferred to Module_MainRouting rather than fired inside the item
// receipt: the receipt still owns Link, an ancilla slot and the submodule.
// Only from normal dungeon (7) and overworld (9, and 11 = the special areas,
// which run the same handler) play, only with the submodule idle and nothing
// holding Link, so it can never cut into a transition, a menu, a text box or
// the receipt animation itself.
void Randomizer_CheckPendingWin(void) {
  if (!s_win_pending)
    return;
  if (main_module_index != 7 && main_module_index != 9 && main_module_index != 11)
    return;
  if (submodule_index != 0 || flag_is_link_immobilized || item_receipt_method != 0)
    return;
  s_win_pending = 0;
  main_module_index = 25;
  submodule_index = 0;
  subsubmodule_index = 0;
}

uint8 g_rando_bomb_bag;
uint8 g_rando_retro;

// ---- HEART COLOR (reference `heartcolor`) --------------------------------
// Presentation-only (see the RandoOpt comment): Palette_Load_HUD (load_gfx.c)
// reloads the HUD's BG palette 1 into aux_palette_buffer[16..31] (offset by
// overworld_palette_aux_or_main, same as Palette_LoadMultiple uses) every
// time the HUD palette is (re)uploaded; the heart tiles (hud.c
// kHudItemBoxTab1/2, tile words 0x24A0-0x24A2) carry palette bits 001 = BG
// palette 1.  RED (index 0) is vanilla and this never touches the buffer for
// it.  The other three colors are this port's own BGR555 picks - which exact
// word of that 16-word row is the heart's fill color could not be confirmed
// against a real ROM (none ships in this worktree) or a live boot (this is a
// compile-only sandbox), so word 2 (the usual mid-tone fill slot of a
// transparent/outline/fill/highlight 4bpp sprite) is a best-effort guess -
// see the tester checklist.
uint8 g_rando_heart_color;   // 0 red (vanilla, no-op) 1 blue 2 green 3 yellow
static const uint16 kHeartColorBGR555[3] = { 0x7D08, 0x1364, 0x0B7F };  // blue, green, yellow
void Randomizer_ApplyHeartColor(void) {
  if (!g_rando_heart_color)
    return;
  uint16 c = kHeartColorBGR555[g_rando_heart_color - 1];
  int idx = (overworld_palette_aux_or_main >> 1) + 16 + 2;
  aux_palette_buffer[idx] = c;
  main_palette_buffer[idx] = c;
  flag_update_cgram_in_nmi += 1;
}

// ---- HEART BEEP (reference `heartbeep`) ----------------------------------
// hud.c reloads link_lowlife_countdown_timer_beep with this many frames
// minus one every time the low-health beep fires (32 is vanilla).  OFF
// reports 0, which the caller reads as "never play the sound effect" while
// still reloading the normal-speed timer underneath, so turning the row back
// on mid-file needs no extra state.
int Randomizer_HeartBeepInterval(void) {
  if (!Randomizer_FileEnabled())
    return 32;
  switch (Randomizer_FileOptValue("heartbeep")) {
    case 1: return 64;    // half
    case 2: return 128;   // quarter
    case 3: return 0;     // off
    case 4: return 16;    // double (Rom.py 0x180033 = 0x10)
    default: return 32;   // normal (vanilla)
  }
}

// ---- MENU SPEED (reference `fastmenu`) -----------------------------------
// hud.c's Hud_BringMenuDown / Hud_CloseMenu step BG3VOFS_copy2 by 8 a frame
// and stop on an exact match (0xff18 open, 0 closed) - that exact-equality
// guard is vanilla's own way of knowing the slide finished, and 232 (the
// full throw) is not a multiple of every candidate step, so this never
// changes the step size itself (that could overshoot the target and hang the
// menu open forever).  Instead it says how many 8-unit steps hud.c should
// apply in one frame: >1 speeds the slide up, "half" applies a step on only
// every other call (see the toggle in hud.c), "instant" applies enough steps
// (30) to always finish the full 232-unit throw in a single frame.
int Randomizer_MenuSpeedSteps(void) {
  if (!Randomizer_FileEnabled())
    return 1;
  switch (Randomizer_FileOptValue("menuspeed")) {
    case 1: return 30;   // instant
    case 2: return 2;    // double
    case 3: return 3;    // triple
    case 4: return 4;    // quadruple
    case 5: return -1;   // half - hud.c special-cases this (skip every other call)
    default: return 1;   // normal (vanilla)
  }
}

// ---- MUSIC (reference `disablemusic`) ------------------------------------
// Presentation-only: gates the music-track request funnel in nmi.c's
// Interrupt_NMI_AudioParts_Locked, which substitutes the engine's existing
// "pause spc player" control byte (0xf0, already used for MSU pausing - see
// audio.c's ZeldaPlayMsuAudioTrack) for every real track/fade request while
// this is on, leaving sound_effect_1/2/ambient (SFX) untouched.
int Randomizer_MusicDisabled(void) {
  return Randomizer_FileEnabled() && Randomizer_FileOptValue("disablemusic") == 1;
}

// ---- FLASHING (reference `reduce_flashing`) ------------------------------
// Presentation-only: ORs the engine's existing global DimFlashes feature bit
// (features.h kFeatures0_DimFlashes, normally a zelda3.ini-only setting) into
// the running feature set for a file with this row on REDUCED - see the
// enhanced_features0 resync in zelda_rtl.c. It only ever ADDS the bit on top
// of whatever the global config already says, so a file at NORMAL (index 0,
// the reference default) never changes anything - a global DimFlashes=1
// still dims every file exactly as it does today. The bit already dims
// Filter_Majorly_Whiten_Color's screen-whiten amount (load_gfx.c): Ether's
// direct call and the shared HandleScreenFlash used by Bombos/Quake, the
// Agahnim lightning bolts (sprite_main.c) and the maiden-crystal warp, plus
// the intro cutscene's sword flash (ending.c).
int Randomizer_FlashingReduced(void) {
  return Randomizer_FileEnabled() && Randomizer_FileOptValue("reduce_flashing") == 1;
}

int Randomizer_MaxBombs(void) {
  // BOMB BAG: no bag, no capacity - Link cannot hold a single bomb, which is
  // what the reference does by starting the file at 0 bomb capacity
  // (InitialSram.py: starting_bomb_cap_upgrades = 10 if not bombbag else 0).
  if (g_rando_bomb_bag && !link_has_bomb_bag)
    return 0;
  return kMaxBombsForLevel[link_bomb_upgrades & 7];
}

void Randomizer_TakeCapacityUpgrade(void) {
  int code = s_pending_capacity;
  if (!code)
    return;
  s_pending_capacity = 0;              // one receipt, one upgrade
  if (code <= 0x82) {
    int steps = (code == 0x82) ? 2 : 1;
    // BOMB BAG: the FIRST upgrade is the bag itself and only unlocks the
    // vanilla ten bombs; the second (the pool holds exactly two) raises the
    // level, so a bomb-bag file ends at the same 20 the reference gives.
    if (g_rando_bomb_bag && !link_has_bomb_bag) {
      link_has_bomb_bag = 1;
      steps = 0;
    }
    int lvl = link_bomb_upgrades + steps;
    link_bomb_upgrades = (uint8)(lvl > 7 ? 7 : lvl);
    link_bomb_filler = kMaxBombsForLevel[link_bomb_upgrades];   // fill it, like the pond does
  } else {
    // 0x85 = "Arrow Upgrade (+70)", the reference's one-shot full quiver
    int lvl = (code == 0x85) ? 7 : link_arrow_upgrades + (code == 0x84 ? 2 : 1);
    link_arrow_upgrades = (uint8)(lvl > 7 ? 7 : lvl);
    link_arrow_filler = kMaxArrowsForLevel[link_arrow_upgrades];
  }
  Hud_RefreshIcon();
}

int Randomizer_DungeonItemBegin(int item) {
  static const int kPlain[4] = { 0x24, 0x32, 0x33, 0x25 };
  s_pending_dungeon = -1;
  // Vanilla chest bytes are all < 0x4C - the engine's receipt tables have
  // exactly 76 entries and vanilla indexes them unchecked - so this range
  // can only hold codes the fill wrote.  The flag keeps even that much off
  // the vanilla path.
  if (!s_dungeon_codes_live || !Rando_DungeonItemIsCode(item))
    return item;
  s_pending_dungeon = Rando_DungeonItemDungeon(item);
  s_pending_plain = kPlain[Rando_DungeonItemClass(item)];
  return s_pending_plain;
}

// AncillaAdd_ItemReceipt's grant for j in {0x24, 0x25, 0x32, 0x33}: 1 when
// this one was a dungeon-targeted pickup and has been credited here, 0 when
// the caller should do its normal "current dungeon" write.
int Randomizer_DungeonItemGrant(int j) {
  int d = s_pending_dungeon, slot, cur, cur_slot;
  if (d < 0 || j != s_pending_plain)
    return 0;
  s_pending_dungeon = -1;              // one receipt, one grant
  switch (j) {
  case 0x24:                           // small key -> that dungeon's counter
    slot = DungeonKeySlot(d);
    cur = (uint8)cur_palace_index_x2;
    cur_slot = (cur == 0xff) ? -1 : DungeonKeySlot(cur >> 1);
    if (cur_slot == slot) {
      if (link_num_keys < 99)
        link_num_keys++;
      link_keys_earned_per_dungeon[slot] = link_num_keys;
    } else if (link_keys_earned_per_dungeon[slot] < 99) {
      link_keys_earned_per_dungeon[slot]++;
    }
    return 1;
  case 0x32: link_bigkey |= DungeonItemBit(d); return 1;
  case 0x33: link_dungeon_map |= DungeonItemBit(d); return 1;
  case 0x25: link_compass |= DungeonItemBit(d); return 1;
  default: return 0;
  }
}

// ---- SMALL KEYS = UNIVERSAL -------------------------------------------
//
// The reference (keyshuffle=universal) drops the per-dungeon key counters
// entirely: it pools ~19 nameless "Small Key (Universal)" items and every
// small-key door in the game takes one from that single pool, wherever it
// was found.  The engine has no such pool: link_num_keys (0xF36F) is the
// CURRENT dungeon's count, reloaded from link_keys_earned_per_dungeon on
// every dungeon entry and blanked to 0xff on the overworld, so a key found
// outside a dungeon used to be thrown away and one found inside only ever
// opened doors in that dungeon.
//
// The fix keeps link_num_keys as the ONE live counter for the whole game -
// it is simply never reloaded per dungeon and never blanked - so every
// vanilla site that hands out or spends a key (the 0x24 receipt in misc.c,
// the pot/enemy key drop in sprite.c, the bonk key in sprite_main.c, the
// small-key door in dungeon.c, even the twitch "givekey" verb) keeps
// working untouched.  What is added is the persistent copy: 0xF38B, the
// byte the reference itself uses (see link_num_keys_universal in
// variables.h), because link_num_keys alone would come back as 0xff from a
// file saved on the overworld.  Hold() is the only thing that touches it:
// it moves the value in whichever direction is meaningful at that site.
uint8 g_rando_universal_keys;

void Randomizer_UniversalKeysHold(void) {
  if (link_num_keys == 0xff)
    link_num_keys = link_num_keys_universal;   // arriving from a blanked state
  else
    link_num_keys_universal = link_num_keys;   // keep the saved pool current
}

// ---- DUNGEON COUNT / COLLECT RATE ---------------------------------------
//
// Both rows need the same one question answered at run time: of the chests
// this seed filled, which have been opened?  The engine already knows: a
// dungeon room's save word (save_dung_info[room], 0x7EF000..) carries one
// "chest taken" bit per chest of the room, and dungeon.c's chest code
// (kChestOpenMasks {0x100,0x200,...} in the LIVE dung_savegame_state_bits,
// which Dung_SaveDataForCurrentRoom writes back shifted down by 4) indexes
// them by the chest's position among that room's records in the
// kDungeonRoomChests scan order.  So a chest record is opened iff
//     save_dung_info[room] & (0x10 << slot)
// with |slot| its 0-based position among the records of the same room.
//
// The apply loop below records that pair for every placement it writes, plus
// which dungeon the placement's LOCATION NAME says it belongs to, so the HUD
// can filter by the palace Link is standing in.  Only chest placements are
// tracked: a boss drop, pedestal, NPC or shop slot has no room flag the
// engine could read back, so counting them would only ever be wrong.
#define kTrackedMax 256
static struct { uint16 room; uint8 slot; uint8 palace; } s_tracked[kTrackedMax];
static int s_tracked_n;

// Location-name prefix -> palace index, the engine's 0..13 numbering (the one
// DungeonItemBit and cur_palace_index_x2 >> 1 use).  These are LOCATION
// prefixes from the dump ("Thieves' Town - Big Chest"), which differ from the
// ITEM names in kRandoDungeonNames ("Thieves Town"): the fork writes Hyrule
// Castle and the Sewers as two prefixes of the one physical dungeon and calls
// Agahnim's tower "Castle Tower" here.  Sewers/Hyrule Castle both map to 1
// and the lookup folds palace 0 onto 1, exactly as DungeonKeySlot does.
static const struct { const char *prefix; uint8 palace; } kLocDungeonPrefix[] = {
  { "Sewers - ", 1 },              { "Hyrule Castle - ", 1 },
  { "Eastern Palace - ", 2 },      { "Desert Palace - ", 3 },
  { "Castle Tower - ", 4 },        { "Swamp Palace - ", 5 },
  { "Palace of Darkness - ", 6 },  { "Misery Mire - ", 7 },
  { "Skull Woods - ", 8 },         { "Ice Palace - ", 9 },
  { "Tower of Hera - ", 10 },      { "Thieves' Town - ", 11 },
  { "Turtle Rock - ", 12 },        { "Ganons Tower - ", 13 },
};
static int PalaceOfLocation(const char *name) {
  for (size_t i = 0; i < sizeof kLocDungeonPrefix / sizeof kLocDungeonPrefix[0]; i++)
    if (!strncmp(name, kLocDungeonPrefix[i].prefix, strlen(kLocDungeonPrefix[i].prefix)))
      return kLocDungeonPrefix[i].palace;
  return 0;   // not in a dungeon
}
static int TrackedIsOpen(int i) {
  return (save_dung_info[s_tracked[i].room] & (uint16)(0x10 << s_tracked[i].slot)) != 0;
}

int Randomizer_DungeonItemsLeft(void) {
  // cheapest tests first: this runs from Hud_Update_Inventory, once a frame
  if (!s_tracked_n)                       // vanilla boot, or the fill wrote nothing
    return -1;
  int cur = (uint8)cur_palace_index_x2;
  if (cur == 0xff)                        // not in a dungeon
    return -1;
  int mode = Randomizer_FileEnabled() ? Randomizer_FileOptValue("counters") : 2;
  if (mode == 2)                          // row OFF
    return -1;
  int p = cur >> 1;
  if (p == 0) p = 1;                      // sewers count as Hyrule Castle
  if (p > 13) return -1;
  if (mode == 0) {
    // NORMAL = what the reference's own default resolves to for the profiles
    // this port dumps (no door / drop / pottery shuffle, Rom.py map_hud_mode
    // and compass_mode): show on pickup once a map or compass can turn up
    // outside its dungeon, and never under universal keys.
    // ANYWHERE only: Rom.py's map_hud_mode / compass_mode both test
    // "not in ['none', 'nearby']", so a NEARBY map or compass leaves the HUD
    // counter off just as OWN DUNGEON does.
    if (g_rando_universal_keys ||
        (Randomizer_FileOptValue("maps") != 1 && Randomizer_FileOptValue("compasses") != 1))
      return -1;
    mode = 3;
  }
  if (mode == 3 && !(link_compass & DungeonItemBit(p)))   // PICKUP: needs the compass
    return -1;
  int left = 0;
  for (int i = 0; i < s_tracked_n; i++)
    if (s_tracked[i].palace == p && !TrackedIsOpen(i))
      left++;
  return left;
}

int Randomizer_CollectionRate(int *got, int *total) {
  if (!Randomizer_FileEnabled() || Randomizer_FileOptValue("collectrate") != 1 || !s_tracked_n)
    return 0;
  int g = 0;
  for (int i = 0; i < s_tracked_n; i++)
    if (TrackedIsOpen(i))
      g++;
  *got = g;
  *total = s_tracked_n;
  return 1;
}

// chest record -> the fill's item (bets on chest contents, feed lines)
#define kChestInfoMax 512
static struct { char item[32]; uint8 prog, known; } s_chest_info[kChestInfoMax];
int Randomizer_ChestRecordInfo(int rec, char *item, size_t cap, int *progression) {
  if (rec < 0 || rec >= kChestInfoMax || !s_chest_info[rec].known) return 0;
  if (item) snprintf(item, cap, "%s", s_chest_info[rec].item);
  if (progression) *progression = s_chest_info[rec].prog;
  return 1;
}

static int ApplyFillPlacements(int seed, const char *dump_dir, FILE *lf,
                               const char *pin_location,
                               const char *pin_item) {
  RandoWorld world;
  memset(s_chest_info, 0, sizeof s_chest_info);
  s_tracked_n = 0;
  // keysanity: tell the name->code mapping which of the four dungeon-item
  // classes this file shuffles ANYWHERE, so those placements get the
  // dungeon-targeted codes instead of the plain "current dungeon" ones.
  int ks_keys = FillOptValue("keys");
  // NEARBY (index 3 on SMALL KEYS, index 2 on the other three) puts a dungeon
  // item OUTSIDE its home dungeon exactly as ANYWHERE does - the reference
  // only narrows WHERE to that dungeon's own district - so it needs the same
  // dungeon-targeted receipt code.  UNIVERSAL (2 on SMALL KEYS) still does
  // not: it is one nameless shared key and keeps the plain 0x24.
  Rando_SetKeysanity(ks_keys == 3 ? 1 : ks_keys,
                     FillOptValue("bigkeys") ? 1 : 0,
                     FillOptValue("maps") ? 1 : 0,
                     FillOptValue("compasses") ? 1 : 0);
  s_dungeon_codes_live = 0;
  // KEY DROPS = SHUFFLED: let the fill fill the reference's fourteen enemy
  // key drops, and read the (room, sprite slot) table the engine needs to
  // recognise them.  LoadDropTable also clears s_keydrops_live, so every
  // early return below leaves the enemy drops vanilla.
  int keydrops = FillOptValue("keydrops") == 1;
  Rando_SetDropShuffle(keydrops);
  LoadDropTable(dump_dir);
  // POTS = SHUFFLED: same story for the nineteen pot keys, except that the
  // table is cross-checked against kDungeonSecrets before the fill is allowed
  // to use it at all.  Read it now (LoadPotTable also clears s_potkeys_live);
  // the fill is armed below, once the world is loaded and its Pot locations
  // can be matched against this table.
  int potkeys = FillOptValue("pots") == 1;
  Rando_SetPotShuffle(0);
  LoadPotTable(dump_dir);
  // BONK DROPS = SHUFFLED: and the same again for the 42 bonk / tree-pull
  // prizes, cross-checked against kOverworldSprites and kDungeonSprites before
  // the fill is allowed near them.  LoadBonkTable also clears s_bonkdrops_live,
  // so every early return below leaves every tree vanilla.
  int bonkdrops = FillOptValue("bonkdrops") == 1;
  Rando_SetBonkShuffle(0);
  LoadBonkTable(dump_dir);
  int n_universal_keys = 0;   // "Small Key (Universal)" copies the fill placed
  char err[256];
  int pin_requested = pin_location[0] && pin_item[0];
  int pin_loc = -1, pin_item_id = -1;
  double t0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;

  if (Rando_LoadWorld(dump_dir, &world, err, sizeof(err)) != 0) {
    printf("[randomizer] fill skipped (world load failed: %s)\n", err);
    return 0;
  }
  if (pin_requested) {
    pin_loc = Rando_LocationId(&world, pin_location);
    pin_item_id = Rando_ItemId(&world, pin_item);
  }
  // pots: all or nothing.  The fill may only put an item under a pot when
  // every Pot location this world holds is one the engine can hand out.
  if (potkeys && !PotTableCoversWorld(&world))
    potkeys = 0;
  Rando_SetPotShuffle(potkeys);
  // bonk drops: all or nothing, for the same reason
  if (bonkdrops && !BonkTableCoversWorld(&world))
    bonkdrops = 0;
  Rando_SetBonkShuffle(bonkdrops);

  int *placements = (int *)malloc((size_t)world.n_locations * sizeof(int));
  uint8_t *eligible = (uint8_t *)malloc((size_t)world.n_locations);
  int *addr = ParseLocationAddresses(dump_dir, world.n_locations);
  if (!placements || !eligible || !addr) {
    printf("[randomizer] fill skipped (out of memory)\n");
    free(placements);
    free(eligible);
    free(addr);
    Rando_FreeWorld(&world);
    return 0;
  }

  RandoFillStats stats;
  if (Rando_FillWorldEx(&world, dump_dir, (uint32)seed, placements, eligible,
                        &stats, err, sizeof(err),
                        pin_requested ? pin_location : NULL,
                        pin_requested ? pin_item : NULL) != 0) {
    printf("[randomizer] fill failed (%s); falling back to shuffle\n", err);
    if (pin_requested)
      printf("[randomizer] pin NOT applied (fill failed)\n");
    free(placements);
    free(eligible);
    free(addr);
    Rando_FreeWorld(&world);
    return 0;
  }
  if (pin_requested)
    printf("[randomizer] pin: %s\n", stats.pin_note);
  if (pin_requested && stats.pin_applied &&
      (pin_loc < 0 || pin_item_id < 0 || placements[pin_loc] != pin_item_id))
    printf("[randomizer] pin inconsistency: placement table does not hold "
           "the pinned item at %s\n", pin_location);
  double t1 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;

  // ---- apply the chest subset to the engine table ----
  uint8 *chest = kDungeonRoomChests;
  int count = kDungeonRoomChests_SIZE / 3;
  if (count != kForkChestRecords) {
    // the mapping tables are generated against the 168-record US 1.0 table
    printf("[randomizer] chest table has %d records, expected %d; "
           "falling back to shuffle\n", count, kForkChestRecords);
    free(placements);
    free(eligible);
    free(addr);
    Rando_FreeWorld(&world);
    return 0;
  }
  // per-progressive-item copy counters for the chain remap (see
  // Rando_ApplyChestCode in rando_fill.c): the k-th pool copy of a chain
  // item resolves to the chain's k-th concrete engine receipt, so the
  // tiers the engine hands out are exactly the copies the fill's can-beat
  // proof assumed.
  int *chain_seen = (int *)calloc((size_t)world.n_items, sizeof(int));
  uint8_t *used = (uint8_t *)calloc((size_t)count, 1);
  if (!chain_seen || !used) {
    printf("[randomizer] out of memory (apply scratch)\n");
    free(chain_seen);
    free(used);
    free(placements);
    free(eligible);
    free(addr);
    Rando_FreeWorld(&world);
    return 0;
  }

  int n_chest_locs = 0, n_applied = 0, n_unmappable = 0, n_unknown = 0;
  int n_no_record = 0, n_reused = 0;
  char unmapped_names[512];
  unmapped_names[0] = 0;

  if (lf) {
    fprintf(lf, "# fill from %s, %u eligible slots, %u progression + %u junk,\n",
            dump_dir, stats.eligible, stats.prog_items, stats.junk_items);
    fprintf(lf, "# %u attempt(s), %u sweeps, fill %.1f ms\n",
            stats.attempts, stats.sweeps, stats.fill_ms);
    fprintf(lf, "# applied chest placements: room_id big chest_in_room item_id item_name\n");
  }

  DialogueOverride_ClearPlacements();
  s_gift_n = 0;
  for (int loc = 0; loc < world.n_locations; loc++) {
    int a = addr[loc];
    // the engine item code for whatever the fill placed here, chest or not
    // (progressive chains are handed out in location order, so the uncle's
    // sword and the pedestal take chain slots like any chest)
    int item = placements[loc];
    const char *name = (item >= 0 && item < world.n_items) ? world.items[item].name : "?";
    const char *code_why = NULL;
    int code = Rando_ApplyChestCode(&world, item, chain_seen, &code_why);
    // universal keys: the pool's copies are all one nameless item, so the
    // only honest count is how many the fill actually placed
    if (strcmp(name, "Small Key (Universal)") == 0)
      n_universal_keys++;
    {
      // hints (tiles / fortune teller): every real progression item, chest or not
      int it = placements[loc];
      if (it >= 0 && it < world.n_items && world.items[it].progression &&
          world.items[it].pool_count > 0 && world.items[it].itype == NULL)
        DialogueOverride_NotePlacement(world.locations[loc].name, world.items[it].name);
    }
    if (a < kForkChestBase || a >= kForkChestBase + 3 * kForkChestRecords ||
        (a - kForkChestBase) % 3 != 0) {
      // not an engine chest home: an NPC, pedestal, tablet, drop, shop or
      // event. The ones the sprite code asks about get the fill's item.
      if (code >= 0 && s_gift_n < kGiftMax) {
        snprintf(s_gift[s_gift_n].loc, sizeof s_gift[0].loc, "%s", world.locations[loc].name);
        s_gift[s_gift_n].code = code;
        s_gift_n++;
        if (Rando_DungeonItemIsCode(code))
          s_dungeon_codes_live = 1;
      }
      continue;
    }
    n_chest_locs++;

    // resolve the fork record -> the engine record with the same room word
    int fork_rec = (a - kForkChestBase) / 3;
    uint16 roomword = kForkChestRoomWords[fork_rec];
    int k = 0;
    for (int j = 0; j < fork_rec; j++) {
      if (kForkChestRoomWords[j] == roomword)
        k++;
    }
    int seen = 0, engine_idx = -1;
    for (int j = 0; j < count; j++) {
      uint16 w = (uint16)(chest[j * 3] | (chest[j * 3 + 1] << 8));
      if (w == roomword) {
        if (seen == k) {
          engine_idx = j;
          break;
        }
        seen++;
      }
    }

    const char *why = NULL;
    if (code < 0) {
      why = code_why ? code_why : "unknown item";
      if (strcmp(why, "unknown item") == 0) {
        n_unknown++;
      } else {
        // no engine receipt (>= 76) outside the mappable chains
        n_unmappable++;
        size_t len = strlen(unmapped_names);
        snprintf(unmapped_names + len, sizeof(unmapped_names) - len, "%s%s",
                 len ? ", " : "", name);
      }
    } else if (engine_idx < 0) {
      why = "no engine record";
      n_no_record++;
    } else if (used[engine_idx]) {
      why = "engine record reused";
      n_reused++;
    }

    if (why != NULL) {
      // keep the record's vanilla item; log it honestly
      if (lf)
        fprintf(lf, "# skipped %s: %s (%s)\n", world.locations[loc].name, name, why);
      if (loc == pin_loc && stats.pin_applied) {
        // the gold-path record itself could not be written - say so loudly
        if (lf)
          fprintf(lf, "# pin NOT applied: %s = %s (%s)\n", pin_location, name, why);
        printf("[randomizer] pin NOT applied to a chest record: %s = %s (%s)\n",
               pin_location, name, why);
      }
      continue;
    }

    used[engine_idx] = 1;
    chest[engine_idx * 3 + 2] = (uint8)code;
    // DUNGEON COUNT / COLLECT RATE: remember where this placement lives so the
    // HUD can read the save's room flags back.  |slot| is the record's index
    // among the records of the SAME room ignoring the big-chest bit, because
    // that is how dungeon.c indexes kChestOpenMasks (Dungeon_OpenChest walks
    // kDungeonRoomChests matching (room & 0x7fff) and reuses the count).
    if (s_tracked_n < kTrackedMax) {
      uint16 room = (uint16)(roomword & 0x7fff);
      int slot = 0;
      for (int j = 0; j < engine_idx; j++)
        if ((uint16)((chest[j * 3] | (chest[j * 3 + 1] << 8)) & 0x7fff) == room)
          slot++;
      if (slot < 6) {
        s_tracked[s_tracked_n].room = room;
        s_tracked[s_tracked_n].slot = (uint8)slot;
        s_tracked[s_tracked_n].palace = (uint8)PalaceOfLocation(world.locations[loc].name);
        s_tracked_n++;
      }
    }
    if (Rando_DungeonItemIsCode(code))
      s_dungeon_codes_live = 1;
    if (engine_idx < kChestInfoMax) {
      snprintf(s_chest_info[engine_idx].item, sizeof s_chest_info[0].item, "%s", name);
      s_chest_info[engine_idx].prog = world.items[item].progression && world.items[item].pool_count > 0 &&
                                      world.items[item].itype == NULL;
      s_chest_info[engine_idx].known = 1;
    }
    n_applied++;
    if (lf)
      fprintf(lf, "0x%03x %d %d %d %s\n", roomword & 0x7fff,
              (roomword & 0x8000) != 0, k, code, name);
    if (loc == pin_loc && stats.pin_applied) {
      // the pinned demo chest is the gold path: log the exact record written
      if (lf)
        fprintf(lf, "# pin applied: %s = %s -> chest record room 0x%03x "
                    "(chest %d, item id %d)\n",
                pin_location, name, roomword & 0x7fff, k, code);
      printf("[randomizer] pin applied to chest record: room 0x%03x "
             "chest %d = %s (item id %d)\n",
             roomword & 0x7fff, k, name, code);
    }
  }

  // the fourteen enemy key drops read their code out of the gift table the
  // loop just filled, and arm the sprite.c hooks
  ResolveDropCodes(keydrops);
  // and the nineteen pot keys do the same
  ResolvePotCodes(potkeys);
  // and so do the 42 bonk prizes
  ResolveBonkCodes(bonkdrops);

  double t2 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;
  int n_other = world.n_locations - n_chest_locs;
  int n_skipped = n_unmappable + n_unknown + n_no_record + n_reused;

  // Write the full table for offline/gold-test comparison (next to the
  // executable, like randomizer_log.txt).  Written AFTER the apply loop so
  // the JSON can report what the engine world actually received:
  // can_beat_game stays the MODEL claim (the fill proved it on the chain
  // model); the apply object carries the engine-side truth.
  {
    RandoApplyCounts counts;
    counts.chest_locations = n_chest_locs;
    counts.applied = n_applied;
    counts.skipped = n_skipped;
    counts.non_chest_locations = n_other;
    Rando_FillWriteJson(&world, placements, eligible, (uint32)seed, &stats, 1,
                        "rando_placement.json", &counts);
  }

  printf("[randomizer] fill+apply: %d/%d chest placements written "
         "(%d skipped: %d unmappable, %d unknown, %d no-record, %d reused)\n",
         n_applied, n_chest_locs, n_skipped,
         n_unmappable, n_unknown, n_no_record, n_reused);
  // honest boundary: items at non-engine slots never reach the engine's
  // apply layer; a player opening those spots finds vanilla content
  printf("[randomizer] %d items at non-engine slots (NPC/pedestal/drop/pot/"
         "event) are vanilla in-game (phase C)\n", n_other);
  if (n_skipped == 0) {
    // every chest placement landed: the model's can-beat proof carries to
    // the engine chest table, so the unqualified claim is true
    printf("[randomizer] boot cost: load+fill+apply %.0f ms (fill %.1f ms, "
           "%u attempt(s)) - can-beat verified, all %d chest placements "
           "applied\n",
           t2 - t0, stats.fill_ms, stats.attempts, n_chest_locs);
  } else {
    // something kept vanilla: the engine world can diverge from the model,
    // so only the model is claimed, with the gap spelled out
    printf("[randomizer] boot cost: load+fill+apply %.0f ms (fill %.1f ms, "
           "%u attempt(s)) - MODEL can-beat verified; applied %d/%d chest "
           "placements, %d kept vanilla\n",
           t2 - t0, stats.fill_ms, stats.attempts, n_applied, n_chest_locs,
           n_skipped);
  }
  if (n_unmappable && unmapped_names[0])
    printf("[randomizer] items with no engine receipt (vanilla kept): %s\n",
           unmapped_names);
  if (ks_keys == 2)
    // SMALL KEYS = UNIVERSAL: every one of these is the plain 0x24 receipt
    // and goes to the shared pool wherever Link picks it up
    printf("[keys] universal small keys: %d in the pool%c", n_universal_keys, 10);
  if (lf) {
    fprintf(lf, "# summary: %d applied of %d chest locations, %d skipped "
                "(kept vanilla)", n_applied, n_chest_locs, n_skipped);
    if (n_skipped == 0)
      fprintf(lf, " - can-beat verified (all chest placements applied)\n");
    else
      fprintf(lf, " - MODEL can-beat verified only; %d chest(s) diverge "
                  "from the model in-game\n", n_skipped);
    fprintf(lf, "# %d other locations (NPC/pedestal/drop/pot/event) have no "
                "engine apply and are vanilla in-game (phase C)\n", n_other);
  }

  free(used);
  free(chain_seen);
  free(placements);
  free(eligible);
  free(addr);
  Rando_FreeWorld(&world);
  return 1;
}

static uint8 *s_vanilla_chests;        // copy of kDungeonRoomChests before any fill
static size_t s_vanilla_chests_size;
static int s_applied_valid, s_applied_enabled, s_applied_seed, s_applied_logic, s_applied_start;
static int s_applied_opt[kRandoOptCount];
static void EnsureVanillaChests(void) {
  if (s_vanilla_chests) return;
  s_vanilla_chests_size = kDungeonRoomChests_SIZE;
  s_vanilla_chests = (uint8 *)malloc(s_vanilla_chests_size);
  if (s_vanilla_chests) memcpy(s_vanilla_chests, kDungeonRoomChests, s_vanilla_chests_size);
}
static void RestoreVanillaChests(void) {
  if (s_vanilla_chests) memcpy(kDungeonRoomChests, s_vanilla_chests, s_vanilla_chests_size);
}
static void AppliedRecord(int enabled, int seed, int logic, int start, const int *opt) {
  s_applied_valid = 1; s_applied_enabled = enabled; s_applied_seed = seed; s_applied_logic = logic; s_applied_start = start;
  memcpy(s_applied_opt, opt, sizeof s_applied_opt);
}

void Randomizer_Init(void) {
  SettingsMenuEnsureRegistered();      // menu rows + commit hook, disabled or not
  int enabled, seed, log_enabled, logic;
  char dump_dir[256];
  char pin_location[64], pin_item[64];
  LoadRandomizerConfig(&enabled, &seed, &log_enabled, &logic,
                       dump_dir, sizeof(dump_dir),
                       pin_location, sizeof(pin_location),
                       pin_item, sizeof(pin_item));
  // stash what boot read, for the in-game settings editor (runs even when
  // disabled - that is the state the editor must be able to flip)
  s_cfg_enabled = enabled;
  s_cfg_seed = seed;
  s_cfg_logic = logic;
  s_cfg_start = s_ini_start;
  memcpy(s_cfg_opt, s_ini_opt, sizeof s_cfg_opt);
  s_cfg_loaded = 1;
  s_boot_enabled = enabled;            // boot truth for the NEXT BOOT hints
  s_boot_seed = seed;
  s_boot_logic = logic;
  s_boot_start = s_ini_start;
  memcpy(s_boot_opt, s_ini_opt, sizeof s_boot_opt);
  memcpy(s_fill_opt, s_ini_opt, sizeof s_fill_opt);
  EnsureVanillaChests();
  if (!enabled) {
    AppliedRecord(0, 0, logic, s_boot_start, s_boot_opt);
    BossClearOverrides();
    printf("[randomizer] disabled (randomizer.ini)\n");
    return;
  }

  if (seed == 0)
    seed = (int)time(NULL);
  s_boot_seed_used = seed;
  AppliedRecord(1, seed, logic, s_boot_start, s_boot_opt);
  g_rnd_state = (uint32)seed | 1;
  RulesDirEnsure(logic, s_ini_start, s_ini_opt, seed, dump_dir, sizeof(dump_dir));
  char nickname[64];
  MakeSeedNickname(seed, nickname, sizeof(nickname));
  printf("[randomizer] enabled, seed=%d logic=%s (%s) - \"%s\"\n", seed,
         kLogicKeys[logic], dump_dir, nickname);

  // the demo-chest pin is the loud promise to the viewer: whatever the seed
  // shuffles, THIS chest always holds THIS item.  Empty ini values disable.
  int pinned = pin_location[0] && pin_item[0];
  if (pinned)
    printf("[randomizer] demo chest pinned: %s = %s\n", pin_location, pin_item);

  FILE *lf = log_enabled ? fopen("randomizer_log.txt", "w") : NULL;
  if (lf) {
    if (pinned)
      fprintf(lf, "[randomizer] demo chest pinned: %s = %s\n", pin_location,
              pin_item);
    fprintf(lf, "# \"%s\"\n", nickname);
    fprintf(lf, "# zelda3 randomizer (phase B fill) seed %d\n", seed);
  }

  // WORLD/mode gate.  Every folder this build makes is open or standard, so
  // this is clear and nothing below changes; a hand-built inverted folder is
  // the one case it catches.  See src/rando/rando_inverted.c.
  RandoInverted_Load(dump_dir);
  ApplyBossShuffle(dump_dir);   // boss sprites follow the same dump
  ApplyEntranceShuffle(dump_dir, s_ini_opt[kEntranceOptIndex]);   // and the doors
  if (!RandoInverted_Active() &&
      ApplyFillPlacements(seed, dump_dir, lf, pin_location, pin_item)) {
    if (lf) {
      fclose(lf);
      printf("[randomizer] placements written to randomizer_log.txt + "
             "rando_placement.json\n");
    }
    Enemizer_Apply();
    return;
  }
  if (lf)
    fclose(lf);

  // Fallback: the phase-1 class-preserving chest shuffle (no dumps needed,
  // logic-blind - kept for when the fill path is unavailable).
  printf("[randomizer] fill unavailable, using phase-1 chest shuffle\n");
  if (pinned)
    printf("[randomizer] pin NOT applied (phase-1 shuffle fallback has no "
           "pin support)\n");

  // Chest table: 3-byte records { uint16 room (bit15 = big chest), uint8 item }.
  // Shuffle the item bytes within each class (big stays big, small stays
  // small). Logic-safe placement arrives with the fill-algorithm port; this
  // phase proves the pipeline end to end.
  uint8 *chest = kDungeonRoomChests;
  int count = kDungeonRoomChests_SIZE / 3;
  int *small = (int *)malloc(count * sizeof(int));
  int *big = (int *)malloc(count * sizeof(int));
  if (!small || !big) {
    free(small);
    free(big);
    printf("[randomizer] out of memory\n");
    return;
  }
  int nsmall = 0, nbig = 0;
  for (int i = 0; i < count; i++) {
    if (chest[i * 3 + 1] & 0x80)
      big[nbig++] = i;
    else
      small[nsmall++] = i;
  }
  printf("[randomizer] chest records: %d total (%d small, %d big)\n", count, nsmall, nbig);

  for (int pass = 0; pass < 2; pass++) {
    int *list = pass ? big : small;
    int n = pass ? nbig : nsmall;
    for (int i = n - 1; i > 0; i--) {
      int j = (int)(RndNext() % (uint32)(i + 1));
      uint8 t = chest[list[i] * 3 + 2];
      chest[list[i] * 3 + 2] = chest[list[j] * 3 + 2];
      chest[list[j] * 3 + 2] = t;
    }
  }

  if (log_enabled) {
    lf = fopen("randomizer_log.txt", "w");
    if (lf) {
      fprintf(lf, "# \"%s\"\n", nickname);
      fprintf(lf, "# zelda3 randomizer seed %d (phase-1 fallback shuffle)\n", seed);
      fprintf(lf, "# room_id big item_id\n");
      for (int i = 0; i < count; i++) {
        uint16 room = (chest[i * 3] | ((uint16)chest[i * 3 + 1] << 8)) & 0x7fff;
        fprintf(lf, "%d %d %d\n", room, (chest[i * 3 + 1] & 0x80) != 0, chest[i * 3 + 2]);
      }
      fclose(lf);
      printf("[randomizer] placements written to randomizer_log.txt\n");
    }
  }
  free(small);
  free(big);
  Enemizer_Apply();
}

// ---- per-file settings: saves/rando_slots.ini next to the SRAM -----------
// The owner ruled the seed belongs to the save file and never changes once the
// file exists. The save itself only carries a flag byte (0x4E0); the values
// live in a small ini keyed by slot, so nothing in the vanilla save layout is
// touched. Copy/erase on the file-select screen mirror the entries.
#define kRandoSlotsIni "saves/rando_slots.ini"
typedef struct RandoSlotCfg { int present, enabled, seed, logic, start; int opt[kRandoOptCount]; } RandoSlotCfg;
static RandoSlotCfg s_slots[3];
static int s_slots_loaded;

static void SlotsLoad(void) {
  if (s_slots_loaded) return;
  s_slots_loaded = 1;
  memset(s_slots, 0, sizeof(s_slots));
  FILE *f = fopen(kRandoSlotsIni, "r");
  if (!f) return;
  char line[160], key[32], val[96];
  int n;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "slot%d.%31[a-z_]=%95s", &n, key, val) != 3 || n < 1 || n > 3) continue;
    RandoSlotCfg *c = &s_slots[n - 1];
    c->present = 1;
    if (!strcmp(key, "enabled")) c->enabled = atoi(val);
    else if (!strcmp(key, "seed")) c->seed = atoi(val);
    else if (!strcmp(key, "logic")) { int i = LogicIndexFromKey(val); c->logic = i < 0 ? 0 : i; }
    else if (!strcmp(key, "start")) c->start = !strcmp(val, "standard") ? kStartStandard : kStartOpen;
    else {
      for (int o = 0; o < kRandoOptCount; o++)
        if (!strcmp(key, kRandoOpts[o].inikey)) c->opt[o] = OptIndexFromValue(o, val);
    }
  }
  fclose(f);
}

static void SlotsSave(void) {
  FILE *f = fopen(kRandoSlotsIni, "wb");
  if (!f) { printf("[randomizer] cannot write %s%c", kRandoSlotsIni, 10); return; }
  fprintf(f, "# per-file randomizer settings (slotN = file N); fixed when the file is made%c", 10);
  for (int i = 0; i < 3; i++) {
    if (!s_slots[i].present) continue;
    fprintf(f, "slot%d.enabled=%d%c", i + 1, s_slots[i].enabled, 10);
    fprintf(f, "slot%d.seed=%d%c", i + 1, s_slots[i].seed, 10);
    fprintf(f, "slot%d.logic=%s%c", i + 1, kLogicKeys[s_slots[i].logic], 10);
    fprintf(f, "slot%d.start=%s%c", i + 1, kStartKeys[s_slots[i].start & 1], 10);
    for (int o = 0; o < kRandoOptCount; o++)
      fprintf(f, "slot%d.%s=%s%c", i + 1, kRandoOpts[o].inikey, kRandoOpts[o].vals[s_slots[i].opt[o]], 10);
  }
  fclose(f);
}

static int CurrentSlot(void) {
  int s = WORD(g_ram[0]) / 0x500;      // set by both file-load paths before CopySaveToWRAM
  return (s >= 0 && s < 3) ? s : 0;
}

static void StampSlot(int slot, int enabled, int seed, int logic, int start, const int *opt) {
  SlotsLoad();
  RandoSlotCfg *c = &s_slots[slot];
  c->present = 1; c->enabled = enabled; c->seed = seed; c->logic = logic; c->start = start;
  memcpy(c->opt, opt, sizeof c->opt);
  SlotsSave();
}

// Re-fill (or restore vanilla) for the given settings; no-op when already applied.
static void ApplySettingsNow(int enabled, int seed, int logic, int start, const int *opt) {
  if (logic < 0 || logic >= kLogicCount) logic = 0;
  if (s_applied_valid && enabled == s_applied_enabled &&
      (!enabled || (seed == s_applied_seed && logic == s_applied_logic && start == s_applied_start &&
                    !memcmp(opt, s_applied_opt, sizeof s_applied_opt))))
    return;
  EnsureVanillaChests();
  RestoreVanillaChests();
  s_dungeon_codes_live = 0;
  s_keydrops_live = 0;        // a vanilla file drops vanilla keys
  s_potkeys_live = 0;         // ... and vanilla pot keys
  RandoEntrance_Reset();      // and walks into the doors it always did
  s_bonkdrops_live = 0;       // ... and vanilla tree / bonk prizes
  Randomizer_BonkDropsReset();
  RandoInverted_Reset();      // ... in the world it always did
  memcpy(s_fill_opt, opt, sizeof s_fill_opt);
  AppliedRecord(enabled, seed, logic, start, opt);
  if (!enabled) {
    BossClearOverrides();
    printf("[randomizer] file: vanilla chests (randomizer off for this save)%c", 10);
    fflush(stdout);
    return;
  }
  int e, sd, log_enabled, lg;
  char dir[256], pin_loc[64], pin_item[64];
  LoadRandomizerConfig(&e, &sd, &log_enabled, &lg, dir, sizeof(dir), pin_loc, sizeof(pin_loc), pin_item, sizeof(pin_item));
  RulesDirEnsure(logic, start, opt, seed, dir, sizeof(dir));
  g_rnd_state = (uint32)seed | 1;
  FILE *lf = log_enabled ? fopen("randomizer_log.txt", "w") : NULL;
  if (lf) fprintf(lf, "# zelda3 randomizer (per-file) seed %d logic %s%c", seed, kLogicKeys[logic], 10);
  printf("[randomizer] file: seed=%d logic=%s%c", seed, kLogicKeys[logic], 10);
  RandoInverted_Load(dir);   // WORLD/mode gate, see src/rando/rando_inverted.c
  ApplyBossShuffle(dir);   // the file's bosses, from the same rule folder
  ApplyEntranceShuffle(dir, opt[kEntranceOptIndex]);   // and the file's doors
  if (RandoInverted_Active() ||
      !ApplyFillPlacements(seed, dir, lf, pin_loc, pin_item))
    printf("[randomizer] file: fill unavailable, vanilla chests%c", 10);
  if (lf) fclose(lf);
  fflush(stdout);
}

void Randomizer_NewFileStamp(int slot) {
  SettingsEnsureLoaded();
  if (slot < 0 || slot > 2) return;
  int seed = PendingFileSeed();   // the very number PrepareRules built for
  s_pending_seed_rolled = 0;      // the file after this one rolls its own
  StampSlot(slot, s_cfg_enabled, seed, s_cfg_logic, s_cfg_start, s_cfg_opt);
  printf("[randomizer] file %d created: %s seed=%d logic=%s start=%s%c", slot + 1,
         s_cfg_enabled ? "randomizer" : "vanilla", seed, kLogicKeys[s_cfg_logic], kStartKeys[s_cfg_start], 10);
  fflush(stdout);
}

void Randomizer_SlotErased(int slot) {
  SlotsLoad();
  if (slot >= 0 && slot < 3 && s_slots[slot].present) { s_slots[slot].present = 0; SlotsSave(); }
}

void Randomizer_SlotCopied(int dst, int src) {
  SlotsLoad();
  if (dst < 0 || dst > 2 || src < 0 || src > 2 || dst == src) return;
  s_slots[dst] = s_slots[src];
  SlotsSave();
}

int Randomizer_SlotInfo(int slot, int *enabled, int *seed, int *logic) {
  SlotsLoad();
  if (slot < 0 || slot > 2 || !s_slots[slot].present) return 0;
  *enabled = s_slots[slot].enabled; *seed = s_slots[slot].seed; *logic = s_slots[slot].logic;
  return 1;
}

// RETRO: the reference rewrites the enemy prize packs so arrows never drop -
// Fill.py set_prize_drops, bow_mode retro: 0xE1 (5 arrows) -> 0xDA (blue
// rupee), 0xE2 (10 arrows) -> 0xDB (red rupee) - and the engine's
// kPrizeItems[56] IS that table (sprite.c; the same 56 bytes the reference
// writes to ROM 0x37A78).  Patched from the pristine copy on every file load,
// so a vanilla or non-retro file gets the untouched table back.
static uint8 s_prize_items_vanilla[56];
static int s_prize_items_saved;
static void ApplyRetroPrizeTable(void) {
  if (!s_prize_items_saved) {
    memcpy(s_prize_items_vanilla, kPrizeItems, sizeof s_prize_items_vanilla);
    s_prize_items_saved = 1;
  }
  memcpy(kPrizeItems, s_prize_items_vanilla, sizeof s_prize_items_vanilla);
  if (!g_rando_retro)
    return;
  for (size_t i = 0; i < sizeof s_prize_items_vanilla; i++) {
    if (kPrizeItems[i] == 0xe1) kPrizeItems[i] = 0xda;
    else if (kPrizeItems[i] == 0xe2) kPrizeItems[i] = 0xdb;
  }
}

void Randomizer_ApplyFromSave(void) {
  SettingsEnsureLoaded();
  SlotsLoad();
  int slot = CurrentSlot();
  RandoSlotCfg *c = &s_slots[slot];
  if (!rando_save_flag || !c->present) {
    // a file from before per-file settings: freeze what this boot used so it
    // plays on unchanged from here
    rando_save_flag = 1;               // WRAM mirror; SaveGameFile persists it
    StampSlot(slot, s_boot_enabled, s_boot_enabled ? s_boot_seed_used : 0, s_boot_logic, kStartStandard, s_boot_opt);   // an old file played the vanilla intro
    printf("[randomizer] file %d had no settings: frozen to this boot's (%s seed=%d logic=%s)%c",
           slot + 1, s_boot_enabled ? "randomizer" : "vanilla", s_boot_seed_used, kLogicKeys[s_boot_logic], 10);
    fflush(stdout);
  }
  printf("[randomizer] file %d: seed=%d logic=%s %s%c", slot + 1, c->seed, kLogicKeys[c->logic],
         c->enabled ? "(randomizer)" : "(vanilla)", 10);
  fflush(stdout);
  ApplySettingsNow(c->enabled, c->seed, c->logic, c->start, c->opt);
  Enemizer_Apply();   // the file's ENEMY HEALTH / ENEMY DAMAGE tables

  // SMALL KEYS = UNIVERSAL: decide it here, once, for the file that just
  // landed in WRAM - the engine's key paths read the flag, not the setting.
  // A file with the randomizer off never placed universal keys, so it keeps
  // the vanilla per-dungeon counters.
  // RETRO also means universal keys: the reference's CLI expands --retro into
  // keyshuffle='universal' (CLI.py) before the pool is built, so a retro dump
  // really does carry 19 "Small Key (Universal)" items whatever the SMALL KEYS
  // row says, and the engine has to pool them the same way.
  g_rando_retro = (uint8)(c->enabled && Randomizer_FileOptValue("retro") == 1);
  g_rando_universal_keys = (uint8)(c->enabled && (Randomizer_FileOptValue("keys") == 2 || g_rando_retro));
  // BOMB BAG: bombs are uncarryable until the bag turns up.  The flag itself
  // lives in the save (link_has_bomb_bag / 0x7EF4E1), so a file that already
  // found it keeps it across a load.
  g_rando_bomb_bag = (uint8)(c->enabled && Randomizer_FileOptValue("bombbag") == 1);
  if (g_rando_bomb_bag && !link_has_bomb_bag) {
    link_item_bombs = 0;              // a bag-less file carries nothing
    link_bomb_filler = 0;
  }
  ApplyRetroPrizeTable();
  // TRIFORCE HUNT: the piece count lives in the save (link_triforce_pieces /
  // 0x7EF4E3) and comes back with the file; only the "win is due" latch is
  // per-session state, and it must not survive a switch to another file.
  s_win_pending = 0;
  // HEART COLOR: RANDOM (index 4) picks once here, from whatever g_rnd_state
  // this file's fill left behind - a pure function of c->seed, so the same
  // file gets the same color on every future load.  A file with the
  // randomizer off keeps 0 (RED, vanilla).
  {
    int hc = c->enabled ? Randomizer_FileOptValue("heartcolor") : 0;
    g_rando_heart_color = (uint8)(hc == 4 ? (RndNext() & 3) : hc);
  }
  if (g_rando_universal_keys) {
    // the saved pool is the authority the moment the save is in WRAM:
    // link_num_keys is saved too, but a file saved on the overworld saved
    // the 0xff "no dungeon" blank over it.
    link_num_keys = link_num_keys_universal;
    printf("[keys] universal small keys: this file carries %d%c", link_num_keys, 10);
    fflush(stdout);
  }
}

// the loaded file's value index for an extra setting, by its ini key
// ("keys", "enemyhp"...): 0 = the reference default. Engine rounds read this.
int Randomizer_FileOptValue(const char *inikey) {
  SlotsLoad();
  RandoSlotCfg *c = &s_slots[CurrentSlot()];
  for (int o = 0; o < kRandoOptCount; o++)
    if (!strcmp(kRandoOpts[o].inikey, inikey))
      return c->present ? c->opt[o] : (SettingsEnsureLoaded(), s_cfg_opt[o]);
  return 0;
}
const char *Randomizer_FileOptRefValue(const char *inikey) {
  int v = Randomizer_FileOptValue(inikey);
  for (int o = 0; o < kRandoOptCount; o++)
    if (!strcmp(kRandoOpts[o].inikey, inikey)) return kRandoOpts[o].vals[v];
  return "";
}

int Randomizer_FileStart(void) { SlotsLoad(); RandoSlotCfg *c = &s_slots[CurrentSlot()]; return c->present ? c->start : Randomizer_SettingsStart(); }

// OPEN start = what the reference randomizer's init_open_mode_sram does:
// progress indicator 2 (Zelda already safe), progress flags 0x14 (uncle gone),
// starting entrance 1 (Link's house), Hyrule Castle gate pre-opened.
void Randomizer_NewFileInitSram(unsigned char *sram) {
  SettingsEnsureLoaded();
  if (s_cfg_start != kStartOpen) return;
  sram[0x3C5] = 2;
  sram[0x3C6] = 0x14;
  sram[0x3C8] = 1;
  sram[0x280 + 0x1B] |= 0x20;
  if (s_cfg_opt[7] == 1) sram[0x280 + 0x5B] |= 0x20;   // PYRAMID HOLE = OPEN
  printf("[randomizer] new file: OPEN start (house, Zelda safe, gate open)%c", 10);
}

// ---- the RANDOMIZER page rows -------------------------------------------
int Randomizer_PageRows(void) { return 4 + kRandoOptCount + 1; }

int Randomizer_PageRow(int i, const char **label, char *val, size_t cap, const char **d1, const char **d2) {
  SettingsEnsureLoaded();
  *label = ""; *d1 = ""; *d2 = "";
  if (cap) val[0] = 0;
  switch (i) {
  case 0:
    *label = "RANDOMIZER"; snprintf(val, cap, "%s", s_cfg_enabled ? "ON" : "OFF");
    *d1 = "SHUFFLES THE ITEMS"; *d2 = "OFF IS THE NORMAL GAME";
    return kRPKind_Enum;
  case 1:
    *label = "SEED"; *d1 = "SAME SEED SAME ITEMS"; *d2 = "A RANDOM  L OR R JUMPS";
    return kRPKind_Seed;
  case 2:
    *label = "LOGIC"; snprintf(val, cap, "%s", kLogicShort[s_cfg_logic]);
    switch (s_cfg_logic) {
    case 0: *d1 = "ALL ITEMS REACHABLE"; *d2 = "WITHOUT GLITCHES"; break;
    case 1: *d1 = "MINOR GLITCHES MAY BE"; *d2 = "NEEDED FOR SOME ITEMS"; break;
    case 2: *d1 = "OVERWORLD GLITCHES MAY"; *d2 = "BE NEEDED FOR ITEMS"; break;
    case 3: *d1 = "OVERWORLD AND MAJOR"; *d2 = "GLITCHES MAY BE NEEDED"; break;
    default: *d1 = "NO GUARANTEES AT ALL"; *d2 = "ITEMS CAN BE ANYWHERE"; break;
    }
    return kRPKind_Enum;
  case 3:
    *label = "START"; snprintf(val, cap, "%s", kStartLabels[s_cfg_start & 1]);
    if (s_cfg_start == kStartOpen) { *d1 = "SKIPS THE CASTLE INTRO"; *d2 = "ZELDA IS ALREADY SAFE"; }
    else { *d1 = "THE NORMAL OPENING"; *d2 = "UNCLE CASTLE ZELDA"; }
    return kRPKind_Enum;
  default: {
    int o = i - 4;
    if (o >= 0 && o < kRandoOptCount) {
      *label = kRandoOpts[o].label;
      snprintf(val, cap, "%s", kRandoOpts[o].names[s_cfg_opt[o]]);
      *d1 = kRandoOpts[o].d1; *d2 = kRandoOpts[o].d2;
      return kRPKind_Enum;
    }
    *label = "BEGIN";
    return kRPKind_Begin;
  }
  }
}

void Randomizer_PageRowCycle(int i, int delta) {
  SettingsEnsureLoaded();
  int step = delta < 0 ? -1 : 1;
  switch (i) {
  case 0: Randomizer_SettingsToggleEnabled(); break;
  case 2: Randomizer_SettingsLogicCycle(step); break;
  case 3: Randomizer_SettingsStartCycle(); break;
  default: {
    int o = i - 4;
    if (o >= 0 && o < kRandoOptCount)
      s_cfg_opt[o] = (s_cfg_opt[o] + step + kRandoOpts[o].n) % kRandoOpts[o].n;
    break;
  }
  }
  s_rules_tried = 0;          // a new combination may need its rules built
  s_pending_seed_rolled = 0;  // ...and, with BOSSES on, its own seed
}

int Randomizer_FileLocked(void) { SlotsLoad(); return s_slots[CurrentSlot()].present; }
int Randomizer_FileEnabled(void) { SlotsLoad(); RandoSlotCfg *c = &s_slots[CurrentSlot()]; return c->present ? c->enabled : Randomizer_SettingsEnabled(); }
int Randomizer_FileSeed(void) { SlotsLoad(); RandoSlotCfg *c = &s_slots[CurrentSlot()]; return c->present ? c->seed : Randomizer_SettingsSeed(); }
int Randomizer_FileLogic(void) { SlotsLoad(); RandoSlotCfg *c = &s_slots[CurrentSlot()]; return c->present ? c->logic : Randomizer_SettingsLogic(); }

// Pending-settings helper for the in-engine RANDOMIZER page (select_file.c):
// A on the SEED row flips between RANDOM (0) and the last number.
void Randomizer_SettingsSeedToggleRandom(void) {
  static int last = 1234;
  SettingsEnsureLoaded();
  if (s_cfg_seed) { last = s_cfg_seed; s_cfg_seed = 0; }
  else s_cfg_seed = last;
}
int Randomizer_SettingsSeedAdjustFrom(int delta) {   // RANDOM + delta starts from the last number
  SettingsEnsureLoaded();
  if (s_cfg_seed == 0) s_cfg_seed = 1234;
  Randomizer_SettingsSeedAdjust(delta);
  return s_cfg_seed;
}

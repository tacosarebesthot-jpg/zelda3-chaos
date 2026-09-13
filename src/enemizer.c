#include "enemizer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "randomizer.h"
#include "sprite.h"

// ENEMY HEALTH / ENEMY DAMAGE, per save file.  See enemizer.h for the shape;
// the interesting decisions are which sprite types count as "an enemy" and how
// bosses are kept out of it, both of which are answered from the vanilla
// tables alone (no hand-maintained list of 243 rows).

// "enemyhp" value order, as the RANDOMIZER page (kV_eh/kN_eh) defines it.
enum { kHp_Normal, kHp_Easy, kHp_Shuffled, kHp_Hard, kHp_Expert, kHp_Count };
// "enemydmg" value order (kV_ed/kN_ed).
enum { kDmg_Normal, kDmg_Shuffled, kDmg_Random, kDmg_Count };

static const char *const kHpNames[kHp_Count] = {"NORMAL", "EASY", "SHUFFLED", "HARD", "EXPERT"};
static const char *const kDmgNames[kDmg_Count] = {"NORMAL", "SHUFFLED", "RANDOM"};

// The runtime copies the spawn path reads.  Vanilla until Enemizer_Apply says
// otherwise; s_have_tables guards the window before the first Apply so a spawn
// there cannot read the zero-initialized arrays.
static uint8 s_health[kEnemizerTypes];
static uint8 s_bump[kEnemizerTypes];
static int s_have_tables;

// Sprite types the two tables also cover that are not enemies: the tables are
// indexed by sprite type and plenty of types are furniture.  Randomizing these
// would change things the settings never promised to touch (a pull switch that
// suddenly kills, a townsperson with a boss's hit points), so their rows are
// copied through untouched.  Everything with vanilla health 0 is already out
// (Link's arrow, thrown items, the Master Sword, most projectiles and the
// overlord-ish helpers all sit at 0), which leaves only these.
static const uint8 kNotEnemies[] = {
  0x03, 0x04, 0x05, 0x06, 0x07,  // pull switches (health 3, bump class 2)
  0x21,                          // water switch
  0x29, 0x2a, 0x2b, 0x2c, 0x2d,  // townspeople, telepathic tile,
  0x2e, 0x2f, 0x30, 0x32, 0x34,  //   minigame hosts, the witch,
  0x35, 0x36, 0x38, 0x3d,        //   the eye statue
  0x40,                          // tutorial guard / barrier
  0x65,                          // archery game host
  0x89,                          // Mothula's beam (a boss projectile)
  0x8a,                          // spike block (a room hazard, not an enemy)
  0x9e, 0x9f, 0xa0,              // haunted grove ostrich / rabbit / bird
  0xab,                          // crystal maiden
};

static bool IsNotEnemy(int t) {
  for (size_t i = 0; i < arraysize(kNotEnemies); i++)
    if (kNotEnemies[i] == t)
      return true;
  return false;
}

// A boss plays exactly as it always did, in every mode.  Two tests, both read
// off the vanilla tables:
//   - vanilla health >= 0x40.  That is the round's rule, and it also sweeps up
//     every 255 ("not hurt the ordinary way") row.
//   - bit 4 of the vanilla bump-damage byte.  That is the engine's own boss
//     bit: Sprite_ReturnIfBossFinished (sprite_main.c) clears every sprite in
//     the room that does NOT have it when the boss is already dead.  It
//     catches the bosses the health rule misses - Moldorm (12 hp), Lanmolas
//     (16), Mothula and Arrghus (32, plus Arrghi at 8), the Trinexx heads
//     (40), Armos Knights, Helmasaur King and Vitreous' eye (48), and
//     Kholdstare's falling ice (8).
static bool IsBoss(int t) {
  return kSpriteInit_Health[t] >= 0x40 || (kSpriteInit_BumpDamage[t] & 0x10) != 0;
}

// The set both settings work on: ordinary, killable, hostile sprite types.
static bool IsEnemy(int t) {
  return kSpriteInit_Health[t] != 0 && !IsBoss(t) && !IsNotEnemy(t);
}

// xorshift32.  Deterministic from the file's seed so a seed always plays the
// same; the two settings get their own stream (different salt) so changing one
// of them cannot move the other one's roll.
static uint32 s_rng;

static uint32 RndNext(void) {
  uint32 x = s_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return s_rng = x;
}

static void RndSeed(int seed, uint32 salt) {
  // Scatter neighbouring seeds (1, 2, 3... are the hand-typed ones) and never
  // let the state be 0, the one value xorshift cannot leave.
  uint32 s = ((uint32)seed ^ salt) * 2654435761u + 0x9e3779b9u;
  s_rng = s | 1;
}

// Fisher-Yates over |n| bytes, in place.
static void ShuffleBytes(uint8 *v, int n) {
  for (int i = n - 1; i > 0; i--) {
    int j = (int)(RndNext() % (uint32)(i + 1));
    uint8 t = v[i];
    v[i] = v[j];
    v[j] = t;
  }
}

// ENEMIES / ENEMY COLOR, further down: rebuilt on the same call, so a file
// load never leaves one of the four settings stale.
static void Enemizer_ApplyExtras(void);

void Enemizer_Apply(void) {
  memcpy(s_health, kSpriteInit_Health, sizeof(s_health));
  memcpy(s_bump, kSpriteInit_BumpDamage, sizeof(s_bump));
  s_have_tables = 1;
  Enemizer_ApplyExtras();

  if (!Randomizer_FileEnabled())
    return;                              // a vanilla file keeps vanilla stats

  int hp_mode = Randomizer_FileOptValue("enemyhp");
  int dmg_mode = Randomizer_FileOptValue("enemydmg");
  if (hp_mode < 0 || hp_mode >= kHp_Count) hp_mode = kHp_Normal;
  if (dmg_mode < 0 || dmg_mode >= kDmg_Count) dmg_mode = kDmg_Normal;
  if (hp_mode == kHp_Normal && dmg_mode == kDmg_Normal)
    return;                              // nothing applied, nothing to say

  int seed = Randomizer_FileSeed();

  // The enemy set, once, for both settings.
  int list[kEnemizerTypes], n = 0;
  for (int t = 0; t < kEnemizerTypes; t++)
    if (IsEnemy(t))
      list[n++] = t;

  if (hp_mode != kHp_Normal) {
    RndSeed(seed, 0x48500000u);          // "HP"
    if (hp_mode == kHp_Shuffled) {
      // Health values change owners inside the enemy set only, so no ordinary
      // enemy can end up with a boss's hit points and no boss loses its own.
      uint8 vals[kEnemizerTypes];
      for (int i = 0; i < n; i++)
        vals[i] = kSpriteInit_Health[list[i]];
      ShuffleBytes(vals, n);
      for (int i = 0; i < n; i++)
        s_health[list[i]] = vals[i];
    } else {
      for (int i = 0; i < n; i++) {
        int hp = kSpriteInit_Health[list[i]];
        if (hp_mode == kHp_Easy)
          hp = hp / 2 < 1 ? 1 : hp / 2;  // halved, but never down to 0
        else if (hp_mode == kHp_Hard)
          hp = hp * 3 / 2;
        else                             // kHp_Expert
          hp = hp * 2;
        // The toughest ordinary enemy has 32 hp (everything above that is a
        // boss), so EXPERT tops out at 64 and 255 is never reached; the clamp
        // is here so the byte can never wrap.
        s_health[list[i]] = (uint8)(hp > 255 ? 255 : hp);
      }
    }
  }

  if (dmg_mode != kDmg_Normal) {
    // Contact damage lives in the low nibble of kSpriteInit_BumpDamage: a
    // class that Sprite_AttemptDamageToLinkPlusRecoil turns into hearts with
    // kPlayerDamages[3 * class + link_armor].  The high nibble is flags (0x10
    // boss, 0x20/0x40/0x80 read elsewhere), so only the nibble moves.
    // An enemy whose vanilla class is 0 stays at 0: that is the table's "no
    // contact-damage class of its own" row, and the handful of real enemies
    // sitting there (rat, keese, wallmaster, hardhat beetle) have prep code
    // that writes sprite_bump_damage itself anyway.
    int dl[kEnemizerTypes], dn = 0;
    uint8 classes[kEnemizerTypes];
    for (int i = 0; i < n; i++) {
      uint8 c = kSpriteInit_BumpDamage[list[i]] & 0xf;
      if (!c)
        continue;
      dl[dn] = list[i];
      classes[dn] = c;
      dn++;
    }
    // The distinct classes vanilla hands to ordinary enemies (1..8 - class 9
    // exists, but only on Ganon), which is the whole draw pool for RANDOM.
    // Drawing from it is what keeps RANDOM from ever being harsher than the
    // hardest vanilla enemy.
    uint8 pool[16];
    int nc = 0;
    for (int i = 0; i < dn; i++) {
      bool seen = false;
      for (int p = 0; p < nc; p++)
        seen = seen || pool[p] == classes[i];
      if (!seen && nc < (int)arraysize(pool))
        pool[nc++] = classes[i];
    }

    RndSeed(seed, 0x444d0000u);          // "DM"
    if (dmg_mode == kDmg_Shuffled) {
      ShuffleBytes(classes, dn);
      for (int i = 0; i < dn; i++)
        s_bump[dl[i]] = (kSpriteInit_BumpDamage[dl[i]] & 0xf0) | classes[i];
    } else if (nc > 0) {                 // kDmg_Random
      for (int i = 0; i < dn; i++)
        s_bump[dl[i]] = (kSpriteInit_BumpDamage[dl[i]] & 0xf0) | pool[RndNext() % (uint32)nc];
    }
  }

  printf("[enemizer] health=%s damage=%s seed=%d\n", kHpNames[hp_mode], kDmgNames[dmg_mode], seed);
  fflush(stdout);
}

uint8 Enemizer_Health(int type) {
  if (!s_have_tables || (unsigned)type >= kEnemizerTypes)
    return kSpriteInit_Health[type];
  return s_health[type];
}

uint8 Enemizer_BumpDamage(int type) {
  if (!s_have_tables || (unsigned)type >= kEnemizerTypes)
    return kSpriteInit_BumpDamage[type];
  return s_bump[type];
}

// ===========================================================================
// ENEMIES (the "enemyshuffle" row) - a seeded, per-file enemy swap.
// ===========================================================================
//
// WHY THIS IS THE FORK'S OWN SHUFFLE AND NOT THE REFERENCE'S.
// The reference DOES implement its enemizer in python (source/enemizer/
// Enemizer.py randomize_enemies, driven by --setting shuffleenemies=shuffled;
// there is no external Enemizer.exe in this fork), and its result is a whole
// new underworld/overworld sprite table plus a re-rolled set of sprite SHEETS
// per room.  Two measurements decided against passing the setting through:
//
//   1. It is not presentation.  Dumping seed 1234 with and without
//      `--setting shuffleenemies=shuffled` changes the logic: item_names
//      44 -> 45 (a Bottle becomes a Bottle (Bee)), trivial_always_true
//      1518 -> 1515, nontrivial_rules 1066 -> 1069, 29 edges get a new rule
//      and the "Crab Drop" farm locations disappear (ItemList.py only makes
//      them when enemy_shuffle == 'none').
//   2. Those 29 edges are the kill-the-room shutter doors, and they move in
//      BOTH directions.  With enemy shuffle on, "Eastern Single Eyegore NE"
//      drops from "needs Bow" to always-true, "PoD Mimics 1 NW" and "TR Twin
//      Pokeys" lose their weapon requirement, and so on - because the
//      reference knows exactly which enemy it put there.  An engine that
//      cannot reproduce that exact placement but ships the loosened logic
//      would hand out seeds whose only copy of an item sits behind a shutter
//      the player provably cannot open.
//
// Reproducing the placement means dumping the reference's own uw/ow enemy
// tables AND its sheet assignment (201 of 247 rooms change at seed 1234, and
// 47 rooms gain sprites they never had), then re-basing KEY DROPS, POTS, BONK
// DROPS and BOSSES on the new lists.  That is its own round.  Until then this
// row is engine-side and presentation-only: it never reaches the dump, so the
// rule folder keeps VANILLA enemy logic, and the swap below is built so that
// vanilla logic stays true.
//
// THE SAFE POOL.  A slot may only change type, never position, and only into
// a type that
//   * is an ordinary enemy by the same test ENEMY HEALTH uses (IsEnemy: real
//     hit points, not a boss, not furniture/NPC),
//   * is below 0xd8, which drops every prize, key drop, heart container and
//     heart piece in one bound (0xd8 is exactly where the absorbables start),
//     and is not 0x79 / 0xac, the bee and the apples the BONK DROPS round arms,
//   * shares the room's SPRITE SHEET.  A room's sprite graphics come from room
//     header byte 3 (kDungeonRoomHeaders[..][3]), outdoors from
//     kOverworldSpriteGfx; a type taken from another slot with the same sheet
//     is guaranteed to have its tiles already in VRAM, so nothing renders as
//     garbage and no sheet has to be re-rolled.
//   * has the same KILL MASK.  kEnemyDamageData is the vanilla 16-nibble
//     "what each of the 16 damage sources does to me" row per sprite type;
//     run through kEnemizerDamages (the engine's own kEnemyDamages table) it
//     says which sources actually hurt this type.  Two types with the same
//     mask need the same weapons, so every "kill everything in the room"
//     shutter door keeps its vanilla requirement and the vanilla logic in the
//     rule folder stays exact.
//
// Rooms the other rounds own are skipped whole: the thirteen boss super-tiles
// and the Thieves Town maiden room (BOSSES rewrites those lists), and any
// room holding a 0xe4 die-action marker (the fourteen KEY DROPS).  Overlord
// records (x >= 0xe0) are not sprites and are never touched.  POTS live in
// kDungeonSecrets, not in the sprite list, so they cannot be reached at all.
//
// What the pool comes out as, measured against the shipped zelda3_assets.dat:
//   underworld  16 groups, 260 slots in 68 rooms -
//     MiniMoldorm/RedBari/BlueBari, CricketRat/Snake, Blue/RedZazak,
//     Popo/Popo2, GreenGuard/GreenKnifeGuard/BallNChain,
//     RedSpearGuard/BluesainBolt/RedJavelinGuard/BallNChain,
//     RedSpearGuard/BlueArcher
//   overworld   22 groups, ~470 slot visits -
//     Octorok/Octorok4Way/Buzzblob/BlueArcher/Crab, Snapdragon/Hinox/Moblin/
//     Ropa, the guard families, Green/BlueZirro
// Every one of those is a ground-walking or low-hovering enemy paired with
// another of the same build, so no swap can strand an enemy over a pit or in
// water, and no flyer ever becomes a walker.

enum { kEnem_Normal, kEnem_Shuffled, kEnem_Count };

// The vanilla damage-source table, 16 sources x 8 subclasses.  This is the
// same const kEnemyDamages sprite.c keeps for Sprite_AttemptDamage; it is
// duplicated here (128 vanilla bytes that can never change) rather than
// exported, so the spawn path keeps its own static table.
static const uint8 kEnemizerDamages[128] = {
  0, 1, 32, 255, 252, 251, 0, 0, 0, 2, 64, 4, 0, 0, 0, 0,
  0, 4, 64, 2, 3, 0, 0, 0, 0, 8, 64, 4, 0, 0, 0, 0,
  0, 16, 64, 8, 0, 0, 0, 0, 0, 16, 64, 8, 0, 0, 0, 0,
  0, 4, 64, 16, 0, 0, 0, 0, 0, 255, 64, 255, 252, 251, 0, 0,
  0, 4, 64, 255, 252, 251, 32, 0, 0, 100, 24, 100, 0, 0, 0, 0,
  0, 249, 250, 255, 100, 0, 0, 0, 0, 8, 64, 253, 4, 16, 0, 0,
  0, 8, 64, 254, 4, 0, 0, 0, 0, 16, 64, 253, 0, 0, 0, 0,
  0, 254, 64, 16, 0, 0, 0, 0, 0, 32, 64, 255, 0, 0, 0, 250,
};

// The rooms BOSSES owns: the thirteen boss super-tiles of kBossRooms plus the
// Thieves Town maiden room, all in randomizer.c.  A boss room's list is
// rewritten wholesale there, so nothing here may touch it.
static const uint16 kEnemizerSkipRooms[] = {
  0x06, 0x07, 0x1c, 0x29, 0x33, 0x45, 0x4d, 0x5a,
  0x6c, 0x90, 0xa4, 0xac, 0xc8, 0xde,
};

// Replacement type + 1 per byte offset of a record's start in the asset, or 0
// for "this record plays vanilla".  NULL while the row is NORMAL.
static uint8 *s_uw_swap, *s_ow_swap;

// Which of the 16 damage sources deal this type real damage.  The nibble is
// the type's subclass for that source; kEnemizerDamages turns it into hearts
// (1..100), a status effect (0xf9..0xff) or nothing.  0xfd, "incinerated", is
// a kill, so it counts; the stun / freeze / faerie / blob effects do not.
static uint16 KillMask(int t) {
  uint16 m = 0;
  if ((t + 1) * 16 > (int)kEnemyDamageData_SIZE * 2)
    return 0;
  for (int s = 0; s < 16; s++) {
    int i = t * 16 + s;
    int sub = (i & 1) ? (kEnemyDamageData[i >> 1] & 0xf) : (kEnemyDamageData[i >> 1] >> 4);
    if (sub > 7)
      continue;
    uint8 v = kEnemizerDamages[s * 8 + sub];
    if ((v >= 1 && v <= 100) || v == 0xfd)
      m |= (uint16)(1 << s);
  }
  return m;
}

static bool IsShufflable(int t) {
  if ((unsigned)t >= 0xd8)     // absorbables, key drops, containers, heart pieces
    return false;
  if (t == 0x79 || t == 0xac)  // the bee and the apples BONK DROPS arms
    return false;
  if ((t + 1) * 16 > (int)kEnemyDamageData_SIZE * 2)
    return false;              // no damage row = no provable kill mask
  return IsEnemy(t);
}

typedef struct EnemySlot {
  int off;       // byte offset of the 3-byte record inside the asset
  uint16 mask;   // kill mask of the type standing there
  uint8 sheet;   // the sprite sheet the record's room / area loads
  uint8 type;
} EnemySlot;

#define kEnemizerMaxSlots 2048
static EnemySlot s_eslots[kEnemizerMaxSlots];

// 0xff = no room seen yet, 0xfe = two rooms with different sheets share this
// record (both assets are deduplicated, so that does happen) - such a record
// is left alone, because one shuffle cannot be right for both.
#define kSheetNone 0xff
#define kSheetConflict 0xfe

static void MarkSheet(uint8 *map, int off, int sheet) {
  if (map[off] == kSheetNone)
    map[off] = (uint8)sheet;
  else if (map[off] != (uint8)sheet)
    map[off] = kSheetConflict;
}

// Both sprite assets are built with append_scan_bytes (assets/
// compile_resources.py), which reuses any byte run it already emitted - so a
// second list can start in the MIDDLE of another one, and there is no
// guarantee the two agree on where a 3-byte record begins.  Every byte gets a
// role while the lists are walked; a record is only ever touched when its own
// three bytes are unanimously "start, inner, inner", which makes it impossible
// to rewrite a byte another list reads as a coordinate.
enum { kRoleNone = 0, kRoleStart, kRoleInner, kRoleClash };

static void MarkRole(uint8 *role, int off, int want) {
  if (role[off] == kRoleNone)
    role[off] = (uint8)want;
  else if (role[off] != (uint8)want)
    role[off] = kRoleClash;
}

static bool RoleOk(const uint8 *role, int off) {
  return role[off] == kRoleStart && role[off + 1] == kRoleInner && role[off + 2] == kRoleInner;
}

// Fisher-Yates each (sheet, kill mask) group's TYPES across that group's
// slots.  A group holding one type is skipped: shuffling it is a no-op.
static int ShuffleGroups(EnemySlot *v, int n, uint8 *swap) {
  static uint8 done[kEnemizerMaxSlots];
  static int idx[kEnemizerMaxSlots];
  static uint8 types[kEnemizerMaxSlots];
  int swapped = 0;
  memset(done, 0, (size_t)n);
  for (int i = 0; i < n; i++) {
    if (done[i])
      continue;
    int m = 0;
    for (int j = i; j < n; j++) {
      if (done[j] || v[j].sheet != v[i].sheet || v[j].mask != v[i].mask)
        continue;
      done[j] = 1;
      idx[m] = j;
      types[m] = v[j].type;
      m++;
    }
    bool multi = false;
    for (int a = 1; a < m; a++)
      multi = multi || types[a] != types[0];
    if (!multi)
      continue;
    ShuffleBytes(types, m);
    for (int a = 0; a < m; a++) {
      if (types[a] == v[idx[a]].type)
        continue;
      swap[v[idx[a]].off] = (uint8)(types[a] + 1);
      swapped++;
    }
  }
  return swapped;
}

// The underworld: pass 0 blocks the rooms other rounds own, pass 1 learns each
// record's sheet, then one collecting walk.
static int BuildUnderworld(void) {
  int size = (int)kDungeonSprites_SIZE;
  int nrooms = (int)(kDungeonSpriteOffs_SIZE / 2);
  uint8 *sheet = (uint8 *)malloc((size_t)size);
  uint8 *role = (uint8 *)calloc((size_t)size, 1);
  if (!sheet || !role) {
    free(sheet); free(role);
    return 0;
  }
  memset(sheet, kSheetNone, (size_t)size);

  for (int pass = 0; pass < 2; pass++) {
    for (int room = 0; room < nrooms; room++) {
      int off = kDungeonSpriteOffs[room];
      if (off < 0 || off >= size)
        continue;
      bool skip = false;
      for (size_t i = 0; i < arraysize(kEnemizerSkipRooms); i++)
        skip = skip || kEnemizerSkipRooms[i] == room;
      // A 0xe4 die-action marker anywhere in the list means this room owns one
      // of the fourteen KEY DROPS; leave the whole room vanilla.
      for (int i = off + 1; i + 2 < size && kDungeonSprites[i] != 0xff; i += 3)
        skip = skip || (kDungeonSprites[i + 2] == 0xe4 &&
                        (kDungeonSprites[i] == 0xfe || kDungeonSprites[i] == 0xfd));
      if (pass == 0 ? !skip : skip)
        continue;
      int hdr = kDungeonRoomHeadersOffs[room];
      int sh = (hdr + 3 < (int)kDungeonRoomHeaders_SIZE) ? kDungeonRoomHeaders[hdr + 3]
                                                         : kSheetConflict;
      for (int i = off + 1; i + 2 < size && kDungeonSprites[i] != 0xff; i += 3) {
        MarkSheet(sheet, i, pass == 0 ? kSheetConflict : sh);
        MarkRole(role, i, kRoleStart);
        MarkRole(role, i + 1, kRoleInner);
        MarkRole(role, i + 2, kRoleInner);
      }
    }
  }

  int n = 0;
  for (int room = 0; room < nrooms && n < kEnemizerMaxSlots; room++) {
    int off = kDungeonSpriteOffs[room];
    if (off < 0 || off >= size)
      continue;
    for (int i = off + 1; i + 2 < size && kDungeonSprites[i] != 0xff; i += 3) {
      int sh = sheet[i];
      if (sh >= kSheetConflict || !RoleOk(role, i))
        continue;
      sheet[i] = kSheetConflict;              // deduplicated asset: collect once
      if (kDungeonSprites[i + 1] >= 0xe0)     // overlord record, not a sprite
        continue;
      int t = kDungeonSprites[i + 2];
      if (!IsShufflable(t))
        continue;
      if (n >= kEnemizerMaxSlots)
        break;
      s_eslots[n].off = i;
      s_eslots[n].mask = KillMask(t);
      s_eslots[n].sheet = (uint8)sh;
      s_eslots[n].type = (uint8)t;
      n++;
    }
  }
  free(sheet);
  free(role);
  return ShuffleGroups(s_eslots, n, s_uw_swap);
}

// The overworld sheet for an area, the way Overworld_LoadGFXAndScreenSize
// reads it: overworld_sprite_gfx is kOverworldSpriteGfx + progress * 64 for
// the light world and + 0xc0 for the dark one.  Areas 0x80 and up (the special
// screens) take their sheet from elsewhere and are left alone.
static int OwSheet(int base, int area) {
  int i = (area < 0x40) ? base * 64 + area :
          (area < 0x80) ? 0xc0 + (area - 0x40) : -1;
  if (i < 0 || i >= (int)kOverworldSpriteGfx_SIZE)
    return -1;
  int sh = kOverworldSpriteGfx[i];
  return sh >= kSheetConflict ? -1 : sh;
}

static int BuildOverworld(void) {
  int size = (int)kOverworldSprites_SIZE;
  int nent = (int)(kOverworldSpriteOffs_SIZE / 2);
  uint8 *sheet = (uint8 *)malloc((size_t)size);
  uint8 *role = (uint8 *)calloc((size_t)size, 1);
  if (!sheet || !role) {
    free(sheet); free(role);
    return 0;
  }
  memset(sheet, kSheetNone, (size_t)size);

  for (int e = 0; e < nent; e++) {
    int off = kOverworldSpriteOffs[e];
    int sh = OwSheet(e / 144, e % 144);
    if (off < 0 || off >= size)
      continue;
    for (int i = off; i + 2 < size && kOverworldSprites[i] != 0xff; i += 3) {
      MarkSheet(sheet, i, sh < 0 ? kSheetConflict : sh);
      MarkRole(role, i, kRoleStart);
      MarkRole(role, i + 1, kRoleInner);
      MarkRole(role, i + 2, kRoleInner);
    }
  }

  int n = 0;
  for (int e = 0; e < nent && n < kEnemizerMaxSlots; e++) {
    int off = kOverworldSpriteOffs[e];
    if (off < 0 || off >= size)
      continue;
    for (int i = off; i + 2 < size && kOverworldSprites[i] != 0xff; i += 3) {
      int sh = sheet[i];
      if (sh >= kSheetConflict || !RoleOk(role, i))
        continue;
      sheet[i] = kSheetConflict;              // collect each record once
      int t = kOverworldSprites[i + 2];
      if (t == 0xf4 || !IsShufflable(t))      // 0xf4 is Overworld_LoadSprites' own marker
        continue;
      if (n >= kEnemizerMaxSlots)
        break;
      s_eslots[n].off = i;
      s_eslots[n].mask = KillMask(t);
      s_eslots[n].sheet = (uint8)sh;
      s_eslots[n].type = (uint8)t;
      n++;
    }
  }
  free(sheet);
  free(role);
  return ShuffleGroups(s_eslots, n, s_ow_swap);
}

static void EnemyShuffleClear(void) {
  free(s_uw_swap); s_uw_swap = NULL;
  free(s_ow_swap); s_ow_swap = NULL;
}

static int EnemyShuffleBuild(int seed) {
  EnemyShuffleClear();
  s_uw_swap = (uint8 *)calloc(kDungeonSprites_SIZE ? kDungeonSprites_SIZE : 1, 1);
  s_ow_swap = (uint8 *)calloc(kOverworldSprites_SIZE ? kOverworldSprites_SIZE : 1, 1);
  if (!s_uw_swap || !s_ow_swap) {
    EnemyShuffleClear();
    return 0;
  }
  RndSeed(seed, 0x45530000u);          // "ES"
  int n = BuildUnderworld() + BuildOverworld();
  if (!n)
    EnemyShuffleClear();
  return n;
}

uint8 Enemizer_SwapUW(const uint8 *rec) {
  if (s_uw_swap) {
    size_t off = (size_t)(rec - kDungeonSprites);
    if (off < kDungeonSprites_SIZE && s_uw_swap[off])
      return (uint8)(s_uw_swap[off] - 1);
  }
  return rec[2];
}

uint8 Enemizer_SwapOW(const uint8 *rec) {
  if (s_ow_swap) {
    size_t off = (size_t)(rec - kOverworldSprites);
    if (off < kOverworldSprites_SIZE && s_ow_swap[off])
      return (uint8)(s_ow_swap[off] - 1);
  }
  return rec[2];
}

// ===========================================================================
// ENEMY COLOR (the "enemypalette" row) - a seeded remap of the SPRITE
// palettes.  Cosmetic, and this fork's own: the reference has no enemy-palette
// setting at all (its ow_palettes / uw_palettes are BACKGROUND palettes and
// are not offered here), so the row never reaches the dump either.
//
// Only the enemy halves of CGRAM move:
//   kPalette_MainSpr    -> SP1..SP4 (Palette_Load_SpriteMain, 2 worlds x 4
//                          rows x 15 colors), the main enemy palettes
//   kPalette_SpriteAux1 -> SP5L / SP6L (24 rows x 7), the per-room enemy aux
// Link's armour and gloves (kPal_ArmorGloves, SP7), the sword and shield
// (kPal_Sword / kPal_Shield), the HUD (Palette_Load_HUD, where HEART COLOR
// lives) and the message palettes are all other rows out of other assets and
// are not touched.
// ===========================================================================

enum { kEcol_Normal, kEcol_Shuffled, kEcol_Random, kEcol_Count };
static const char *const kEcolNames[kEcol_Count] = {"NORMAL", "SHUFFLED", "RANDOM"};

static uint16 *s_pal_main, *s_pal_aux1;

static void PermuteRows(uint16 *v, int rows, int cols) {
  for (int i = rows - 1; i > 0; i--) {
    int j = (int)(RndNext() % (uint32)(i + 1));
    for (int c = 0; c < cols; c++) {
      uint16 t = v[i * cols + c];
      v[i * cols + c] = v[j * cols + c];
      v[j * cols + c] = t;
    }
  }
}

// SNES BGR555.  Rotating the three 5-bit channels is a hue change that can
// never leave the gamut and never crushes a palette to black or white, so a
// RANDOM row stays as readable as the vanilla one.
static const uint8 kHueRot[6][3] = {
  {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
};

static void HueRows(uint16 *v, int rows, int cols) {
  for (int r = 0; r < rows; r++) {
    const uint8 *p = kHueRot[RndNext() % 6];
    for (int c = 0; c < cols; c++) {
      uint16 col = v[r * cols + c];
      uint8 ch[3] = { (uint8)(col & 31), (uint8)(col >> 5 & 31), (uint8)(col >> 10 & 31) };
      v[r * cols + c] = (uint16)(ch[p[0]] | ch[p[1]] << 5 | ch[p[2]] << 10);
    }
  }
}

static void PaletteClear(void) {
  free(s_pal_main); s_pal_main = NULL;
  free(s_pal_aux1); s_pal_aux1 = NULL;
}

static void PaletteBuild(int mode, int seed) {
  PaletteClear();
  if (mode == kEcol_Normal)
    return;
  // 120 = 2 worlds x 4 rows x 15, 168 = 24 rows x 7.  A rebuilt assets file
  // with different shapes simply leaves the palettes alone.
  size_t nm = kPalette_MainSpr_SIZE / 2, na = kPalette_SpriteAux1_SIZE / 2;
  if (nm != 120 || na != 168)
    return;
  s_pal_main = (uint16 *)malloc(nm * 2);
  s_pal_aux1 = (uint16 *)malloc(na * 2);
  if (!s_pal_main || !s_pal_aux1) {
    PaletteClear();
    return;
  }
  memcpy(s_pal_main, kPalette_MainSpr, nm * 2);
  memcpy(s_pal_aux1, kPalette_SpriteAux1, na * 2);
  RndSeed(seed, 0x50430000u);          // "PC"
  if (mode == kEcol_Shuffled) {
    PermuteRows(s_pal_main, 4, 15);        // light world SP1..SP4
    PermuteRows(s_pal_main + 60, 4, 15);   // dark world SP1..SP4
    PermuteRows(s_pal_aux1, 24, 7);
  } else {
    HueRows(s_pal_main, 8, 15);
    HueRows(s_pal_aux1, 24, 7);
  }
}

const uint16 *Enemizer_PalMainSpr(const uint16 *src) {
  if (s_pal_main) {
    ptrdiff_t o = src - (const uint16 *)kPalette_MainSpr;
    if (o >= 0 && (size_t)o < kPalette_MainSpr_SIZE / 2)
      return s_pal_main + o;
  }
  return src;
}

const uint16 *Enemizer_PalAux1Spr(const uint16 *src) {
  if (s_pal_aux1) {
    ptrdiff_t o = src - (const uint16 *)kPalette_SpriteAux1;
    if (o >= 0 && (size_t)o < kPalette_SpriteAux1_SIZE / 2)
      return s_pal_aux1 + o;
  }
  return src;
}

// Rebuilt from the loaded file by Enemizer_Apply, alongside the stat tables.
static void Enemizer_ApplyExtras(void) {
  EnemyShuffleClear();
  PaletteClear();
  if (!Randomizer_FileEnabled())
    return;                              // a vanilla file plays vanilla
  int shuf = Randomizer_FileOptValue("enemyshuffle");
  int ecol = Randomizer_FileOptValue("enemypalette");
  if (shuf < 0 || shuf >= kEnem_Count) shuf = kEnem_Normal;
  if (ecol < 0 || ecol >= kEcol_Count) ecol = kEcol_Normal;
  int seed = Randomizer_FileSeed();
  if (shuf == kEnem_Shuffled) {
    int n = EnemyShuffleBuild(seed);
    printf("[enemizer] SHUFFLED: %d sprites swapped%c", n, 10);
  }
  if (ecol != kEcol_Normal) {
    PaletteBuild(ecol, seed);
    printf("[enemizer] palettes=%s seed=%d%c", kEcolNames[ecol], seed, 10);
  }
  if (shuf != kEnem_Normal || ecol != kEcol_Normal)
    fflush(stdout);
}

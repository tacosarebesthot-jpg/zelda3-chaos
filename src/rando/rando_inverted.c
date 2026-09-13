/* rando_inverted.c - the WORLD/mode gate, and the survey behind it.
 *
 * See rando_inverted.h for why `inverted` is not one of the START values.
 * This file is the gate (about forty lines at the bottom) plus the survey
 * that decided it, kept here so the next round does not have to redo it.
 *
 * HOW TO REPRODUCE THE SURVEY
 *   python_embed\python.exe randomizer_ref\dump_logic.py
 *       --repo randomizer_ref\ALttPDoorRandomizer --out <dir>
 *       --seed 1234 --logic noglitches --mode inverted
 *   generates cleanly: 933 regions / 2224 edges / 366 locations,
 *   flatten 100.0%, 0 opaque nodes, 0 unhandled entrance entries.
 *
 * IS THE ROW PRESENTATION-ONLY?  No.  Against the same seed's open dump:
 * 260 edges appear, 256 disappear and 1579 keep their endpoints but change
 * rule; locations.json loses exactly one name ("Flute Activation").  It is a
 * real signature row, which is the whole problem: the fill would prove a seed
 * winnable in a world the engine is not running.
 *
 * FILL SIDE (rando_load.c / rando_fill.c) IS ALREADY FINE.  This fork has no
 * InvertedRegions.py / InvertedEntrances.py at all - inverted is expressed as
 * `World.is_tile_swapped()` over the ordinary tables - so the region NAMES are
 * byte-for-byte the open set (933 names, zero added, zero removed), "Menu" and
 * "Links House" both present, and no new location names.  A loader that can
 * read an open dump can read an inverted one; what it must not do is BELIEVE
 * it, which is what the gate below is for.
 *
 * ---------------------------------------------------------------------------
 * EVERY Rom.py WRITE PLAIN `--mode inverted` MAKES, AND WHAT IT IS HERE
 * ---------------------------------------------------------------------------
 * Plain inverted means owMixed off, shuffle=vanilla, doorShuffle=vanilla.
 * Note that `is_tile_swapped(owid) == (mode == 'inverted')` for EVERY owid
 * then (BaseClasses.py), so every `if world.is_tile_swapped(...)` branch in
 * set_inverted_mode fires; inverted is not reducible to a subset.
 * Rom.py addresses below are PC unless written as $bb:aaaa (SNES).
 * "verified" = read out of zelda3_assets.dat and byte-compared, zero
 * mismatches, the way b96864f verified its three tables.
 *
 *  A. TABLES THE PORT OWNS - these would be transcription, and they check out
 *  ------------------------------------------------------------------------
 *  A1  $02:D8D2.. (7 starting points) -> kStartingPoint_* (assets 28..45).
 *      set_inverted_mode moves start 1 Sanctuary -> Dark Sanctuary (room
 *      $0112) and, under is_bombshop_start, start 0 Links House -> Big Bomb
 *      Shop (room $011C).  VERIFIED: rooms[0] == 0x0104, rooms[1] == 0x0012,
 *      all seventeen tables 7 entries wide, and every value Rom.py patches is
 *      a partial-byte edit of exactly those (e.g. it writes only the low byte
 *      $1C at $02D8D2 to turn 0x0104 into 0x011C).  CAVEAT: the ROM block
 *      orders the pairs scrollX, scrollY, playerY, playerX, cameraY, cameraX
 *      - the Y/X of the player and camera pairs is the other way round from
 *      the order these are DECLARED in assets.h.  Map by meaning, not by
 *      position.  (Derived from the dark-sanctuary constants: room $0112 puts
 *      y in $22xx and x in $04xx, and $02D936 = 0x229A is a y.)
 *  A2  $02:E849.. (17 x uint16, stride $22 per table) -> kBirdTravel_*
 *      (assets 113..122): 8 flute spots, then spot 9 (the pyramid, area
 *      0x5B), then the 8 whirlpool destinations.  Inverted writes owid+0x40
 *      for all eight spots, and $02:E859 (spot 9) = 0x001B.  VERIFIED:
 *      kBirdTravel_ScreenIndex[0..7] == FluteShuffle.default_flute_connections
 *      exactly; [8] == 0x5b; [9..16] is kWhirlpoolAreas as a set; and all ten
 *      fields of all eight spots equal flute_data's vanilla entries, zero
 *      mismatches.  CAVEAT: Rom.py writes unknown_1 / unknown_2 as int16 at
 *      stride 2, but the port keeps kBirdTravel_Unk1 / _Unk3 as int8[17] -
 *      write the low byte only.
 *  A3  WHIRLPOOLS ARE NOT AN INVERTED WRITE.  Rom.py only touches
 *      $02:EA5C when world.owWhirlpoolShuffle is on, which plain inverted
 *      does not set.  kWhirlpoolAreas stays vanilla.
 *  A4  0xDB96F / 0xDBA71 / 0xDBB73 (129 entries each) ->
 *      kOverworld_Entrance_Area / _Pos / _Id (assets 124/125/126).  VERIFIED
 *      by stride (0xDB96F + 2*129 == 0xDBA71 + 2*129 == 0xDBB73) and by
 *      Area[0x35] == 0x5B, Pos[0x35] == 0x0D9C, Id[0x35] == 0x36.  Inverted
 *      moves the pyramid door to Hyrule Castle: Area[0x35] = 0x1B,
 *      Pos[0x35] = 0x011C.  The entrance loader of b96864f overrides only
 *      _Id, so this pair would be new.
 *  A5  0x15AEE.. (79 exit rows) -> kExitData_*.  The ROM block is, in order,
 *      Rooms(0x15AEE, u16) ScreenIndex(0x15B8C, u8) Map16LoadSrcOff(0x15BDB)
 *      ScrollY(0x15C79) ScrollX(0x15D17) YCoord(0x15DB5) XCoord(0x15E53)
 *      CameraYScroll(0x15EF1) CameraXScroll(0x15F8F) Unk1(0x1602D, i8)
 *      Unk3(0x1607C, i8) NormalDoor(0x160CB) FancyDoor(0x16169).  VERIFIED
 *      twice over: the strides land exactly, and row 0x37 read out of the
 *      asset file equals EntranceData.door_addresses['Pyramid Entrance']'s
 *      13-tuple field for field.
 *      CORRECTION TO b96864f's REPORT: the Inverted Pyramid Entrance's exit
 *      data differs from vanilla row 0x37 in 9 of the 12 non-room fields
 *      (ow_area, vram_loc, scroll_y, scroll_x, link_y, camera_y, unknown_1,
 *      unknown_2, door_2) but the ROOM IS THE SAME, 0x0010.  RandoExitRecord
 *      is therefore already the right shape - no room field is needed, and
 *      LoadOverworldFromDungeon's search by room still finds the row.
 *  A6  0x15AEE + 2*0x38 / + 2*0x25, and 0xDBB73 + 0x23 / + 0x36: the AT/GT
 *      swap (is_atgt_swapped is true in inverted).  Same two tables as A4/A5.
 *  A7  0x15AEE + 2*0x06 and the twelve rows under it: the post-Agahnim
 *      Hyrule Castle spawn moves to area 0x1B.  Same table as A5.
 *
 *  B. SINGLE BRANCH BYTES IN VANILLA CODE - the port has each as a plain `if`
 *  ------------------------------------------------------------------------
 *  Rom.py flips one opcode byte; the port would need one condition.  Each
 *  address below was resolved to LoROM $bb:aaaa and matched to the port
 *  function whose `// bbaaaa` marker contains it.
 *  B1  0x03A943 = $07:A943 -> src/player.c LinkItem_Mirror (// 87a91a), the
 *      `!(overworld_screen_index & 0x40)` in the refuse-with-SFX test.  This
 *      IS the mirror direction flip.  (D0 = dark->light, F0 = light->dark.)
 *      Note the port already has an unrelated escape hatch on that same line,
 *      g_config MirrorToDarkworld / kFeatures0_MirrorToDarkworld, which
 *      allows BOTH directions; inverted needs the other one, not both.
 *  B2  0x03A96D = $07:A96D -> src/player.c DoSwordInteractionWithTiles_Mirror
 *      (// 87a95c), the `if (last_light_vs_dark_world)` that stashes Link's
 *      position into bird_travel_*[15].
 *  B3  0x03A9A7 = $07:A9A7 -> src/player.c LinkState_CrossingWorlds
 *      (// 87a9b1).  Rom.py writes 0xD0 here in EVERY mode - not an inverted
 *      difference.
 *  B4  0x02AF79 = $05:AF79 -> src/sprite_main.c Sprite_6C_MirrorPortal
 *      (// 85af75), the vortex direction.
 *  B5  $02:83E0 -> src/overworld.c PreOverworld_LoadProperties (// 8283c7),
 *      which is where `if (!(overworld_screen_index & 0x40))
 *      Sprite_InitializeMirrorPortal()` lives - the residual portal.
 *  B6  $02:B34D -> src/overworld.c MirrorWarp_LoadSpritesAndColors
 *      (// 82b334), the other residual-portal write.
 *  B7  $07:A3F4 -> src/player.c LinkItem_Flute (// 87a3db), the duck.
 *  B8  $08:D40C -> src/ancilla.c MorphPoof_Draw (// 88d3fd).
 *  B9  $0A:BFBB -> src/messaging.c WorldMap_HandleSprites (// 8abf66), the
 *      mirror-portal blip on the world map.
 *  B10 $02:80A6 -> src/select_file.c Intro_ValidateSram (// 828054), "use
 *      starting point prompt instead of start at pyramid".
 *  B11 $06:DB78 (dark-style portal) and $0D:B3C5 (vortex) have NO annotated
 *      function in this port - banks 06 and 0D have no `// 86db..` / `// 8db3`
 *      marker at all.  Unresolved.
 *
 *  C. WHAT ACTUALLY BLOCKS INVERTED
 *  ------------------------------------------------------------------------
 *  C1  0x18004A = 1, "Inverted mode".  $180000+ is not vanilla ROM: it is a
 *      data page belonging to the randomizer's own 65816 patch, shipped as
 *      randomizer_ref/ALttPDoorRandomizer/data/base2current.bps.  That patch
 *      has no source anywhere in this tree (there is not one .asm file), and
 *      there is no base zelda3.sfc here to apply it to and disassemble, so
 *      there is no way to learn what it does with the flag and no way to
 *      byte-compare a C reimplementation against anything.  Everything the
 *      inverted ROM is famous for that is NOT in list A or B hangs off this
 *      byte: Link is the bunny in the LIGHT world instead of the dark one at
 *      every world-transition site, the Ganon warp, the save-and-quit
 *      destination, the crystal-locked Agahnim/Ganon tower door.  THIS IS THE
 *      PIECE THAT BLOCKS THE ROW.
 *  C2  Same page, same problem: 0x180089 (open TR after exit), 0x18008F
 *      (AT/GT swapped), 0x180169 = 2, 0x1800A2 / 0x180174 (fix_fake_world),
 *      0x180240..0x180277 (patch_shuffled_dark_sanc / patch_shuffled_bomb_shop
 *      relocate the start when the entrance shuffle moved it), and 0x180340,
 *      the extra address the Inverted Pyramid Hole carries in entrances.json's
 *      `extra[]` and the reason RandoEntrance_Load refuses inverted.
 *  C3  0x0086E = [0x5C,0x00,0xA0,0xA1] - a raw JML into that same patch (the
 *      "TR tail").  Pure asm, no data counterpart at all.
 *  C4  0x153A70, 0x82 bytes ("apply inverted map changes") plus 0x1539B0 and
 *      0x153C80.  Also not vanilla ROM - these are the overworld-shuffle
 *      patch's per-area map flags, and the alternate map data they select
 *      lives in the same .bps.
 *  C5  The Hyrule-Castle hole that replaces the pyramid hole in inverted:
 *      0x00D009 / 0x00D0E8 / 0x00D1C7 (hole graphics), 0x1BE8DA (a palette
 *      colour), 0x0FF1C8 and 0x0FA480 (map16 definitions), 0x1BB810 / 0x1BB836
 *      (the hole entrance), 0x1AF730 (the 32-byte hole mask position) and the
 *      whole retreat-bat block at 0x1AF504..0x1AF58C / 0x1AF696 / 0x1AF6B2.
 *      These ARE vanilla ROM regions, but the port stores them cooked into
 *      different assets (map16 definitions, overlay tables, sprite OAM
 *      tables), so each one is its own mapping round.  Without them the
 *      inverted pyramid hole is an invisible hole in the wrong overworld
 *      screen.
 *  C6  The "add warp under rock" / "remove secret portal" pairs for areas
 *      0x05, 0x07, 0x10, 0x2F, 0x30, 0x33 and 0x35: 0x1BC3DF, 0x1BC387,
 *      0x1BC428, 0x1BC43A, 0x1BC590, 0x1BC5A1, 0x1BC5B1, 0x1BC5C7, 0x1BC655,
 *      0x1BC67A, 0x1BC80D, 0x1BC81E, 0x1BC85A, 0x1BD1D8, 0x1BD1DD.  The
 *      overworld secret / warp-tile tables.  Untouched here on purpose: a
 *      parallel branch owns that path this round.
 *  C7  `del ow_enemy_table[0xab][5]` - remove the castle gate warp - and the
 *      peg-puzzle moves at $04:E7A3 / $04:E7E4 for Turtle Rock.
 *  C8  Free consequence worth writing down: inverted removes the "Flute
 *      Activation" location entirely (is_tile_swapped(0x18) makes the ocarina
 *      start activated).  Any future inverted round must force the FLUTE row
 *      to ACTIVE, or the fill hands out a flute the player can never turn on.
 *
 * A, B and C8 are a day's work with real verification behind every line.  C1
 * is not deliverable at all from this tree, and C4/C5/C6/C7 are each their own
 * round.  So `inverted` stays out of the values list, and this gate makes sure
 * an inverted rule folder cannot be filled by accident.
 */
#include "rando_inverted.h"
#include "rando_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_inv_active;
static char s_inv_mode[24];

bool RandoInverted_NameIsInverted(const char *mode) {
  return mode != NULL && strcmp(mode, "inverted") == 0;
}

void RandoInverted_Reset(void) {
  s_inv_active = false;
  s_inv_mode[0] = 0;
}

bool RandoInverted_Active(void) { return s_inv_active; }

const char *RandoInverted_Mode(void) { return s_inv_mode; }

static char *InvReadAll(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0 || n > (32 << 20)) { fclose(f); return NULL; }
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  return buf;
}

// Pull one mode string out of <dir>/<file>: meta.json keeps it under
// "settings", entrances.json at the top level.  Returns 1 when it found one.
static int InvReadMode(const char *dir, const char *file, const char *section,
                       char *out, size_t outsz) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, file);
  char *text = InvReadAll(path);
  if (!text) return 0;
  JsonValue *root = Json_Parse(text, NULL, 0);
  free(text);
  if (!root) return 0;
  const JsonValue *obj = section ? Json_Get(root, section) : root;
  const char *mode = Json_AsString(Json_Get(obj, "mode"));
  int found = 0;
  if (mode && *mode) {
    snprintf(out, outsz, "%s", mode);
    found = 1;
  }
  Json_Free(root);
  return found;
}

void RandoInverted_Load(const char *dir) {
  RandoInverted_Reset();
  if (!dir || !*dir) return;
  if (!InvReadMode(dir, "meta.json", "settings", s_inv_mode, sizeof s_inv_mode))
    InvReadMode(dir, "entrances.json", NULL, s_inv_mode, sizeof s_inv_mode);
  if (!RandoInverted_NameIsInverted(s_inv_mode)) return;   // open / standard / unknown: nothing changes
  s_inv_active = true;
  printf("[inverted] %s was dumped with mode=inverted, which this engine does "
         "not implement (see src/rando/rando_inverted.c): the fill is REFUSED "
         "rather than played in the wrong world%c", dir, 10);
  fflush(stdout);
}

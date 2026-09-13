/* rando_entrance.h - ENTRANCE SHUFFLE: the overworld door / exit / hole wiring.
 *
 * The reference randomizer (`--setting shuffle=simple|restricted|full|...`)
 * rewires which overworld door leads to which interior by rewriting three ROM
 * tables (Rom.py, "patch entrance/exits/holes").  The port reads the very same
 * three tables out of zelda3_assets.dat, so the whole round is "read the
 * reference's finished wiring out of entrances.json and put a live override in
 * front of those three reads":
 *
 *   kOverworld_Entrance_Id[slot]  which_entrance when Link walks into a door
 *                                 (Overworld_UseEntrance, src/overworld.c)
 *   kFallHole_Entrances[slot]     which_entrance when Link falls in a hole
 *                                 (Overworld_GetPitDestination)
 *   kExitData_*[row]              where Link comes back out
 *                                 (LoadOverworldFromDungeon)
 *
 * Interiors that live in rooms 0x100..0x17f (every one-room cave, shop and
 * house except Link's House) never touch the exit table at all: the engine
 * sends them back through LoadCachedEntranceProperties, i.e. to the door Link
 * actually walked in through, which is exactly what a shuffled cave wants.
 * Only the 60 interiors that own a real exit row need the third table.
 *
 * Nothing here is live unless a file's ENTRANCES row is non-vanilla AND its
 * rule folder carried an entrances.json saying so; RandoEntrance_Active() is
 * the single gate, and with it clear every accessor returns the asset value.
 */
#ifndef RANDO_ENTRANCE_H
#define RANDO_ENTRANCE_H

#include "../types.h"

// One row of the exit table, as LoadOverworldFromDungeon consumes it.  The
// room id is deliberately absent: the reference never rewrites it either, so a
// row keeps its identity and the engine's "find the row whose room is the room
// Link is standing in" search still works.
typedef struct RandoExitRecord {
  uint16 scroll_x, scroll_y;
  uint16 link_x, link_y;
  uint16 camera_x, camera_y;
  uint16 vram_loc;           // kExitData_Map16LoadSrcOff
  uint16 door_1, door_2;     // kExitData_NormalDoor / kExitData_FancyDoor
  uint8 ow_area;             // kExitData_ScreenIndex
  int8 unknown_1, unknown_3; // kExitData_Unk1 / kExitData_Unk3
} RandoExitRecord;

// Drop every override; the three accessors go back to the asset tables.
void RandoEntrance_Reset(void);
// Read <dir>/entrances.json and arm the wiring it describes.  A missing,
// unreadable, vanilla or inverted-mode file leaves everything vanilla; this is
// not an error (a rule folder built before this round simply has no file).
void RandoEntrance_Load(const char *dir);
bool RandoEntrance_Active(void);

// kOverworld_Entrance_Id[slot], remapped.
uint8 RandoEntrance_DoorId(int slot);
// kFallHole_Entrances[slot], remapped.
uint8 RandoEntrance_HoleId(int slot);
// The exit row `row` as the engine should use it: always filled from the asset
// tables first, then overlaid with the shuffle's record if there is one.
void RandoEntrance_ExitRecord(int row, RandoExitRecord *out);

#endif  // RANDO_ENTRANCE_H

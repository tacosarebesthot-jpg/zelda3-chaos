/* rando_entrance.c - ENTRANCE SHUFFLE: live overrides for the three tables
 * that decide where a door goes and where you come back out.  See the header
 * for the shape of the round; this file is only the loader plus three tiny
 * accessors, and it is included into randomizer.c's unity TU.
 *
 * WHY entrances.json AND NOT A SHUFFLE OF OUR OWN.  The reference's
 * EntranceShuffle2.py picks the pairing with the generator's RNG and the
 * logic dump (edges.json) is built on top of the pairing it picked, so the
 * engine must use THAT pairing or the fill's reachability proof is about a
 * different game.  dump_logic.py's dump_entrances() writes the finished
 * wiring exactly as Rom.py would have patched it; everything below is
 * transcription plus range and permutation checks.
 *
 * THE THREE TABLES, and why they are enough:
 *
 *   doors[]  -> kOverworld_Entrance_Id.  Overworld_UseEntrance finds the door
 *      slot by (area, position) and reads this byte into which_entrance;
 *      Dungeon_LoadEntrance then indexes every kEntranceData_* array with it.
 *      Remapping the byte therefore moves the whole interior - room, camera,
 *      music, palace index - with no further work.
 *   holes[]  -> kFallHole_Entrances.  Same idea for Overworld_GetPitDestination.
 *      One hole can own several table slots (the Pyramid hole owns three), so
 *      a hole entry lists all of them and they all get the same target.
 *   exits[]  -> kExitData_*.  LoadOverworldFromDungeon looks the row up by the
 *      room Link is standing in and reads the overworld spot to drop him at.
 *      The reference never rewrites the row's ROOM (Rom.py says so in a
 *      comment), so that search still finds the right row; only the
 *      destination fields move.  Rooms 0x100..0x17f (every one-room cave and
 *      house bar Link's House) never reach that code at all - they come back
 *      through LoadCachedEntranceProperties, i.e. to the door Link walked in
 *      through - which is why only 60 of the 79 rows ever appear here.
 *
 * VANILLA IS A NO-OP.  The reference connects a vanilla world with
 * connect_simple(), which sets no addresses and no target, so a vanilla dump's
 * doors[] is empty and `shuffle` reads "vanilla"; the loader then refuses to
 * arm and every accessor is a plain asset read.
 */
#include "rando_entrance.h"
#include "rando_inverted.h"
#include "rando_json.h"
#include "../assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// generous upper bounds - the real sizes come from the asset table and are
// checked against these once, at load
#define kEntDoorMax 160    // kOverworld_Entrance_Id: 129 in the vanilla assets
#define kEntHoleMax 32     // kFallHole_Entrances:     19
#define kEntExitMax 96     // kExitDataRooms:          79
#define kEntIdMax 256      // kEntranceData_rooms:    133

static bool s_ent_active;
static uint8 s_door_to[kEntDoorMax], s_door_has[kEntDoorMax];
static uint8 s_hole_to[kEntHoleMax], s_hole_has[kEntHoleMax];
static RandoExitRecord s_exit_rec[kEntExitMax];
static uint8 s_exit_has[kEntExitMax];

void RandoEntrance_Reset(void) {
  s_ent_active = false;
  memset(s_door_has, 0, sizeof s_door_has);
  memset(s_hole_has, 0, sizeof s_hole_has);
  memset(s_exit_has, 0, sizeof s_exit_has);
}

bool RandoEntrance_Active(void) { return s_ent_active; }

uint8 RandoEntrance_DoorId(int slot) {
  if (s_ent_active && (unsigned)slot < kEntDoorMax && s_door_has[slot])
    return s_door_to[slot];
  return kOverworld_Entrance_Id[slot];
}

uint8 RandoEntrance_HoleId(int slot) {
  if (s_ent_active && (unsigned)slot < kEntHoleMax && s_hole_has[slot])
    return s_hole_to[slot];
  return kFallHole_Entrances[slot];
}

void RandoEntrance_ExitRecord(int row, RandoExitRecord *out) {
  // always the asset row first: with the shuffle off this is exactly what
  // LoadOverworldFromDungeon read before this round existed
  out->scroll_x = kExitData_ScrollX[row];
  out->scroll_y = kExitData_ScrollY[row];
  out->link_x = kExitData_XCoord[row];
  out->link_y = kExitData_YCoord[row];
  out->camera_x = kExitData_CameraXScroll[row];
  out->camera_y = kExitData_CameraYScroll[row];
  out->vram_loc = kExitData_Map16LoadSrcOff[row];
  out->door_1 = kExitData_NormalDoor[row];
  out->door_2 = kExitData_FancyDoor[row];
  out->ow_area = kExitData_ScreenIndex[row];
  out->unknown_1 = kExitData_Unk1[row];
  out->unknown_3 = kExitData_Unk3[row];
  if (s_ent_active && (unsigned)row < kEntExitMax && s_exit_has[row])
    *out = s_exit_rec[row];
}

// ---------------------------------------------------------------------------

static char *EntReadAll(const char *path) {
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

static int EntJsonInt(const JsonValue *obj, const char *key, int def) {
  const JsonValue *v = Json_Get(obj, key);
  return (v && v->type == JSON_NUMBER) ? Json_AsInt(v) : def;
}

void RandoEntrance_Load(const char *dir) {
  RandoEntrance_Reset();
  char path[512];
  snprintf(path, sizeof path, "%s/entrances.json", dir);
  char *text = EntReadAll(path);
  if (!text) return;   // folder built before this round: entrances stay vanilla
  char err[128];
  JsonValue *root = Json_Parse(text, err, sizeof err);
  free(text);
  if (!root) {
    printf("[entrances] bad %s: %s%c", path, err, 10);
    return;
  }
  const char *js = Json_AsString(Json_Get(root, "shuffle"));
  const char *mode = Json_AsString(Json_Get(root, "mode"));
  char shuffle[32];
  snprintf(shuffle, sizeof shuffle, "%s", js ? js : "");
  if (!js || !strcmp(shuffle, "vanilla")) { Json_Free(root); return; }
  // INVERTED.  The wiring in this file would mostly carry it: the Inverted
  // Pyramid Entrance's exit data keeps vanilla row 0x37's ROOM, 0x0010 - nine
  // of the twelve destination fields move and the row's identity does not -
  // which is exactly the shape RandoExitRecord has.  Two loose ends remain:
  // door slot 0x35 moves from overworld area 0x5B to 0x1B, so kOverworld_
  // Entrance_Area / _Pos would want overrides beside the _Id one below, and
  // the Inverted Pyramid Hole carries an extra address 0x180340 that
  // dump_entrances parks in `extra[]` and no engine table answers.  Neither is
  // what stops the mode; src/rando/rando_inverted.c is the one place that
  // records what does.  Refuse here too so the entrance loader still stands
  // alone, and share the string test so the two gates cannot drift.
  if (RandoInverted_NameIsInverted(mode)) {
    printf("[entrances] %s is inverted: not supported, staying vanilla%c", path, 10);
    Json_Free(root);
    return;
  }
  // the asset tables have to be the sizes this code was written against
  int n_door = (int)kOverworld_Entrance_Id_SIZE;
  int n_hole = (int)kFallHole_Entrances_SIZE;
  int n_exit = (int)(kExitDataRooms_SIZE / 2);
  int n_id = (int)(kEntranceData_rooms_SIZE / 2);
  if (n_door <= 0 || n_door > kEntDoorMax || n_hole <= 0 || n_hole > kEntHoleMax ||
      n_exit <= 0 || n_exit > kEntExitMax || n_id <= 0 || n_id > kEntIdMax) {
    printf("[entrances] unexpected asset table sizes (%d/%d/%d/%d), staying vanilla%c",
           n_door, n_hole, n_exit, n_id, 10);
    Json_Free(root);
    return;
  }

  // ---- doors -------------------------------------------------------------
  // Invariant checked below: the ids the dump hands out are a PERMUTATION of
  // the ids the engine's own door table holds for the same slots.  That is the
  // vanilla round-trip test - it can only hold if the reference's entrance-id
  // space and the port's kEntranceData_* index space are the same numbering.
  int id_want[kEntIdMax], id_got[kEntIdMax];
  memset(id_want, 0, sizeof id_want);
  memset(id_got, 0, sizeof id_got);
  int ndoors = 0, bad = 0;
  const JsonValue *arr = Json_Get(root, "doors");
  if (arr && arr->type == JSON_ARRAY) {
    for (const JsonValue *v = arr->child; v; v = v->next) {
      int slot = EntJsonInt(v, "door", -1), target = EntJsonInt(v, "target", -1);
      if (slot < 0 || slot >= n_door || target < 0 || target >= n_id) {
        printf("[entrances] door out of range: slot %d target %d%c", slot, target, 10);
        bad++;
        continue;
      }
      if (s_door_has[slot]) { printf("[entrances] door slot %d listed twice%c", slot, 10); bad++; continue; }
      s_door_has[slot] = 1;
      s_door_to[slot] = (uint8)target;
      id_got[target]++;
      id_want[kOverworld_Entrance_Id[slot]]++;
      ndoors++;
    }
  }
  if (memcmp(id_want, id_got, sizeof id_want) != 0) {
    printf("[entrances] the dump's entrance ids are not a permutation of the "
           "engine's door table: staying vanilla%c", 10);
    bad++;
  }

  // ---- holes -------------------------------------------------------------
  int nholes = 0;
  memset(id_want, 0, sizeof id_want);
  memset(id_got, 0, sizeof id_got);
  arr = Json_Get(root, "holes");
  if (arr && arr->type == JSON_ARRAY) {
    for (const JsonValue *v = arr->child; v; v = v->next) {
      int target = EntJsonInt(v, "target", -1);
      const JsonValue *slots = Json_Get(v, "slots");
      if (target < 0 || target >= n_id || !slots || slots->type != JSON_ARRAY ||
          Json_Size(slots) < 1) {
        printf("[entrances] bad hole entry (target %d)%c", target, 10);
        bad++;
        continue;
      }
      // every slot of one hole must currently hold the same entrance, or the
      // dump's slot numbering does not line up with kFallHole_Entrances
      int van = -1, ok = 1;
      for (const JsonValue *s = slots->child; s; s = s->next) {
        int i = Json_AsInt(s);
        if (i < 0 || i >= n_hole || s_hole_has[i]) { ok = 0; break; }
        if (van < 0) van = kFallHole_Entrances[i];
        else if (van != kFallHole_Entrances[i]) { ok = 0; break; }
      }
      if (!ok) { printf("[entrances] hole slots do not line up with kFallHole_Entrances%c", 10); bad++; continue; }
      for (const JsonValue *s = slots->child; s; s = s->next) {
        int i = Json_AsInt(s);
        s_hole_has[i] = 1;
        s_hole_to[i] = (uint8)target;
      }
      id_got[target]++;
      id_want[van]++;
      nholes++;
    }
  }
  if (memcmp(id_want, id_got, sizeof id_want) != 0) {
    printf("[entrances] the dump's hole ids are not a permutation of the "
           "engine's fall-hole table: staying vanilla%c", 10);
    bad++;
  }

  // ---- exits -------------------------------------------------------------
  int nexits = 0;
  arr = Json_Get(root, "exits");
  if (arr && arr->type == JSON_ARRAY) {
    for (const JsonValue *v = arr->child; v; v = v->next) {
      int row = EntJsonInt(v, "index", -1);
      const JsonValue *d = Json_Get(v, "data");
      if (row < 0 || row >= n_exit || !d || d->type != JSON_OBJECT) {
        printf("[entrances] exit row out of range: %d%c", row, 10);
        bad++;
        continue;
      }
      if (s_exit_has[row]) { printf("[entrances] exit row %d listed twice%c", row, 10); bad++; continue; }
      RandoExitRecord *r = &s_exit_rec[row];
      r->scroll_x = (uint16)EntJsonInt(d, "scroll_x", kExitData_ScrollX[row]);
      r->scroll_y = (uint16)EntJsonInt(d, "scroll_y", kExitData_ScrollY[row]);
      r->link_x = (uint16)EntJsonInt(d, "link_x", kExitData_XCoord[row]);
      r->link_y = (uint16)EntJsonInt(d, "link_y", kExitData_YCoord[row]);
      r->camera_x = (uint16)EntJsonInt(d, "camera_x", kExitData_CameraXScroll[row]);
      r->camera_y = (uint16)EntJsonInt(d, "camera_y", kExitData_CameraYScroll[row]);
      r->vram_loc = (uint16)EntJsonInt(d, "vram_loc", kExitData_Map16LoadSrcOff[row]);
      r->door_1 = (uint16)EntJsonInt(d, "door_1", kExitData_NormalDoor[row]);
      r->door_2 = (uint16)EntJsonInt(d, "door_2", kExitData_FancyDoor[row]);
      r->ow_area = (uint8)EntJsonInt(d, "ow_area", kExitData_ScreenIndex[row]);
      r->unknown_1 = (int8)EntJsonInt(d, "unknown_1", kExitData_Unk1[row]);
      r->unknown_3 = (int8)EntJsonInt(d, "unknown_2", kExitData_Unk3[row]);
      s_exit_has[row] = 1;
      nexits++;
    }
  }

  const JsonValue *unh = Json_Get(root, "unhandled");
  int n_unh = (unh && unh->type == JSON_ARRAY) ? Json_Size(unh) : 0;
  Json_Free(root);
  if (bad || n_unh) {
    printf("[entrances] %s rejected (%d problems, %d unhandled): staying vanilla%c",
           shuffle, bad, n_unh, 10);
    RandoEntrance_Reset();
    return;
  }
  if (!ndoors && !nholes && !nexits) return;
  s_ent_active = true;
  printf("[entrances] %s: %d doors, %d holes, %d exit rows remapped%c",
         shuffle, ndoors, nholes, nexits, 10);
  fflush(stdout);
}

/* rando_inverted.h - the WORLD/mode gate.
 *
 * The reference randomizer takes `--mode open | standard | inverted`.  The
 * port offers the first two (randomizer.ini `start=`, the START row in the
 * settings menu, RulesDirFor's `_standard` folder suffix).  It does NOT offer
 * `inverted`, and this file is the one place that says so and enforces it.
 *
 * WHY NOT.  Inverted is not a table swap; it is a behaviour patch.  The
 * reference builds it out of three layers and only the first has anything to
 * compare against in zelda3_assets.dat:
 *
 *   1. vanilla ROM tables the port also owns as named assets - the starting
 *      points, the flute/bird-travel table, the overworld door and exit
 *      tables.  Those map cleanly (see the table in rando_inverted.c).
 *   2. single branch bytes in vanilla code - the mirror direction, the vortex
 *      direction, the map's portal indicator.  Those are ordinary `if`s in the
 *      port and are listed in rando_inverted.c with the exact function.
 *   3. writes into $180000+ and $153A70+, which are NOT vanilla ROM at all:
 *      they are data pages read by the randomizer's own 65816 patch, shipped
 *      as data/base2current.bps - a binary with no source in this tree and no
 *      base ROM here to apply it to.  $18004A ("Inverted mode") alone drives
 *      the bunny inversion, the Ganon warp, the save-and-quit destination and
 *      an unknown remainder.
 *
 * Layer 3 cannot be transcribed and cannot be verified the way every other
 * rando round in this port was verified (read the asset file, byte-compare,
 * zero mismatches).  A half-inverted engine playing an inverted logic dump is
 * an unwinnable seed, so `inverted` stays out of the START values and this
 * module refuses any rule folder that carries it.
 *
 * Nothing here touches open/standard: RandoInverted_Load leaves the flag
 * clear for them and every caller's behaviour is exactly what it was.
 */
#ifndef RANDO_INVERTED_H
#define RANDO_INVERTED_H

#include "../types.h"

// Drop the gate; RandoInverted_Active() goes back to false.
void RandoInverted_Reset(void);
// Read <dir>/meta.json (settings.mode), falling back to <dir>/entrances.json
// ("mode").  A folder with neither, or with an unreadable one, is treated as
// NOT inverted - that is what every folder this port builds actually is.
// Prints one banner when it does find inverted.
void RandoInverted_Load(const char *dir);
// true only when the loaded rule folder says mode=inverted.
bool RandoInverted_Active(void);
// The mode string as the folder reported it ("open" / "standard" /
// "inverted"), or "" when no folder has been read.
const char *RandoInverted_Mode(void);
// The one definition of "this mode string means inverted", shared with
// rando_entrance.c so the two gates cannot drift apart.
bool RandoInverted_NameIsInverted(const char *mode);

#endif  // RANDO_INVERTED_H

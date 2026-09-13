#pragma once
#include "types.h"

// Runtime dialogue override.
//
// A plain "dialogue.txt" next to the exe replaces in-game messages by number
// using the same "N: text" format as assets/dialogue.txt (control codes in
// square brackets: [Name] [2] [3] [Scroll] [Waitkey] [Speed 03] ...).
// Lines without any [1]/[2]/[3]/[Scroll] code are word-wrapped to the box
// automatically using the real font widths, so a joke line can be typed as
// one sentence.  "{chatter}" becomes the name of a recent chatter (or a name
// from the "chatters: a, b, c" pool line when nobody has chatted yet).
// Messages missing from the file keep the built-in text.

void DialogueOverride_Init(void);
// Builds the command stream for message |index| into |dst| (capacity |cap|).
// Returns the byte count, or 0 when there is no override for that message.
int DialogueOverride_Get(int index, uint8 *dst, int cap);
int DialogueOverride_Count(void);
// MODS page TEXT row: JOKES (dialogue.txt) / NORMAL, persisted in text.ini
bool DialogueOverride_Enabled(void);          // mode != NORMAL
int DialogueOverride_Mode(void);              // 0 NORMAL, 1 CLEAN (no cursing), 2 DIRTY
const char *DialogueOverride_ModeLabel(void);
bool DialogueOverride_Available(void);   // a dialogue.txt was loaded
void DialogueOverride_Toggle(void);
// A chatter seen live: added to the {chatter} pool and appended to chatters.txt.
void DialogueOverride_NoteChatter(const char *name);
// HINTS (telepathic tiles + fortune teller): 0 NORMAL = the original text,
// 1 REAL = true placements from this seed's fill, 2 JOKES = dialogue.txt
// lines tagged h ("182h: ..."). Persisted in text.ini hints=.
int DialogueOverride_HintsMode(void);
const char *DialogueOverride_HintsLabel(void);
void DialogueOverride_SetHintsMode(int mode);
// The randomizer hands over every progression placement after a fill
// (location name as the reference randomizer spells it, item name).
void DialogueOverride_ClearPlacements(void);
void DialogueOverride_NotePlacement(const char *location, const char *item);
// Ending credits: 0 STAFF (the original names), 1 CHAT (top chatters), text.ini credits=.
// GAME JOKES switch: the r-tagged lines (Rocket League, EverQuest, Ultima Online), text.ini gamejokes=.
int DialogueOverride_GameJokes(void);
void DialogueOverride_SetGameJokes(int on);
int DialogueOverride_CreditsMode(void);
void DialogueOverride_SetCreditsMode(int mode);
// Chatters in leaderboard order (count desc, staff last); 0 = past the end.
int DialogueOverride_RankedChatter(int rank, char *out, size_t cap);
// Messages counted for a chatter (chatters.txt, all streams); 0 = unknown.
int DialogueOverride_ChatterCount(const char *name);

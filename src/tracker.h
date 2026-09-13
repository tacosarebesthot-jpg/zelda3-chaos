#ifndef TRACKER_H
#define TRACKER_H

// Stream pickup tracker: writes the last 5 items received (newest first) to
// tracker_items.txt next to the game, for an OBS browser source
// (tools/tracker.html polls the same file).

#include "types.h"

void Tracker_LogItem(uint8 item_id);
// MAIN THREAD ONLY (unsynchronized static ring + .tmp/rename file write).
// All callers live on the main thread (twitch.c's Twitch_Tick dispatcher);
// the IRC thread must hand feed lines over via twitch.c's queue
// (TwitchQueueActivity), never call this directly.
void Tracker_LogActivity(const char *who, const char *verb);

// Stream scoreboard: lifetime counters persisted in scoreboard.txt
// (key=value lines) plus a preformatted single line in scoreboard_line.txt
// for OBS text sources. tools/feed.html renders the key=value file.
// Scoreboard_Init loads the saved tallies (call once at startup);
// Scoreboard_LogDeath / Scoreboard_LogCommand bump a counter and rewrite
// both files (.tmp+rename, like every other stream file here).
// CHAT BOSS v1: Scoreboard_LogBossWin / Scoreboard_LogLoss feed the
// additive bosses_won=/losses= keys (old files load them as 0).

void Scoreboard_Init(void);
void Scoreboard_LogDeath(void);
void Scoreboard_LogCommand(void);
void Scoreboard_LogBossWin(void);
void Scoreboard_LogBossLoss(void);

#endif

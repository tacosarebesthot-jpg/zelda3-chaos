// Stream pickup tracker (see tracker.h). Hooked from Link_ReceiveItem, which
// every meaningful acquisition path funnels through (the final item id after
// chest alternates is what arrives here). Filler pickups (hearts, rupees,
// ammo) bypass Link_ReceiveItem in the engine and are intentionally not
// tracked - lane wants the notable pickups.

#include "tracker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TRACKER_DEPTH 5

// Receipt ids -> names, derived from kMemoryLocationToGiveItemTo /
// kValueToGiveItemTo in misc.c (which inventory byte each id writes, and to
// what value). 0 = unknown/uncertain - falls back to "Item #N".
static const char *const kItemNames[76] = {
  "Fighter Sword", "Master Sword", "Tempered Sword", "Golden Sword",  // 0-3
  "Fighter Shield", "Red Shield", "Mirror Shield", "Fire Rod",        // 4-7
  "Ice Rod", "Hammer", "Hookshot", "Bow",                             // 8-11
  "Boomerang", "Mushroom", "Bottle", "Bombos Medallion",              // 12-15
  "Ether Medallion", "Quake Medallion", "Lamp", "Flute",              // 16-19
  "Flute", "Cane of Somaria", "Bottle", "Heart Piece",                // 20-23
  "Cane of Byrna", "Magic Cape", "Magic Mirror", "Power Glove",       // 24-27
  "Titan's Mitt", "Book of Mudora", "Zora's Flippers", "Moon Pearl",  // 28-31
  0, "Bug Net", "Progressive Mail", "Blue Mail",                      // 32-35
  "Small Key", "Compass", "Heart Container", 0,                       // 36-39
  0, "Magic Powder", "Magic Boomerang", "Bottle",                     // 40-43
  "Bottle", "Bottle", 0, 0,                                           // 44-47
  0, "Big Key", "Dungeon Map", "5 Rupees",                            // 48-51
  "20 Rupees", "1 Rupee", "Pendant", "Pendant",                       // 52-55
  "Pendant", "Bow & Arrows", "Silver Bow", "Bottle",                  // 56-59
  "Bottle", "Bottle", 0, 0,                                           // 60-63
  "100 Rupees", "50 Rupees", "Magic Container", 0,                    // 64-67
  "Arrow Upgrade", "Bomb Upgrade", "1 Rupee", "1 Rupee",              // 68-71
  "Bottle", "Sword", "Flute", "Pegasus Boots",                        // 72-75
};

static const char *ItemName(uint8 id) {
  if (id < 76 && kItemNames[id])
    return kItemNames[id];
  static char fallback[24];
  snprintf(fallback, sizeof(fallback), "Item #%d", id);
  return fallback;
}

void Tracker_LogItem(uint8 item_id) {
  static char lines[TRACKER_DEPTH][96];
  static int count;

  char when[16];  {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    snprintf(when, sizeof(when), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
  }

  // shift down, newest first
  if (count < TRACKER_DEPTH)
    count++;
  for (int i = TRACKER_DEPTH - 1; i > 0; i--)
    memcpy(lines[i], lines[i - 1], sizeof(lines[0]));
  snprintf(lines[0], sizeof(lines[0]), "%s | %s", ItemName(item_id), when);

  // write .tmp + rename so OBS never reads a half-written file
  FILE *f = fopen("tracker_items.txt.tmp", "w");
  if (!f)
    return;
  for (int i = 0; i < count; i++)
    fprintf(f, "%s\n", lines[i]);
  fclose(f);
  remove("tracker_items.txt");
  rename("tracker_items.txt.tmp", "tracker_items.txt");
}

// Chat activity for the stream overlay: last 8 applied commands, newest
// first, so viewers see their own name trigger an effect. Called from the
// twitch dispatcher on every accepted command.
//
// THREADING: MAIN THREAD ONLY. The ring below is plain static state (no
// lock), localtime() is not thread-safe on all CRTs, and the .tmp+rename
// sequence must never interleave with itself. Every caller lives on the main
// thread (Twitch_Tick and its dispatcher); the IRC thread hands its one feed
// line (the connect tagline) over via twitch.c's TwitchQueueActivity +
// Twitch_Tick flush instead of calling this directly - a direct call raced
// the main thread's ring and file writes on reconnect-during-chat.
void Tracker_LogActivity(const char *who, const char *verb) {
  static char lines[8][128];
  static int count;

  char when[16];
  {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    snprintf(when, sizeof(when), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
  }

  if (count < 8)
    count++;
  for (int i = 7; i > 0; i--)
    memcpy(lines[i], lines[i - 1], sizeof(lines[0]));
  // Format contract with tools/feed.html: parseActivityLine() matches
  // ^(.*?)\s*\|\s*!(.*?)\s*->\s*(.*)$ i.e. "who | !verb -> HH:MM:SS" with
  // the '!' anchored BEFORE the verb (audit P2-5a: the verb is the part
  // viewers scan for). feed.html's toast/history renderers prepend the '!'
  // to the captured verb explicitly, so the overlay display is unchanged.
  snprintf(lines[0], sizeof(lines[0]), "%s | !%s -> %s", who, verb, when);

  FILE *f = fopen("activity_feed.txt.tmp", "w");
  if (!f)
    return;
  for (int i = 0; i < count; i++)
    fprintf(f, "%s\n", lines[i]);
  fclose(f);
  remove("activity_feed.txt");
  rename("activity_feed.txt.tmp", "activity_feed.txt");
}

// -----------------------------------------------------------------------
// Stream scoreboard: two lifetime counters persisted across game restarts.
//   deaths   - Link died; hooked from Death_Func1 in messaging.c, the single
//              choke point every game-over sequence funnels through exactly
//              once (see the comment there).
//   commands - chat verbs applied by the twitch dispatcher (TwitchApply).
// scoreboard.txt is key=value only; tools/feed.html does the formatting.
// scoreboard_line.txt is a preformatted single line for OBS text sources.

static int sb_deaths;
static int sb_commands;
// CHAT BOSS v1 (twitch.c boss state machine): fights chat killed vs fights
// that survived the window. Additive keys - old scoreboard.txt files simply
// load 0/0 for these.
static int sb_boss_wins;
static int sb_boss_losses;

static void Scoreboard_Write(void) {
  FILE *f = fopen("scoreboard.txt.tmp", "w");
  if (!f)
    return;
  fprintf(f, "deaths=%d\n", sb_deaths);
  fprintf(f, "commands=%d\n", sb_commands);
  fprintf(f, "bosses_won=%d\n", sb_boss_wins);
  fprintf(f, "losses=%d\n", sb_boss_losses);
  fclose(f);
  remove("scoreboard.txt");
  rename("scoreboard.txt.tmp", "scoreboard.txt");

  f = fopen("scoreboard_line.txt.tmp", "w");
  if (!f)
    return;
  // \xc2\xb7 = UTF-8 middle dot
  fprintf(f, "deaths: %d \xc2\xb7 commands: %d \xc2\xb7 bosses: %d-%d\n",
          sb_deaths, sb_commands, sb_boss_wins, sb_boss_losses);
  fclose(f);
  remove("scoreboard_line.txt");
  rename("scoreboard_line.txt.tmp", "scoreboard_line.txt");
}

void Scoreboard_Init(void) {
  FILE *f = fopen("scoreboard.txt", "r");
  if (f) {
    char buf[64];
    while (fgets(buf, sizeof(buf), f)) {
      char *eq = strchr(buf, '=');
      if (!eq)
        continue;
      *eq = 0;
      int v = atoi(eq + 1);
      if (!strcmp(buf, "deaths"))
        sb_deaths = v;
      else if (!strcmp(buf, "commands"))
        sb_commands = v;
      else if (!strcmp(buf, "bosses_won"))
        sb_boss_wins = v;
      else if (!strcmp(buf, "losses"))
        sb_boss_losses = v;
    }
    fclose(f);
  }
  // rewrite immediately: regenerates scoreboard_line.txt after a restart and
  // repairs a missing/truncated scoreboard.txt from the loaded tallies
  Scoreboard_Write();
}

void Scoreboard_LogDeath(void) {
  sb_deaths++;
  Scoreboard_Write();
}

void Scoreboard_LogCommand(void) {
  sb_commands++;
  Scoreboard_Write();
}

// CHAT BOSS v1 payoffs (called from the twitch module's boss resolver)
void Scoreboard_LogBossWin(void) {
  sb_boss_wins++;
  Scoreboard_Write();
}

void Scoreboard_LogBossLoss(void) {
  sb_boss_losses++;
  Scoreboard_Write();
}

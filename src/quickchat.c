// Rocket League style QUICK CHAT box - see quickchat.h for the API.
//
// WHY: the owner, Lane and chat play Rocket League and spend the whole lobby
// spamming "What a save!" at each other about how washed they are. This puts
// the same box in Zelda: four lines in the top-left of the picture, newest at
// the bottom, each fading out after ~4 s, filled by what actually happens in
// the game (one-heart survivals, deaths, fairy revives, boss kills, the CHAT
// BOSS fight) plus an idle meme every couple of minutes.
//
// quickchat.txt (OPTIONAL, next to zelda3.exe) - the owner's own lines:
//   one meme per line, 36 characters max (longer lines are cut), blank lines
//   and lines starting with '#' ignored, up to 256 of them. They are appended
//   to the built-in meme table and picked with the same odds. Read once at
//   boot (QuickChat_Init), so edits need a restart.
//
// LAYOUT: everything below is in SNES pixels (the 256x224 picture) and gets
// multiplied by the render scale, so the box is the same size on screen at
// every window scale. Two constants, QC_BOX_X / QC_BOX_Y, place it - move
// them and nothing else moves.
//   x = 48 : clear of the magic meter and the item box on the HUD's left.
//   y = 36 : UNDER the status bar. The brief said y=8, but hud.c puts the
//            heart rows at HUDXY(20,1)..(20,2), i.e. pixels x>=160, y=8..23,
//            and the item box at HUDXY(5,1) = x=40, y=8 - a four line box at
//            y=8 would sit straight on top of both. 36 leaves the whole HUD
//            alone and still reads as the top-left of the picture.
// The box is skipped entirely while the settings-menu overlay is up.
//
// THREADING: main thread only (Twitch_Tick / Twitch_PostDraw), same as the
// tracker feed it writes to.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "quickchat.h"
#include "music_player.h"
#include "types.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "twitch.h"
#include "tracker.h"
#include "dialogue_override.h"
#include "settings_menu.h"

// ------------------------------------------------------------------ state --

#define QC_LINES     4        // visible lines
#define QC_NAME     20        // "Name" column, chars + nul
#define QC_TEXT     40        // message, chars + nul (36 max is enforced)
#define QC_LIFE    240        // 4 s at 60 fps before a line is gone
#define QC_FADE     48        // ...the last 0.8 s of that is the fade out
#define QC_GAP      20        // frames between two released lines (spam rate)
#define QC_PEND     16        // queued-but-not-shown lines
#define QC_MSG_MAX  36        // hard cap on one message, keeps the box narrow
#define QC_BOX_X    48        // box origin in SNES pixels (see LAYOUT above)
#define QC_BOX_Y    36

typedef struct QcLine {
  char name[QC_NAME];
  char text[QC_TEXT];
  int life;                   // counts down to 0, then the line is dropped
} QcLine;

typedef struct QcPending {
  char name[QC_NAME];         // empty = pick a chatter when it is released
  char text[QC_TEXT];
  bool feed;                  // first line of a burst: also goes to the feed
} QcPending;

static QcLine g_qc_line[QC_LINES];
static int g_qc_n;
static QcPending g_qc_pend[QC_PEND];
static int g_qc_pend_n;
static int g_qc_gap;                     // frames until the next release
static char g_qc_last_name[QC_NAME];     // avoid the same name twice in a row

// Own little xorshift: the quick chat must never disturb the game's RNG.
static uint32 g_qc_rng = 0x5eed1337u;
static uint32 QcRand(void) {
  g_qc_rng ^= g_qc_rng << 13;
  g_qc_rng ^= g_qc_rng >> 17;
  g_qc_rng ^= g_qc_rng << 5;
  return g_qc_rng;
}
static int QcRandRange(int n) { return n > 0 ? (int)(QcRand() % (uint32)n) : 0; }

// -------------------------------------------------------------- the memes --

// RL flavour, self-deprecating, the way this chat actually talks. Each line
// is kept under 36 characters so "Name: line" still fits the box.
static const char *const kQcMemes[] = {
  "you will never be Grand Champ",
  "SSL is a myth",
  "Champ 1 with a Bronze brain",
  "ball cam off since 2016",
  "whiffed an open net, ff at 0-2",
  "boost starved and proud",
  "musty flick into our own net",
  "we are washed",
  "demo'd by a Diamond 2",
  "mechanics? never heard of her",
  "nice flip reset... into the wall",
  "chat is Silver 3 in life",
  "he plays like the ball owes him",
  "no boost no brain",
  "pros make it look easy",
  "we make it look impossible",
  "GG go next",
  "this is a Plat lobby and it shows",
  "wavedash into the pit",
  "faking... nobody",
  "In position. (behind the ball)",
  "kickoff went like the last stream",
  "50/50 lost, like every 50/50",
  "own goal speedrun any%",
  "the only rank we hit is washed",
  "ceiling shot? ceiling fall",
  "trade a demo for a heart? deal",
  "chat rotates like Link: badly",
  "ranked reset hit like Ganon",
  "double commit, classic",
  "ball chasing is a playstyle",
  "hardstuck since the beta",
  "aerial? in this economy?",
  "that was a pass. to them.",
  "our defence is a rumour",
  "kickoff cheat, kickoff loss",
  "smurf detected, ff now",
  "one more game he says",
  "queue at 2am, rank at 2am",
  "we peaked in Gold 2",
  "the wall is our best defender",
  "boost pads are for cowards",
  "flip cancel? flip canceled",
  "he dribbles like he skips leg day",
  "goalie in name only",
  "shot on target: zero",
  "we lost to a Bakkesmod tutorial",
  "the car drives itself, badly",
  "reset MMR, reset dignity",
  "air roll left is a personality",
  "backboard read? backboard bonk",
  "second man? second guessing",
  "ranked 1v1 is a hate crime",
  "we do not miss, we redirect",
  "team of three ball chasers",
  "epic servers ate the goal",
  "packet loss took my rank",
  "the ball was in my blind spot",
  "our kickoff plan is vibes",
  "flip reset holder of nothing",
  "our team comp is three strikers",
  "he ghosted the ball entirely",
  "even the bots are diffing us",
  "a Bronze would have saved that",
  "playing like the lag is a teammate",
  "we are allergic to open nets",
  "hit the post, hit the copium",
  "the goalpost is our arch nemesis",
  "master sword? master whiff",
  "Ganon has better rotation",
  "Hyrule 1v1 me, no boost",
};
#define QC_MEME_COUNT ((int)(sizeof(kQcMemes) / sizeof(kQcMemes[0])))

// quickchat.txt extras, appended to the pool above.
#define QC_EXTRA_MAX 256
static char g_qc_extra[QC_EXTRA_MAX][QC_TEXT];
static int g_qc_extra_n;

static const char *QcMeme(void) {
  int n = QC_MEME_COUNT + g_qc_extra_n;
  int i = QcRandRange(n);
  return (i < QC_MEME_COUNT) ? kQcMemes[i] : g_qc_extra[i - QC_MEME_COUNT];
}

// Event bursts. Real RL quick chats first, chat's own colour after.
static const char *const kQcSaveLines[] = {
  "What a save!", "What a save!", "What a save!", "Wow!",
  "Close one!", "Nice block!", "Calculated.", "Holy cow!",
  "one pixel of health, calculated",
};
static const char *const kQcDeathLines[] = {
  "Noooo!", "Whoops...", "What a save!", "gg", "gg ez",
  "Rematch!", "One. More. Game.", "ff at 0-2",
};
static const char *const kQcReviveLines[] = {
  "Calculated.", "In position.", "What a save!", "What a save!",
  "Nice one!", "the fairy has better rotation",
};
static const char *const kQcBossLines[] = {
  "Nice shot!", "Siiiick!", "What a play!", "Savage!",
  "Holy cow!", "OMG!", "that was actually clean",
};
static const char *const kQcWinLines[] = {
  "Great pass!", "Nice shot!", "What a play!", "Wow!", "Savage!",
};
static const char *const kQcLossLines[] = {
  "gg ez", "Well played.", "Whoops...", "Faking.", "Take the shot!",
  "chat is hardstuck",
};
static const char *const kQcItemLines[] = {
  "Nice shot!", "Great pass!", "Wow!", "Okay.", "Nice one!",
  "new mechanic unlocked, still Gold",
};

// -------------------------------------------------------------- the queue --

// Pick the name for a line at RELEASE time, not at push time: a burst is
// queued in one frame but drips out 20 frames apart, so resolving late gives
// each line a different chatter (Twitch_RandomChatter rotates on the clock).
static void QcPickName(char *out, size_t cap) {
  static int rank;
  char buf[QC_NAME], live[QC_NAME];
  bool have_live = Twitch_RandomChatter(live, sizeof(live)) && live[0];
  if (have_live && strcmp(live, g_qc_last_name)) {
    snprintf(out, cap, "%s", live);
    return;
  }
  // Nobody has chatted this session (or that name is already on the last
  // line): walk the leaderboard pool from chatters.txt so the box still looks
  // like several people talking. rank is 0-based; 0 = past the end -> wrap.
  for (int tries = 0; tries < 2; tries++) {
    for (int i = 0; i < 8; i++) {        // at most 8 hops to dodge a repeat
      buf[0] = 0;
      if (!DialogueOverride_RankedChatter(rank++, buf, sizeof(buf))) {
        rank = 0;
        break;
      }
      if (buf[0] && strcmp(buf, g_qc_last_name)) {
        snprintf(out, cap, "%s", buf);
        return;
      }
    }
  }
  // One chatter and nothing else known: repeating the name is fine.
  snprintf(out, cap, "%s", have_live ? live : "Chat");
}

static void QcQueue(const char *name, const char *text, bool feed) {
  if (!DialogueOverride_GameJokes()) return;   // OPTIONS > GAME FEATURES > GAME JOKES = OFF silences the box
  if (!text || !text[0] || g_qc_pend_n >= QC_PEND)
    return;
  QcPending *p = &g_qc_pend[g_qc_pend_n++];
  snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
  snprintf(p->text, sizeof(p->text), "%.*s", QC_MSG_MAX, text);
  p->feed = feed;
}

// Move one queued line into the visible box (oldest scrolls off the top).
static void QcRelease(void) {
  QcPending p = g_qc_pend[0];
  g_qc_pend_n--;
  memmove(g_qc_pend, g_qc_pend + 1, sizeof(g_qc_pend[0]) * (size_t)g_qc_pend_n);
  char name[QC_NAME];
  if (p.name[0])
    snprintf(name, sizeof(name), "%s", p.name);
  else
    QcPickName(name, sizeof(name));
  snprintf(g_qc_last_name, sizeof(g_qc_last_name), "%s", name);
  if (g_qc_n >= QC_LINES) {
    memmove(g_qc_line, g_qc_line + 1, sizeof(g_qc_line[0]) * (QC_LINES - 1));
    g_qc_n = QC_LINES - 1;
  }
  QcLine *l = &g_qc_line[g_qc_n++];
  snprintf(l->name, sizeof(l->name), "%s", name);
  snprintf(l->text, sizeof(l->text), "%s", p.text);
  l->life = QC_LIFE;
  if (p.feed)
    Tracker_LogActivity(name, p.text);   // OBS feed overlay gets the opener
  g_qc_gap = QC_GAP;
}

// Queue `count` lines from `tab`, first one flagged for the feed. Repeats are
// allowed (RL chat repeats itself); the start offset just varies the order.
static void QcBurst(const char *const *tab, int tab_n, int count) {
  if (tab_n <= 0)
    return;
  int at = QcRandRange(tab_n);
  for (int i = 0; i < count; i++)
    QcQueue(NULL, tab[(at + i) % tab_n], i == 0);
}

#define QC_BURST(tab, count) QcBurst((tab), (int)(sizeof(tab) / sizeof((tab)[0])), (count))

// ----------------------------------------------------------- verb helpers --

// !qc text: letters, digits, space and light punctuation only, 36 chars, and
// no links (chat WILL try). Returns false when nothing usable is left.
static bool QcSanitize(const char *in, char *out, size_t cap) {
  if (!in || !out || cap < 2)
    return false;
  char low[128];
  size_t li = 0;
  for (const char *p = in; *p && li < sizeof(low) - 1; p++)
    low[li++] = (char)tolower((unsigned char)*p);
  low[li] = 0;
  if (strstr(low, "http") || strstr(low, "www.") || strstr(low, "://"))
    return false;
  size_t n = 0;
  size_t lim = cap - 1 < (size_t)QC_MSG_MAX ? cap - 1 : (size_t)QC_MSG_MAX;
  for (const char *p = in; *p && n < lim; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == ' ' || strchr(".,!?'-:()", c)) {
      if (c == ' ' && (n == 0 || out[n - 1] == ' '))
        continue;                        // no leading / doubled spaces
      out[n++] = (char)c;
    }
  }
  while (n > 0 && out[n - 1] == ' ')
    n--;
  out[n] = 0;
  return n > 0;
}

void QuickChat_ChatLine(const char *who, const char *text) {
  char clean[QC_TEXT];
  if (!QcSanitize(text, clean, sizeof(clean)))
    return;
  QcQueue((who && who[0]) ? who : "Chat", clean, true);
}

void QuickChat_BurstWhatASave(void) { QC_BURST(kQcSaveLines, 3 + QcRandRange(3)); }
void QuickChat_BurstGG(void) { QC_BURST(kQcDeathLines, 3 + QcRandRange(3)); }
void QuickChat_BossWin(void) { QC_BURST(kQcWinLines, 3); }
void QuickChat_BossLoss(void) { QC_BURST(kQcLossLines, 3 + QcRandRange(2)); }

// ------------------------------------------------------------- the events --

enum {
  kQcEvSave, kQcEvDeath, kQcEvRevive, kQcEvBoss, kQcEvItem, kQcEvCount
};
#define QC_EVENT_CD (8 * 60)             // never more than one of a kind / 8 s

static int g_qc_frame;
static int g_qc_ev_at[kQcEvCount];       // frame the event last fired
static bool QcEventReady(int ev) {
  if (g_qc_ev_at[ev] && g_qc_frame - g_qc_ev_at[ev] < QC_EVENT_CD)
    return false;
  g_qc_ev_at[ev] = g_qc_frame;
  return true;
}

// Watched game state, resynced (without firing) whenever play resumes.
static bool g_qc_was_play;
static uint8 g_qc_hp;
static uint8 g_qc_pendants, g_qc_crystals;
static uint8 g_qc_sword, g_qc_bow, g_qc_hook, g_qc_mirror, g_qc_pearl, g_qc_fins;
static int g_qc_idle, g_qc_idle_next;

static int QcBitCount(uint8 v) {
  int n = 0;
  for (; v; v &= (uint8)(v - 1))
    n++;
  return n;
}

static void QcSyncWatched(void) {
  g_qc_hp = link_health_current;
  g_qc_pendants = link_which_pendants;
  g_qc_crystals = link_has_crystals;
  g_qc_sword = link_sword_type;
  g_qc_bow = link_item_bow;
  g_qc_hook = link_item_hookshot;
  g_qc_mirror = link_item_mirror;
  g_qc_pearl = link_item_moon_pearl;
  g_qc_fins = link_item_flippers;
}

static void QcWatchGame(void) {
  // 7 = dungeon, 9 = overworld (kMainRouting). Deliberately NOT the stricter
  // gameplay gate twitch.c uses: the death frame and the fairy revive both
  // happen with a submodule / cutscene lock set, and those are exactly the
  // two moments chat wants to talk over.
  bool play = (main_module_index == 7 || main_module_index == 9);
  if (!play) {
    g_qc_was_play = false;
    return;
  }
  if (!g_qc_was_play) {                  // first frame back in play: resync
    g_qc_was_play = true;
    QcSyncWatched();
    return;
  }

  uint8 hp = link_health_current;

  // Death: health hit 0 while playing.
  if (hp == 0 && g_qc_hp > 0 && QcEventReady(kQcEvDeath))
    QC_BURST(kQcDeathLines, 3 + QcRandRange(3));
  // Fairy revive: back up from 0 without ever leaving gameplay (a reload from
  // the game-over screen goes through other modules and resyncs above).
  else if (hp > 0 && g_qc_hp == 0 && QcEventReady(kQcEvRevive))
    QC_BURST(kQcReviveLines, 3 + QcRandRange(2));
  // Survived on a sliver: took a hit and came out at one heart or less.
  else if (hp > 0 && hp < g_qc_hp && hp <= 8 && QcEventReady(kQcEvSave))
    QC_BURST(kQcSaveLines, 3 + QcRandRange(3));
  g_qc_hp = hp;

  // Dungeon boss down. The dung_savegame_state_bits 0x8000 "boss beaten" bit
  // is NOT usable as the trigger: it is restored from save_dung_info every
  // time the room loads (dungeon.c Dungeon_LoadRoom), and the real set in
  // PrepareDungeonExitFromBossFight lands on the same frame the room index
  // changes, so a 0->1 edge cannot be told apart from walking back into a
  // cleared boss room. The prize is unambiguous and only ever grows.
  uint8 pend = link_which_pendants, crys = link_has_crystals;
  if ((QcBitCount(pend) > QcBitCount(g_qc_pendants) ||
       QcBitCount(crys) > QcBitCount(g_qc_crystals)) && QcEventReady(kQcEvBoss))
    QC_BURST(kQcBossLines, 3 + QcRandRange(3));
  g_qc_pendants = pend;
  g_qc_crystals = crys;

  // Big item, polled instead of hooked (player.c is off limits for this
  // change): the headline equipment slots only ever go up.
  bool got = (link_sword_type > g_qc_sword) || (link_item_bow > g_qc_bow) ||
             (link_item_hookshot > g_qc_hook) ||
             (link_item_mirror > g_qc_mirror) ||
             (link_item_moon_pearl > g_qc_pearl) ||
             (link_item_flippers > g_qc_fins);
  if (got && QcEventReady(kQcEvItem))
    QC_BURST(kQcItemLines, 2 + QcRandRange(2));
  g_qc_sword = link_sword_type;
  g_qc_bow = link_item_bow;
  g_qc_hook = link_item_hookshot;
  g_qc_mirror = link_item_mirror;
  g_qc_pearl = link_item_moon_pearl;
  g_qc_fins = link_item_flippers;

  // Idle: one meme every 2-3 minutes of actual play, so a quiet stretch still
  // gets the box on screen.
  if (++g_qc_idle >= g_qc_idle_next) {
    g_qc_idle = 0;
    g_qc_idle_next = (120 + QcRandRange(61)) * 60;
    QcQueue(NULL, QcMeme(), true);
  }
}

// ---------------------------------------------------------------- ticking --

void QuickChat_Tick(void) {
  g_qc_frame++;

  // NOW PLAYING toast: the music folder decode thread parks the new filename
  // and we drain it here, on the main thread, into the normal chat box (which
  // already ages its lines out after a few seconds).
  {
    char np[64];
    if (MusicPlayer_TakeNowPlaying(np, sizeof np)) {
      char line[96];
      snprintf(line, sizeof line, "NOW PLAYING %s", np);
      QcQueue("Music", line, true);
    }
  }

  g_qc_rng += (uint32)link_x_coord * 2654435761u + (uint32)g_qc_frame;
  QcRand();                              // stir: the box should not loop

  for (int i = 0; i < g_qc_n; ) {        // age, drop the ones that ran out
    if (--g_qc_line[i].life > 0) {
      i++;
      continue;
    }
    memmove(g_qc_line + i, g_qc_line + i + 1,
            sizeof(g_qc_line[0]) * (size_t)(g_qc_n - i - 1));
    g_qc_n--;
  }
  if (g_qc_gap > 0)
    g_qc_gap--;
  if (g_qc_pend_n > 0 && g_qc_gap == 0)
    QcRelease();

  QcWatchGame();
}

// ---------------------------------------------------------------- drawing --

// 5x7 uppercase pixel font, the same glyph forms as the settings menu
// overlay (src/settings_menu.c) - copied rather than shared because that
// module keeps its renderer static. Rows are MSB-first: bit 4 is the LEFT
// column. Lowercase folds to uppercase, so the box shouts like a real lobby.
static const uint8 *QcGlyph(char c) {
  static const uint8 kSpace[7] = { 0 };
  static const uint8 kUpper[26][7] = {
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },  // A
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },  // B
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },  // C
    { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },  // D
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },  // E
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },  // F
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },  // G
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },  // H
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },  // I
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },  // J
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },  // K
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },  // L
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },  // M
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },  // N
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },  // O
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },  // P
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },  // Q
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },  // R
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },  // S
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },  // T
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },  // U
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },  // V
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A },  // W
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },  // X
    { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 },  // Y
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },  // Z
  };
  static const uint8 kDigit[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },  // 0
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },  // 1
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },  // 2
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },  // 3
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },  // 4
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },  // 5
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },  // 6
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },  // 7
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },  // 8
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },  // 9
  };
  if (c >= 'A' && c <= 'Z')
    return kUpper[c - 'A'];
  if (c >= 'a' && c <= 'z')
    return kUpper[c - 'a'];
  if (c >= '0' && c <= '9')
    return kDigit[c - '0'];
  switch (c) {
  case '!': { static const uint8 g[7] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04 }; return g; }
  case '?': { static const uint8 g[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0, 0x04 }; return g; }
  case '.': { static const uint8 g[7] = { 0, 0, 0, 0, 0, 0x0C, 0x0C }; return g; }
  case ',': { static const uint8 g[7] = { 0, 0, 0, 0, 0x0C, 0x0C, 0x08 }; return g; }
  case ':': { static const uint8 g[7] = { 0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0 }; return g; }
  case '\'': { static const uint8 g[7] = { 0x04, 0x04, 0x08, 0, 0, 0, 0 }; return g; }
  case '-': { static const uint8 g[7] = { 0, 0, 0, 0x1F, 0, 0, 0 }; return g; }
  case '/': { static const uint8 g[7] = { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 }; return g; }
  case '(': { static const uint8 g[7] = { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 }; return g; }
  case ')': { static const uint8 g[7] = { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 }; return g; }
  case '%': { static const uint8 g[7] = { 0x11, 0x01, 0x02, 0x04, 0x08, 0x10, 0x11 }; return g; }
  case '+': { static const uint8 g[7] = { 0, 0x04, 0x04, 0x1F, 0x04, 0x04, 0 }; return g; }
  default:
    return kSpace;
  }
}

// src over dst, a = 0..255. Alpha byte forced opaque like every other
// overlay on this buffer.
static void QcBlend(uint32 *p, uint32 rgb, int a) {
  uint32 d = *p;
  uint32 r = ((((rgb >> 16) & 0xff) * a) + (((d >> 16) & 0xff) * (255 - a))) / 255;
  uint32 g = ((((rgb >> 8) & 0xff) * a) + (((d >> 8) & 0xff) * (255 - a))) / 255;
  uint32 b = (((rgb & 0xff) * a) + ((d & 0xff) * (255 - a))) / 255;
  *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void QcDrawGlyph(uint8 *pixels, int pitch, int width, int height,
                        int x, int y, char c, uint32 rgb, int a, int s) {
  const uint8 *g = QcGlyph(c);
  for (int gy = 0; gy < 7; gy++) {
    if (!g[gy])
      continue;
    for (int yy = 0; yy < s; yy++) {
      int py = y + gy * s + yy;
      if (py < 0 || py >= height)
        continue;
      uint32 *row = (uint32 *)(pixels + (size_t)py * pitch);
      for (int gx = 0; gx < 5; gx++) {
        if (!(g[gy] & (1 << (4 - gx))))  // MSB-first: bit 4 is the left column
          continue;
        for (int xx = 0; xx < s; xx++) {
          int px = x + gx * s + xx;
          if (px >= 0 && px < width)
            QcBlend(&row[px], rgb, a);
        }
      }
    }
  }
}

static void QcDrawText(uint8 *pixels, int pitch, int width, int height,
                       int x, int y, const char *t, uint32 rgb, int a, int s) {
  for (; *t; t++, x += 6 * s)
    QcDrawGlyph(pixels, pitch, width, height, x, y, *t, rgb, a, s);
}

void QuickChat_PostDraw(uint8 *pixels, int pitch, int width, int height) {
  if (!pixels || g_qc_n == 0 || width < 128 || height < 96)
    return;
  if (SettingsMenu_IsOpen())
    return;                              // the menu owns the screen

  // One "unit" = one SNES pixel, so the box keeps its size at every scale.
  const int s = (width / 256) < 1 ? 1 : (width / 256);
  const int cw = 6 * s, lh = 9 * s;
  const int x0 = QC_BOX_X * s, y0 = QC_BOX_Y * s;
  int cols = (width - x0 - 4 * s) / cw;
  if (cols < 8)
    return;                              // no room: better nothing than mush
  if (cols > QC_NAME + QC_TEXT + 2)
    cols = QC_NAME + QC_TEXT + 2;        // never index past the line buffer

  for (int i = 0; i < g_qc_n; i++) {
    const QcLine *l = &g_qc_line[i];
    char buf[QC_NAME + QC_TEXT + 4];
    snprintf(buf, sizeof(buf), "%s: %s", l->name, l->text);
    if ((int)strlen(buf) > cols)
      buf[cols] = 0;
    int len = (int)strlen(buf);
    if (len == 0)
      continue;
    int a = (l->life >= QC_FADE) ? 255 : (255 * l->life / QC_FADE);
    int y = y0 + i * lh;

    // Dark backdrop strip, like the RL chat plate: keeps white text readable
    // over bright grass and sand.
    int bx = x0 - 2 * s, bw = len * cw + 3 * s;
    for (int py = y - s; py < y + 8 * s; py++) {
      if (py < 0 || py >= height)
        continue;
      uint32 *row = (uint32 *)(pixels + (size_t)py * pitch);
      for (int px = bx; px < bx + bw; px++)
        if (px >= 0 && px < width)
          QcBlend(&row[px], 0x000000u, a * 160 / 255);
    }
    QcDrawText(pixels, pitch, width, height, x0 + s, y + s, buf,
               0x000000u, a * 200 / 255, s);   // drop shadow
    QcDrawText(pixels, pitch, width, height, x0, y, buf, 0xFFFFFFu, a, s);
  }
}

// ------------------------------------------------------------------- boot --

void QuickChat_Init(void) {
  g_qc_idle_next = (120 + QcRandRange(61)) * 60;   // first meme in 2-3 min
  FILE *f = fopen("quickchat.txt", "rb");
  if (!f)
    return;                              // optional file, silence is correct
  char line[256];
  while (g_qc_extra_n < QC_EXTRA_MAX && fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                     line[n - 1] == ' ' || line[n - 1] == '\t'))
      line[--n] = 0;
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p || *p == '#')
      continue;
    snprintf(g_qc_extra[g_qc_extra_n++], QC_TEXT, "%.*s", QC_MSG_MAX, p);
  }
  fclose(f);
  printf("[quickchat] %d extra meme lines from quickchat.txt\n", g_qc_extra_n);
}

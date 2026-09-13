// windows.h must come before the game headers (variables.h macros clash with winnt.h)
#ifdef _WIN32
#include <windows.h>
#endif
// Runtime dialogue override: see dialogue_override.h.
//
// Output is the same command stream the packed asset uses, chosen from
// g_zenv.dialogue_flags (bit 0 = "new" EU-style command bytes), so the
// existing Text_LoadCharacterBuffer loop handles [Name], [Number], [Window],
// [Position] and [Color] exactly as it does for built-in text.
#include "dialogue_override.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "util.h"
#include "twitch.h"
#include "dungeon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#define DO_MAX_MSGS 512
#define DO_LINE_MAX_PX 168   // 21 tiles of 8 px: the text area of the box
#define DO_POOL_MAX 256

#define DO_MAX_ALT 8
static char *g_do_msg[DO_MAX_MSGS][DO_MAX_ALT];   // alternatives; one is picked per showing
static uint8 g_do_nalt[DO_MAX_MSGS];
static uint8 g_do_dirty[DO_MAX_MSGS][DO_MAX_ALT];   // cursing: tagged Nx: or auto-detected
static int g_do_count;
static char g_do_pool[DO_POOL_MAX][32];
static int g_do_pool_n;
// Chatter counter: one count per pool name (chat messages seen by the game,
// this stream and every stream before it, from chatters.txt). The {chatter}
// token favours the names high on the list, so regulars show up more.
// Names on the "staff:" line of chatters.txt (the streamer, the owner) are
// counted and can be in the jokes like anyone, but they are never ranked:
// they sit at the bottom of the file and carry no extra weight (owner 09-12:
// "we can be jokes, I just don't want us always taking the top chatter spot").
static int g_do_pool_count[DO_POOL_MAX];
#define DO_IGNORE_MAX 16
static char g_do_ignore[DO_IGNORE_MAX][32];   // the staff list
static int g_do_ignore_n;
static char g_do_chatters_header[1024];   // comment lines kept when the file is rewritten
static uint32 g_do_rng = 0x2545f491;
static int g_do_mode = 1;          // text.ini jokes= 0 NORMAL, 1 CLEAN, 2 DIRTY (MODS page TEXT row)

// ---- HINTS (owner 09-12, Discord Q2: "a conf option in the menu, replace
// hints with a flavoured version or normal") -------------------------------
// text.ini hints= 0 NORMAL (vanilla tile/fortune text), 1 REAL (true
// placements from this seed's fill), 2 JOKES (dialogue.txt lines tagged h).
// Hint messages are never replaced by ordinary joke lines, and h lines are
// never used for anything else.
static int g_do_hints = 0;
static int g_do_credits = 1;      // text.ini credits= 0 STAFF, 1 CHAT (ending credits name rows)
static int g_do_gamejokes = 1;    // text.ini gamejokes= 0 OFF, 1 ON: the r-tagged lines (Rocket League, EverQuest, Ultima Online)
static uint8 g_do_gameline[DO_MAX_MSGS][DO_MAX_ALT];
static uint8 g_do_hintline[DO_MAX_MSGS][DO_MAX_ALT];
#define DO_PLACE_MAX 96
static struct { char loc[56]; char item[32]; } g_do_place[DO_PLACE_MAX];
static int g_do_place_n;

int DialogueOverride_HintsMode(void) { return g_do_hints; }
const char *DialogueOverride_HintsLabel(void) {
  return g_do_hints == 0 ? "NORMAL" : g_do_hints == 1 ? "REAL" : "JOKES";
}
void DialogueOverride_ClearPlacements(void) { g_do_place_n = 0; }
void DialogueOverride_NotePlacement(const char *location, const char *item) {
  if (!location || !item || g_do_place_n >= DO_PLACE_MAX) return;
  snprintf(g_do_place[g_do_place_n].loc, sizeof g_do_place[0].loc, "%s", location);
  snprintf(g_do_place[g_do_place_n].item, sizeof g_do_place[0].item, "%s", item);
  g_do_place_n++;
}

bool DialogueOverride_Enabled(void) { return g_do_mode != 0; }
int DialogueOverride_Mode(void) { return g_do_mode; }
const char *DialogueOverride_ModeLabel(void) {
  return g_do_mode == 0 ? "NORMAL" : g_do_mode == 1 ? "CLEAN" : "DIRTY";
}
// words that make a line DIRTY when it is not tagged; whole words, any case
static bool DoIsDirty(const char *t) {
  static const char *const kWords[] = {
    "fuck", "fucking", "fucked", "fucker", "fuckin", "shit", "shitty", "bullshit", "ass",
    "asses", "badass", "bitch", "bitches", "damn", "dammit", "goddamn", "cunt", "dick",
    "cock", "pussy", "whore", "whores", "tits", "nowyafuckedup", "fucterbud" };
  char w[32]; int n = 0;
  for (const char *p = t;; p++) {
    char c = *p;
    bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (letter) { if (n < 31) w[n++] = (char)(c | 0x20); }
    else {
      if (n) {
        w[n] = 0;
        for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++)
          if (!strcmp(w, kWords[i])) return true;
        n = 0;
      }
      if (!c) break;
    }
  }
  return false;
}
bool DialogueOverride_Available(void) { return g_do_count > 0; }

static void DoWriteTextIni(void) {
  FILE *f = fopen("text.ini", "wb");
  if (f) {
    fprintf(f, "# zelda3 stream build - in-game text (MODS page TEXT row, OPTIONS > GAME FEATURES > HINTS)%c", 10);
    fprintf(f, "# jokes=0 original text, 1 jokes without cursing (CLEAN), 2 everything (DIRTY)%c", 10);
    fprintf(f, "jokes=%d%c", g_do_mode, 10);
    fprintf(f, "# hints=0 original tiles and fortunes, 1 REAL item locations of this seed, 2 joke hints (h lines)%c", 10);
    fprintf(f, "hints=%d%c", g_do_hints, 10);
    fprintf(f, "# credits=0 the original staff names in the ending, 1 the top chatters%c", 10);
    fprintf(f, "credits=%d%c", g_do_credits, 10);
    fprintf(f, "# gamejokes=0 hides the r lines (Rocket League, EverQuest, Ultima Online), 1 shows them%c", 10);
    fprintf(f, "gamejokes=%d%c", g_do_gamejokes, 10);
    fclose(f);
  }
}
void DialogueOverride_Toggle(void) {
  g_do_mode = (g_do_mode + 1) % 3;
  DoWriteTextIni();
}
void DialogueOverride_SetHintsMode(int mode) {
  g_do_hints = ((mode % 3) + 3) % 3;
  DoWriteTextIni();
}
int DialogueOverride_GameJokes(void) { return g_do_gamejokes; }
void DialogueOverride_SetGameJokes(int on) { g_do_gamejokes = on ? 1 : 0; DoWriteTextIni(); }
int DialogueOverride_CreditsMode(void) { return g_do_credits; }
void DialogueOverride_SetCreditsMode(int mode) {
  g_do_credits = mode ? 1 : 0;
  DoWriteTextIni();
}

enum {
  kDoCmd_NextPic, kDoCmd_Choose, kDoCmd_Item, kDoCmd_Name, kDoCmd_Window,
  kDoCmd_Number, kDoCmd_Position, kDoCmd_ScrollSpd, kDoCmd_Selchg, kDoCmd_9,
  kDoCmd_Choose3, kDoCmd_Choose2, kDoCmd_Scroll, kDoCmd_1, kDoCmd_2, kDoCmd_3,
  kDoCmd_Color, kDoCmd_Wait, kDoCmd_Sound, kDoCmd_Speed, kDoCmd_Mark,
  kDoCmd_Mark2, kDoCmd_Clear, kDoCmd_Waitkey, kDoCmd_End,
};

typedef struct DoCmdInfo {
  const char *name;
  uint8 us_cmd;      // command index (US byte = 0x67 + us_cmd)
  uint8 us_param;    // US takes a parameter byte
  uint8 eu_byte;     // EU-style first byte (0 = unsupported)
  uint8 eu_param;    // EU-style second byte base (used when eu_byte == 0x87)
  uint8 eu_has2;
} DoCmdInfo;

static const DoCmdInfo kDoCmds[] = {
  {"Scroll",   kDoCmd_Scroll,   0, 0x80, 0,    0},
  {"Waitkey",  kDoCmd_Waitkey,  0, 0x81, 0,    0},
  {"1",        kDoCmd_1,        0, 0x82, 0,    0},
  {"2",        kDoCmd_2,        0, 0x83, 0,    0},
  {"3",        kDoCmd_3,        0, 0x84, 0,    0},
  {"Name",     kDoCmd_Name,     0, 0x85, 0,    0},
  {"Wait",     kDoCmd_Wait,     1, 0x87, 0x00, 1},
  {"Color",    kDoCmd_Color,    1, 0x87, 0x10, 1},
  {"Number",   kDoCmd_Number,   1, 0x87, 0x20, 1},
  {"Speed",    kDoCmd_Speed,    1, 0x87, 0x30, 1},
  {"Sound",    kDoCmd_Sound,    1, 0x87, 0x40, 1},   // only sound 45 exists
  {"Choose",   kDoCmd_Choose,   0, 0x87, 0x80, 1},
  {"Choose2",  kDoCmd_Choose2,  0, 0x87, 0x81, 1},
  {"Choose3",  kDoCmd_Choose3,  0, 0x87, 0x82, 1},
  {"Selchg",   kDoCmd_Selchg,   0, 0x87, 0x83, 1},
  {"Item",     kDoCmd_Item,     0, 0x87, 0x84, 1},
  {"NextPic",  kDoCmd_NextPic,  0, 0x87, 0x85, 1},
  {"Window",   kDoCmd_Window,   1, 0x87, 0x86, 1},   // always window type 2
  {"Position", kDoCmd_Position, 1, 0x87, 0x87, 1},   // 0 or 1
};

// Named glyphs of the US alphabet (index into kTextAlphabet_US).
static const struct { const char *name; uint8 idx; } kDoGlyphs[] = {
  {"...", 67}, {"Ankh", 71}, {"Waves", 72}, {"Snake", 73}, {"LinkL", 74},
  {"LinkR", 75}, {"Up", 77}, {"Down", 78}, {"Left", 79}, {"Right", 80},
  {"1HeartL", 82}, {"1HeartR", 83}, {"2HeartL", 84}, {"3HeartL", 85},
  {"3HeartR", 86}, {"4HeartL", 87}, {"4HeartR", 88}, {"A", 91}, {"B", 92},
  {"X", 93}, {"Y", 94},
};

static int DoLetterIndex(int ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return 26 + ch - 'a';
  if (ch >= '0' && ch <= '9') return 52 + ch - '0';
  switch (ch) {
  case '!': return 62;  case '?': return 63;  case '-': return 64;
  case '.': return 65;  case ',': return 66;  case '>': return 68;
  case '(': return 69;  case ')': return 70;  case '"': return 76;
  case '\'': return 81; case ' ': return 89;  case '<': return 90;
  // not in the font: closest readable stand-ins
  case ':': return 65;  case ';': return 66;  case '_': return 64;
  case '/': return 64;  case '`': return 81;
  }
  return -1;
}

static bool DoIsIgnored(const char *name) {
  for (int i = 0; i < g_do_ignore_n; i++)
    if (!strcasecmp(g_do_ignore[i], name)) return true;
  return false;
}

static int DoPoolFind(const char *name) {
  for (int i = 0; i < g_do_pool_n; i++)
    if (!strcasecmp(g_do_pool[i], name)) return i;
  return -1;
}

// returns the pool index, or -1 when the name is unusable / the pool is full
static int DoPoolAdd(const char *name, int count) {
  size_t n = strlen(name);
  if (!n || n >= 32) return -1;
  int i = DoPoolFind(name);
  if (i >= 0) {
    if (count > g_do_pool_count[i]) g_do_pool_count[i] = count;
    return i;
  }
  if (g_do_pool_n >= DO_POOL_MAX) return -1;
  memcpy(g_do_pool[g_do_pool_n], name, n + 1);
  g_do_pool_count[g_do_pool_n] = count;
  return g_do_pool_n++;
}

// chatters.txt, highest count first: the file itself is the leaderboard.
// Staff names go last whatever their count.
static int DoRankKey(int i) { return DoIsIgnored(g_do_pool[i]) ? -1 : g_do_pool_count[i]; }
static void DoChattersWrite(void) {
  int order[DO_POOL_MAX];
  for (int i = 0; i < g_do_pool_n; i++) order[i] = i;
  for (int i = 1; i < g_do_pool_n; i++) {          // insertion sort, stable
    int k = order[i], j = i;
    while (j > 0 && DoRankKey(order[j - 1]) < DoRankKey(k)) { order[j] = order[j - 1]; j--; }
    order[j] = k;
  }
  FILE *f = fopen("chatters.txt.tmp", "wb");
  if (!f) return;
  fputs(g_do_chatters_header, f);
  if (g_do_ignore_n) {
    fputs("staff:", f);
    for (int i = 0; i < g_do_ignore_n; i++) fprintf(f, "%s %s", i ? "," : "", g_do_ignore[i]);
    fputc(10, f);
  }
  for (int i = 0; i < g_do_pool_n; i++)
    fprintf(f, "%s %d%c", g_do_pool[order[i]], g_do_pool_count[order[i]], 10);
  bool ok = fclose(f) == 0;
#ifdef _WIN32
  if (ok) ok = MoveFileExA("chatters.txt.tmp", "chatters.txt", MOVEFILE_REPLACE_EXISTING) != 0;
#else
  if (ok) ok = rename("chatters.txt.tmp", "chatters.txt") == 0;
#endif
  if (!ok) remove("chatters.txt.tmp");
}

void DialogueOverride_NoteChatter(const char *name) {
  static const char *const kBots[] = {"streamelements", "nightbot", "moobot", "fossabot", "wizebot"};
  if (!name || !name[0] || strchr(name, '?')) return;
  for (size_t i = 0; i < sizeof(kBots) / sizeof(kBots[0]); i++)
    if (!strcasecmp(name, kBots[i])) return;
  int i = DoPoolFind(name);
  if (i >= 0) g_do_pool_count[i]++;
  else i = DoPoolAdd(name, 1);
  if (i < 0) return;
  DoChattersWrite();                          // remembered for the next stream
}

// A pool name, weighted by the counter: a regular with 40+ messages is picked
// about forty times as often as a one-time hello. Staff weigh like a hello.
static uint32 DoWeight(int i) {
  if (DoIsIgnored(g_do_pool[i])) return 1;
  return 1 + (uint32)(g_do_pool_count[i] < 40 ? g_do_pool_count[i] : 40);
}
static const char *DoPickPoolName(uint32 r) {
  if (g_do_pool_n == 0) return NULL;
  uint32 total = 0;
  for (int i = 0; i < g_do_pool_n; i++) total += DoWeight(i);
  uint32 pick = r % total;
  for (int i = 0; i < g_do_pool_n; i++) {
    uint32 w = DoWeight(i);
    if (pick < w) return g_do_pool[i];
    pick -= w;
  }
  return g_do_pool[0];
}

// leaderboard order: non-staff by count (desc, stable), then the staff
int DialogueOverride_RankedChatter(int rank, char *out, size_t cap) {
  if (rank < 0 || rank >= g_do_pool_n) return 0;
  int order[DO_POOL_MAX];
  for (int i = 0; i < g_do_pool_n; i++) order[i] = i;
  for (int i = 1; i < g_do_pool_n; i++) {
    int k = order[i], j = i;
    while (j > 0 && DoRankKey(order[j - 1]) < DoRankKey(k)) { order[j] = order[j - 1]; j--; }
    order[j] = k;
  }
  snprintf(out, cap, "%s", g_do_pool[order[rank]]);
  return 1;
}

// The counter as a line for the RESULT/console (top names first).
int DialogueOverride_ChatterCount(const char *name) {
  int i = DoPoolFind(name);
  return i >= 0 ? g_do_pool_count[i] : 0;
}

static uint32 DoRand(void) {
  g_do_rng ^= g_do_rng << 13; g_do_rng ^= g_do_rng >> 17; g_do_rng ^= g_do_rng << 5;
  return g_do_rng;
}
static int g_do_last_alt, g_do_last_dirty;   // for the RESULT line (harness)
static const char *DoPick(int index, bool want_hint) {
  int n = g_do_nalt[index];
  if (n == 0) return NULL;
  // CLEAN: only the alternates without cursing; none left -> original text.
  // Hint-tagged lines only serve HINTS = JOKES, and never the other way.
  int ok[DO_MAX_ALT], nok = 0;
  for (int a = 0; a < n; a++)
    if ((g_do_mode == 2 || !g_do_dirty[index][a]) && (g_do_hintline[index][a] != 0) == want_hint &&
        (g_do_gamejokes || !g_do_gameline[index][a])) ok[nok++] = a;
  if (nok == 0) return NULL;
  int pick = ok[nok == 1 ? 0 : (int)(DoRand() % (uint32)nok)];
  g_do_last_alt = pick;
  g_do_last_dirty = g_do_dirty[index][pick];
  return g_do_msg[index][pick];
}

// ---- token stream -------------------------------------------------------
enum { kTok_Letter, kTok_Cmd, kTok_Break };
typedef struct DoTok {
  uint8 kind;
  uint8 letter;   // kTok_Letter
  uint8 cmd;      // index into kDoCmds
  uint8 param;
} DoTok;

#define DO_MAX_TOK 1200
static DoTok g_do_tok[DO_MAX_TOK];

static int DoFindCmd(const char *name, size_t n) {
  for (size_t i = 0; i < sizeof(kDoCmds) / sizeof(kDoCmds[0]); i++)
    if (strlen(kDoCmds[i].name) == n && !strncmp(kDoCmds[i].name, name, n))
      return (int)i;
  return -1;
}

static int DoFindGlyph(const char *name, size_t n) {
  for (size_t i = 0; i < sizeof(kDoGlyphs) / sizeof(kDoGlyphs[0]); i++)
    if (strlen(kDoGlyphs[i].name) == n && !strncmp(kDoGlyphs[i].name, name, n))
      return kDoGlyphs[i].idx;
  return -1;
}

static int DoPushLetters(int nt, const char *s) {
  for (; *s && nt < DO_MAX_TOK; s++) {
    int li = DoLetterIndex((uint8)*s);
    if (li < 0) continue;
    g_do_tok[nt].kind = kTok_Letter, g_do_tok[nt].letter = (uint8)li, nt++;
  }
  return nt;
}

// Decodes a few common UTF-8 punctuation marks into ASCII.
static int DoUtf8Fold(const uint8 *p, int *adv) {
  *adv = 1;
  if (p[0] == 0xe2 && p[1] == 0x80) {
    *adv = 3;
    switch (p[2]) {
    case 0x98: case 0x99: return '\'';
    case 0x9c: case 0x9d: return '"';
    case 0x93: case 0x94: return '-';
    case 0xa6: return -2;  // ellipsis
    }
    return ' ';
  }
  if (p[0] >= 0xc0) {  // any other multibyte: skip it whole
    *adv = (p[0] >= 0xf0) ? 4 : (p[0] >= 0xe0) ? 3 : 2;
    return ' ';
  }
  return p[0];
}

// Tokenizes |text| and reports whether it carries explicit line codes.
static int DoTokenize(const char *text, bool *has_lines, int msgno, bool warn) {
  int nt = 0;
  *has_lines = false;
  const uint8 *p = (const uint8 *)text;
  while (*p && nt < DO_MAX_TOK - 2) {
    if (*p == '[') {
      const uint8 *q = (const uint8 *)strchr((const char *)p + 1, ']');
      if (!q) { p++; continue; }
      const char *name = (const char *)p + 1;
      size_t n = q - p - 1, nn = n;
      int param = -1;
      const char *sp = memchr(name, ' ', n);
      if (sp) { nn = sp - name; param = atoi(sp + 1); }
      int gi = DoFindGlyph(name, nn);
      int ci = gi < 0 ? DoFindCmd(name, nn) : -1;
      if (gi >= 0) {
        g_do_tok[nt].kind = kTok_Letter, g_do_tok[nt].letter = (uint8)gi, nt++;
      } else if (ci >= 0) {
        uint8 c = kDoCmds[ci].us_cmd;
        if (c == kDoCmd_1 || c == kDoCmd_2 || c == kDoCmd_3 || c == kDoCmd_Scroll)
          *has_lines = true;
        g_do_tok[nt].kind = kTok_Cmd, g_do_tok[nt].cmd = (uint8)ci;
        g_do_tok[nt].param = (uint8)(param < 0 ? 0 : param), nt++;
      } else if (warn) {
        printf("[dialogue] msg %d: unknown code [%.*s]\n", msgno, (int)n, name);
      }
      p = q + 1;
      continue;
    }
    if (*p == '{') {
      const uint8 *q = (const uint8 *)strchr((const char *)p + 1, '}');
      size_t tn = q ? (size_t)(q - p - 1) : 0;
      bool is_chatter = q && tn == 7 && !strncmp((const char *)p + 1, "chatter", 7);
      bool is_boss = q && tn == 4 && !strncmp((const char *)p + 1, "boss", 4);
      if (is_chatter || is_boss) {
        // {boss}: the CHAT BOSS chat is fighting (or fought last); before the
        // first boss it falls through to a chatter name like {chatter}
        char who[64];
        if (!(is_boss && Twitch_BossName(who, sizeof who)) &&
            !((DoRand() & 1) && Twitch_RandomChatter(who, sizeof who))) {
          const char *pn = DoPickPoolName(DoRand());   // regulars weigh more (chatters.txt counts)
          if (pn)
            snprintf(who, sizeof who, "%s", pn);
          else if (!Twitch_RandomChatter(who, sizeof who))
            snprintf(who, sizeof who, "chat");
        }
        nt = DoPushLetters(nt, who);
        p = q + 1;
        continue;
      }
      p++;
      continue;
    }
    if (*p == '|') {
      g_do_tok[nt++].kind = kTok_Break;
      p++;
      continue;
    }
    int adv, ch = DoUtf8Fold(p, &adv);
    p += adv;
    if (ch == -2) {
      g_do_tok[nt].kind = kTok_Letter, g_do_tok[nt].letter = 67, nt++;
      continue;
    }
    if (ch == '\r' || ch == '\n') continue;
    int li = DoLetterIndex(ch);
    if (li < 0) {
      if (warn && ch >= 0x20 && ch < 0x7f)
        printf("[dialogue] msg %d: '%c' is not in the font, dropped\n", msgno, ch);
      continue;
    }
    g_do_tok[nt].kind = kTok_Letter, g_do_tok[nt].letter = (uint8)li, nt++;
  }
  return nt;
}

// ---- emit ---------------------------------------------------------------
typedef struct DoOut { uint8 *p, *end; bool eu; bool overflow; } DoOut;

static void DoEmitByte(DoOut *o, uint8 b) {
  if (o->p < o->end) *o->p++ = b; else o->overflow = true;
}

static void DoEmitCmd(DoOut *o, int ci, int param) {
  const DoCmdInfo *c = &kDoCmds[ci];
  if (o->eu) {
    if (c->eu_byte == 0x87) {
      DoEmitByte(o, 0x87);
      uint8 second = c->eu_param;
      if (c->us_cmd == kDoCmd_Wait || c->us_cmd == kDoCmd_Color ||
          c->us_cmd == kDoCmd_Number || c->us_cmd == kDoCmd_Speed)
        second += param & 0xf;
      else if (c->us_cmd == kDoCmd_Position)
        second += param & 1;
      DoEmitByte(o, second);
    } else {
      DoEmitByte(o, c->eu_byte);
    }
  } else {
    DoEmitByte(o, (uint8)(0x67 + c->us_cmd));
    if (c->us_param) {
      int v = param;
      if (c->us_cmd == kDoCmd_Window) v = 2;
      if (c->us_cmd == kDoCmd_Sound) v = 45;
      DoEmitByte(o, (uint8)v);
    }
  }
}

static int DoCmdIndex(uint8 us_cmd) {
  for (size_t i = 0; i < sizeof(kDoCmds) / sizeof(kDoCmds[0]); i++)
    if (kDoCmds[i].us_cmd == us_cmd) return (int)i;
  return -1;
}

// Vanilla page pattern: line1 [2] line2 [3] line3 [Waitkey][Scroll] line4
// [Scroll] line5 [Scroll] line6 [Waitkey][Scroll] ...
static void DoEmitLineBreak(DoOut *o, int line_no) {
  if (line_no == 1) DoEmitCmd(o, DoCmdIndex(kDoCmd_2), 0);
  else if (line_no == 2) DoEmitCmd(o, DoCmdIndex(kDoCmd_3), 0);
  else {
    if (line_no % 3 == 0) DoEmitCmd(o, DoCmdIndex(kDoCmd_Waitkey), 0);
    DoEmitCmd(o, DoCmdIndex(kDoCmd_Scroll), 0);
  }
}

// Commands that expand to glyphs at load time count against the line:
// [Name] is up to 6 glyphs (worst case 8 px each), [Number NN] one digit.
static int DoCmdWidthPx(int ci) {
  uint8 c = kDoCmds[ci].us_cmd;
  if (c == kDoCmd_Name) return 6 * 8;
  if (c == kDoCmd_Number) return 8;
  return 0;
}

static const uint8 *DoFontWidths(void) {
  if (g_zenv.dialogue_font_blk.ptr == NULL) return NULL;
  return FindIndexInMemblk(g_zenv.dialogue_font_blk, 1).ptr;
}

// ---- REAL hints ------------------------------------------------------------
static bool DoIsFortuneMsg(int index) {   // kFortuneTeller_Readings in sprite_main.c
  return (index >= 0xea && index <= 0xf1) || (index >= 0xf6 && index <= 0xfd);
}
static bool DoIsHintMsg(int index) { return DoIsFortuneMsg(index) || Dungeon_IsTeleMsg(index); }

// does Link already carry this item? (progressive items are never filtered)
static bool DoItemOwned(const char *item) {
  struct { const char *key; int have; } k[] = {
    { "Hookshot", link_item_hookshot }, { "Bow", link_item_bow }, { "Boomerang", link_item_boomerang },
    { "Hammer", link_item_hammer }, { "Fire Rod", link_item_fire_rod }, { "Ice Rod", link_item_ice_rod },
    { "Bombos", link_item_bombos_medallion }, { "Ether", link_item_ether_medallion }, { "Quake", link_item_quake_medallion },
    { "Lamp", link_item_torch }, { "Ocarina", link_item_flute >= 2 }, { "Shovel", link_item_flute & 1 },
    { "Book of Mudora", link_item_book_of_mudora }, { "Cane of Somaria", link_item_cane_somaria },
    { "Cane of Byrna", link_item_cane_byrna }, { "Cape", link_item_cape }, { "Magic Mirror", link_item_mirror },
    { "Pegasus Boots", link_item_boots }, { "Flippers", link_item_flippers }, { "Moon Pearl", link_item_moon_pearl },
    { "Mushroom", link_item_mushroom == 1 }, { "Magic Powder", link_item_mushroom == 2 },
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(item, k[i].key)) return k[i].have != 0;
  return false;
}

// the reference randomizer's location prefixes for the dungeon Link is in
static int DoDungeonPrefixes(const char **out, int cap) {
  int n = 0;
  switch (cur_palace_index_x2) {
  case 0x00: out[n++] = "Sewers"; out[n++] = "Hyrule Castle"; break;
  case 0x02: out[n++] = "Hyrule Castle"; out[n++] = "Sewers"; break;
  case 0x04: out[n++] = "Eastern Palace"; break;
  case 0x06: out[n++] = "Desert Palace"; break;
  case 0x08: out[n++] = "Castle Tower"; out[n++] = "Agahnims Tower"; break;
  case 0x0a: out[n++] = "Swamp Palace"; break;
  case 0x0c: out[n++] = "Palace of Darkness"; break;
  case 0x0e: out[n++] = "Misery Mire"; break;
  case 0x10: out[n++] = "Skull Woods"; break;
  case 0x12: out[n++] = "Ice Palace"; break;
  case 0x14: out[n++] = "Tower of Hera"; break;
  case 0x16: out[n++] = "Thieves' Town"; out[n++] = "Thieves Town"; break;
  case 0x18: out[n++] = "Turtle Rock"; break;
  case 0x1a: out[n++] = "Ganons Tower"; out[n++] = "Ganon's Tower"; break;
  default: break;
  }
  (void)cap;
  return n;
}

static const char *DoSpotName(const char *loc) {   // "Eastern Palace - Big Chest" -> "Big Chest"
  const char *d = strstr(loc, " - ");
  return d ? d + 3 : loc;
}

static int DoBuildTileHint(char *out, size_t cap) {
  const char *pre[4]; int np = DoDungeonPrefixes(pre, 4);
  int pick[2], npick = 0;
  for (int pass = 0; pass < 2 && npick < 2; pass++) {        // pass 0: not owned, pass 1: anything
    for (int i = 0; i < g_do_place_n && npick < 2; i++) {
      bool here = false;
      for (int p = 0; p < np; p++)
        if (!strncmp(g_do_place[i].loc, pre[p], strlen(pre[p]))) here = true;
      if (!here) continue;
      if (pass == 0 && DoItemOwned(g_do_place[i].item)) continue;
      bool dup = false;
      for (int q = 0; q < npick; q++) if (pick[q] == i) dup = true;
      if (!dup) pick[npick++] = i;
    }
  }
  if (np == 0 || npick == 0)
    return snprintf(out, cap, "[Window 02][Name], it is I, Sahasrahla. Nothing you need is hidden in this place. Take what you like and move on.");
  if (npick == 1)
    return snprintf(out, cap, "[Window 02][Name], it is I, Sahasrahla. In this place the %s waits at the %s.",
                    g_do_place[pick[0]].item, DoSpotName(g_do_place[pick[0]].loc));
  return snprintf(out, cap, "[Window 02][Name], it is I, Sahasrahla. In this place the %s waits at the %s, and the %s at the %s.",
                  g_do_place[pick[0]].item, DoSpotName(g_do_place[pick[0]].loc),
                  g_do_place[pick[1]].item, DoSpotName(g_do_place[pick[1]].loc));
}

static int DoBuildFortuneHint(char *out, size_t cap) {
  int cand[DO_PLACE_MAX], nc = 0;
  for (int i = 0; i < g_do_place_n; i++)
    if (!DoItemOwned(g_do_place[i].item)) cand[nc++] = i;
  if (nc == 0)
    for (int i = 0; i < g_do_place_n; i++) cand[nc++] = i;
  if (nc == 0)
    return snprintf(out, cap, "[Position 01]Hocus pocus! You already carry everything that matters. Go and finish it.");
  int i = cand[DoRand() % (uint32)nc];
  char where[64];
  snprintf(where, sizeof where, "%s", g_do_place[i].loc);
  char *d = strstr(where, " - ");
  if (d) { d[0] = ','; memmove(d + 2, d + 3, strlen(d + 3) + 1); }   // "Palace, Big Chest"
  return snprintf(out, cap, "[Position 01]%s The %s rests at %s.", (DoRand() & 1) ? "Hocus pocus!" : "Abracadabra alakazam!",
                  g_do_place[i].item, where);
}

int DialogueOverride_Get(int index, uint8 *dst, int cap) {
  if (index < 0 || index >= DO_MAX_MSGS) return 0;
  const char *text = NULL;
  char hint[400];
  if (DoIsHintMsg(index)) {
    if (g_do_hints == 1) {
      int n = DoIsFortuneMsg(index) ? DoBuildFortuneHint(hint, sizeof hint) : DoBuildTileHint(hint, sizeof hint);
      if (n > 0) text = hint;
      if (Twitch_Debug() && text) printf("RESULT hint msg=%d real \"%s\"%c", index + 1, hint, 10), fflush(stdout);
    } else if (g_do_hints == 2) {
      text = DoPick(index, true);
    }
    if (!text) return 0;
  } else {
    if (g_do_mode == 0) return 0;
    text = DoPick(index, false);
    if (!text) return 0;
  }
  if (Twitch_Debug())
    printf("RESULT dialogue msg=%d alt=%d dirty=%d mode=%d%c", index + 1, g_do_last_alt, g_do_last_dirty, g_do_mode, 10), fflush(stdout);
  bool has_lines;
  int nt = DoTokenize(text, &has_lines, index, false);
  DoOut o = {dst, dst + cap, (g_zenv.dialogue_flags & 1) != 0, false};
  const uint8 *widths = DoFontWidths();

  if (has_lines || !widths) {
    for (int i = 0; i < nt; i++) {
      const DoTok *t = &g_do_tok[i];
      if (t->kind == kTok_Letter) DoEmitByte(&o, t->letter);
      else if (t->kind == kTok_Cmd) DoEmitCmd(&o, t->cmd, t->param);
    }
  } else {
    // word wrap by pixel width; commands are zero-width and stay in place
    int line_px = 0, line_no = 0;
    int i = 0;
    while (i < nt) {
      // gather the next word (letters up to and including a space) with its width
      int j = i, px = 0;
      bool forced = false;
      while (j < nt) {
        const DoTok *t = &g_do_tok[j];
        if (t->kind == kTok_Break) { forced = true; break; }
        if (t->kind == kTok_Letter) px += widths[t->letter];
        else if (t->kind == kTok_Cmd) px += DoCmdWidthPx(t->cmd);
        j++;
        if (t->kind == kTok_Letter && t->letter == 89) break;  // space ends a word
      }
      // trailing space does not count against the line
      int px_no_space = px;
      if (j > i && g_do_tok[j - 1].kind == kTok_Letter && g_do_tok[j - 1].letter == 89)
        px_no_space -= widths[89];
      if (line_px > 0 && line_px + px_no_space > DO_LINE_MAX_PX) {
        DoEmitLineBreak(&o, ++line_no);
        line_px = 0;
      }
      for (int k = i; k < j; k++) {
        const DoTok *t = &g_do_tok[k];
        if (t->kind == kTok_Letter) {
          if (line_px == 0 && t->letter == 89) continue;  // no leading space
          DoEmitByte(&o, t->letter);
          line_px += widths[t->letter];
        } else if (t->kind == kTok_Cmd) {
          DoEmitCmd(&o, t->cmd, t->param);
          line_px += DoCmdWidthPx(t->cmd);
        }
      }
      i = j;
      if (forced) {
        DoEmitLineBreak(&o, ++line_no);
        line_px = 0;
        i++;
      }
    }
  }
  if (o.overflow) {
    printf("[dialogue] msg %d: too long, truncated\n", index);
    return cap;
  }
  return (int)(o.p - dst);
}

int DialogueOverride_Count(void) { return g_do_count; }

void DialogueOverride_Init(void) {
  size_t len = 0;
  g_do_rng ^= (uint32)time(NULL) * 2654435761u;
  if (!g_do_rng) g_do_rng = 0x2545f491;
  {
    uint8 *ini = ReadWholeFile("text.ini", &len);
    if (ini) {
      const char *j = strstr((const char *)ini, "jokes=");
      if (j) { g_do_mode = atoi(j + 6); if (g_do_mode < 0 || g_do_mode > 2) g_do_mode = 1; }
      const char *h = strstr((const char *)ini, "hints=");
      if (h) { g_do_hints = atoi(h + 6); if (g_do_hints < 0 || g_do_hints > 2) g_do_hints = 0; }
      const char *c = strstr((const char *)ini, "credits=");
      if (c) g_do_credits = atoi(c + 8) ? 1 : 0;
      const char *g = strstr((const char *)ini, "gamejokes=");
      if (g) g_do_gamejokes = atoi(g + 10) ? 1 : 0;
      free(ini);
    }
  }
  uint8 *data = ReadWholeFile("dialogue.txt", &len);
  if (!data) return;
  char *s = (char *)data;
  char *copy = strdup(s);
  // strip a UTF-8 BOM
  if (len >= 3 && (uint8)s[0] == 0xef && (uint8)s[1] == 0xbb && (uint8)s[2] == 0xbf) s += 3;
  int loaded = 0, dirty_n = 0;
  for (char *line = strtok(s, "\n"); line; line = strtok(NULL, "\n")) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (!n || line[0] == '#') continue;
    if (!strncmp(line, "chatters:", 9)) continue;  // parsed below
    char *colon = strchr(line, ':');
    if (!colon) continue;
    // dialogue.txt numbers lines from 1; the engine's message index is 0-based
    // (verified in-game: index 16 shows the file's line 17)
    int idx = atoi(line) - 1;
    if (idx < 0 || idx >= DO_MAX_MSGS) continue;
    // tags between the number and the colon: x = dirty, h = hint joke (only
    // used when HINTS is JOKES; never for the ordinary TEXT jokes)
    bool tagged_dirty = false, tagged_hint = false, tagged_game = false;
    for (const char *t = line; t < colon; t++) {
      if (*t == 'x' || *t == 'X') tagged_dirty = true;
      if (*t == 'h' || *t == 'H') tagged_hint = true;
      if (*t == 'r' || *t == 'R') tagged_game = true;   // game jokes (RL / EQ / UO), own switch
    }
    const char *text = colon + 1;
    if (*text == ' ') text++;
    if (g_do_nalt[idx] < DO_MAX_ALT) {
      int a = g_do_nalt[idx]++;
      g_do_msg[idx][a] = strdup(text);
      g_do_hintline[idx][a] = tagged_hint;
      g_do_gameline[idx][a] = tagged_game;
      g_do_dirty[idx][a] = tagged_dirty || DoIsDirty(text);
      if (g_do_dirty[idx][a]) dirty_n++;
    }
    if (g_do_nalt[idx] == 1) loaded++;
  }
  // second pass for the chatter pool (strtok above is not re-entrant)
  const char *cp = strstr(copy, "chatters:");
  if (cp) {
    cp += 9;
    while (*cp && *cp != '\n' && *cp != '\r' && g_do_pool_n < DO_POOL_MAX) {
      while (*cp == ' ' || *cp == ',') cp++;
      const char *e = cp;
      while (*e && *e != ',' && *e != '\n' && *e != '\r') e++;
      size_t n = e - cp;
      while (n && cp[n - 1] == ' ') n--;
      if (n && n < 32) {
        char nm[32];
        memcpy(nm, cp, n);
        nm[n] = 0;
        DoPoolAdd(nm, 0);
      }
      cp = e;
    }
  }
  free(copy);
  free(data);
  g_do_count = loaded;
  // chatters.txt: "name count" per line (a bare name counts as 1), seeded
  // from the stream's VOD chat logs and grown by the game itself as chatters
  // show up live; "staff: a, b" names are counted but never ranked ("ignore:"
  // in an older file means the same).
  {
    size_t clen = 0;
    uint8 *cd = ReadWholeFile("chatters.txt", &clen);
    g_do_chatters_header[0] = 0;
    if (cd) {
      size_t hn = 0;
      for (char *ln = strtok((char *)cd, "\n"); ln; ln = strtok(NULL, "\n")) {
        size_t n = strlen(ln);
        while (n && (ln[n - 1] == 13 || ln[n - 1] == ' ')) ln[--n] = 0;
        while (*ln == ' ') ln++;
        if (!*ln) continue;
        if (*ln == '#') {
          if (hn + n + 2 < sizeof g_do_chatters_header)
            hn += (size_t)snprintf(g_do_chatters_header + hn, sizeof g_do_chatters_header - hn, "%s\n", ln);
          continue;
        }
        if (!strncasecmp(ln, "ignore:", 7) || !strncasecmp(ln, "staff:", 6)) {
          char *p = ln + (ln[0] == 'i' || ln[0] == 'I' ? 7 : 6);
          while (*p && g_do_ignore_n < DO_IGNORE_MAX) {
            while (*p == ' ' || *p == ',') p++;
            char *e = p;
            while (*e && *e != ',' && *e != ' ') e++;
            if (e > p && e - p < 32) {
              memcpy(g_do_ignore[g_do_ignore_n], p, e - p);
              g_do_ignore[g_do_ignore_n][e - p] = 0;
              g_do_ignore_n++;
            }
            p = e;
          }
          continue;
        }
        char *sp = strchr(ln, ' ');
        if (!sp) sp = strchr(ln, '\t');
        int count = 1;
        if (sp) { *sp = 0; count = atoi(sp + 1); if (count < 0) count = 0; }
        DoPoolAdd(ln, count);
      }
      free(cd);
    }
  }
  // validate once so typos show up at boot instead of in a message box
  int alts = 0;
  for (int i = 0; i < DO_MAX_MSGS; i++) {
    for (int a = 0; a < g_do_nalt[i]; a++) {
      bool has_lines;
      DoTokenize(g_do_msg[i][a], &has_lines, i + 1, true);
      if (a) alts++;
    }
  }
  printf("[dialogue] %d message overrides loaded from dialogue.txt (+%d alternates, %d dirty, %d chatter names, %d staff) mode=%s hints=%s\n",
         loaded, alts, dirty_n, g_do_pool_n, g_do_ignore_n, DialogueOverride_ModeLabel(), DialogueOverride_HintsLabel());
  {
    int best = -1;
    for (int i = 0; i < g_do_pool_n; i++)
      if (!DoIsIgnored(g_do_pool[i]) && (best < 0 || g_do_pool_count[i] > g_do_pool_count[best])) best = i;
    if (best >= 0)
      printf("[dialogue] top chatter: %s (%d messages)\n", g_do_pool[best], g_do_pool_count[best]);
  }
}

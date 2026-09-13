#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif
#include "credits_names.h"
#include "dialogue_override.h"
#include "util.h"

// The top tile row of every two-row staff name in kEnding_Credits_Text (the
// bottom half is the next row). Decoded from the asset: names use the small
// font, A = 93 on the top row and A = 131 on the bottom row, 159 = space,
// length word = 2 * letters - 1, x = (32 - letters) / 2.
static const uint16 kNameRows[] = {
  4, 18, 32, 46, 60, 64, 80, 84, 98, 102, 118, 132, 146, 160, 164, 168, 172,
  176, 180, 184, 188, 204, 218, 222, 236, 240, 244, 248, 252, 268, 272, 286,
  290, 294, 298, 302, 306,
};
// PORT INFO section (owner 09-12: "update the end credits with port info
// from both the original Zelda 3 PC port creators and my port, and Patreon
// stuff", "scratch the Discord"): a few name-pair blocks before the patrons
// are repurposed as heading + content lines instead of staff names. Content
// is data-driven from credits_port.txt (kPortDefault below if missing).
//
// kPortBlocks are indices into kNameRows whose title row (4 rows above the
// name pair) is free of any other name-pair row - i.e. safe to blank or
// caption without corrupting a neighbouring name. Blocks not in this list
// (5, 7, 9, ...) can't be captioned safely, so credits_port.txt content
// never lands on one; it just shows through as the original staff credit,
// same as any other slot with no name assigned to it.
static const int kPortBlocks[] = { 0, 1, 2, 3, 4, 6, 8, 10 };
#define kPortBlocksN (sizeof(kPortBlocks) / sizeof(kPortBlocks[0]))
static const struct { uint16 row; uint8 base; } kPortTitleRow[kPortBlocksN] = {
  { 0, 56 }, { 14, 26 }, { 28, 0 }, { 42, 56 }, { 56, 26 }, { 76, 0 }, { 94, 56 }, { 114, 26 },
};

#define kPortLinesMax kPortBlocksN
static char g_port_headings[kPortBlocksN][40];   // "" = blank the title row
static char g_port_lines[kPortLinesMax][32];
static int g_port_lines_n = -1;   // -1 = credits_port.txt not read yet

// "=" prefixes a heading that captions the next content line; the built-in
// fallback when credits_port.txt is missing or unreadable.
static const char *kPortDefault[] = {
  "=ZELDA III PC PORT", "BY SNESREV", "ORIGINAL GAME", "NINTENDO",
  "=CHAOS STREAM BUILD", "BY GAINEY", "TWITCH GAINEY",
  "=PATREON SUPPORTERS", "SUPPORT ON PATREON",
};

static void PortInfoLoad(void) {
  g_port_lines_n = 0;
  for (size_t i = 0; i < kPortBlocksN; i++) g_port_headings[i][0] = 0;
  char heading_buf[40];
  const char *pending = NULL;
  size_t len = 0;
  uint8 *data = ReadWholeFile("credits_port.txt", &len);
  char *ln = data ? strtok((char *)data, "\n") : NULL;
  int use_default = !data;
  int di = 0;
  for (;;) {
    const char *cur;
    if (use_default) {
      if (di >= (int)(sizeof(kPortDefault) / sizeof(kPortDefault[0]))) break;
      cur = kPortDefault[di++];
    } else {
      if (!ln) break;
      size_t n = strlen(ln);
      while (n && (ln[n - 1] == '\r' || ln[n - 1] == ' ')) ln[--n] = 0;
      while (*ln == ' ') ln++;
      cur = ln;
      ln = strtok(NULL, "\n");
      if (!*cur || *cur == '#') continue;
    }
    if (*cur == '=') {
      snprintf(heading_buf, sizeof heading_buf, "%s", cur + 1);
      pending = heading_buf;
      continue;
    }
    if (g_port_lines_n >= (int)kPortLinesMax) continue;
    snprintf(g_port_lines[g_port_lines_n], 32, "%s", cur);
    if (pending && g_port_lines_n < (int)kPortBlocksN)
      snprintf(g_port_headings[g_port_lines_n], 40, "%s", pending);
    pending = NULL;
    g_port_lines_n++;
  }
  if (data) free(data);
  printf("[credits] %d port-info lines\n", g_port_lines_n);
}

// which physical name-pair block a port-info content line sits on, or -1
static int PortBlockLineIndex(int pair) {
  for (size_t i = 0; i < kPortBlocksN; i++) if (kPortBlocks[i] == pair) return (int)i;
  return -1;
}

// first block after the port-info section: patrons/chatters start there
static int PortReservedBlocks(void) {
  if (g_port_lines_n <= 0) return 0;
  int last = g_port_lines_n - 1;
  if (last >= (int)kPortBlocksN) last = (int)kPortBlocksN - 1;
  return kPortBlocks[last] + 1;
}

#define kPatronsMax 8
static char g_patrons[kPatronsMax][32];
static int g_patrons_n = -1;   // -1 = patrons.txt not read yet

// patrons.txt: one name per line, "name aka alias" keeps the part before aka
static void PatronsLoad(void) {
  g_patrons_n = 0;
  size_t len = 0;
  uint8 *data = ReadWholeFile("patrons.txt", &len);
  if (!data) return;
  for (char *ln = strtok((char *)data, "\n"); ln && g_patrons_n < kPatronsMax; ln = strtok(NULL, "\n")) {
    size_t n = strlen(ln);
    while (n && (ln[n - 1] == '\r' || ln[n - 1] == ' ')) ln[--n] = 0;
    while (*ln == ' ') ln++;
    if (!*ln || *ln == '#') continue;
    char *aka = strstr(ln, " aka ");
    if (aka) *aka = 0;
    snprintf(g_patrons[g_patrons_n++], 32, "%s", ln);
  }
  free(data);
  printf("[credits] %d patrons from patrons.txt\n", g_patrons_n);
}

static bool IsPatron(const char *name) {
  for (int i = 0; i < g_patrons_n; i++)
    if (!strcasecmp(g_patrons[i], name)) return true;
  return false;
}

// the name for name slot |slot|: patrons first, then the chatter leaderboard
// with the patrons skipped so nobody is listed twice
static bool NameForSlot(int slot, char *out, size_t cap) {
  if (slot < g_patrons_n) { snprintf(out, cap, "%s", g_patrons[slot]); return true; }
  int want = slot - g_patrons_n, seen = 0;
  for (int rank = 0;; rank++) {
    char name[64];
    if (!DialogueOverride_RankedChatter(rank, name, sizeof name)) return false;
    if (IsPatron(name)) continue;
    if (seen++ == want) { snprintf(out, cap, "%s", name); return true; }
  }
}

// letters, space and apostrophe only: upper case, digits in the usual leet
// spots, everything else dropped, |max| letters
static int CleanName(const char *name, char *clean, int max) {
  int len = 0;
  for (const char *p = name; *p && len < max; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == '_' || c == '-' || c == '.') c = ' ';
    if (c == '0') c = 'O'; else if (c == '1') c = 'I'; else if (c == '3') c = 'E';
    else if (c == '4') c = 'A'; else if (c == '5') c = 'S'; else if (c == '7') c = 'T';
    if ((c >= 'A' && c <= 'Z') || c == ' ' || c == '\'') clean[len++] = c;
  }
  while (len && clean[len - 1] == ' ') len--;
  clean[len] = 0;
  return len;
}

const uint8 *Credits_CommunityRow(int r18) {
  static uint8 buf[2 + 20];
  if (!DialogueOverride_CreditsMode()) return NULL;
  if (g_patrons_n < 0) PatronsLoad();
  if (g_port_lines_n < 0) PortInfoLoad();

  // PORT INFO / CHAOS STREAM BUILD / PATREON SUPPORTERS headings; "" blanks
  // the row instead of leaving the vanilla staff title above our line
  for (size_t i = 0; i < kPortBlocksN; i++) {
    if (r18 != kPortTitleRow[i].row) continue;
    if ((int)i >= g_port_lines_n) return NULL;   // no line placed here: show vanilla
    const char *title = g_port_headings[i];
    if (!*title) { buf[0] = 0xff; return buf; }
    int len = (int)strlen(title);
    buf[0] = (uint8)((32 - len) / 2);
    buf[1] = (uint8)(2 * len - 1);
    for (int k = 0; k < len; k++)
      buf[2 + k] = title[k] == ' ' ? 159 : (uint8)(kPortTitleRow[i].base + (title[k] - 'A'));
    return buf;
  }

  int pair = -1, half = 0;
  for (size_t i = 0; i < sizeof(kNameRows) / sizeof(kNameRows[0]); i++) {
    if (r18 == kNameRows[i]) { pair = (int)i; half = 0; break; }
    if (r18 == kNameRows[i] + 1) { pair = (int)i; half = 1; break; }
  }
  if (pair < 0) return NULL;
  char name[64], clean[17];
  int reserved = PortReservedBlocks();
  if (pair < reserved) {
    // inside the port-info section: kPortBlocks entries still unused because
    // credits_port.txt authored fewer lines than the table's headroom are
    // NOT reserved (reserved stops at the last used one), so any pair here
    // that isn't a used line is block 5/7/9-style - a safe-caption slot
    // that just isn't captioned this run - either way: vanilla staff name.
    int pli = PortBlockLineIndex(pair);
    if (pli < 0) return NULL;
    snprintf(name, sizeof name, "%s", g_port_lines[pli]);
  } else if (!NameForSlot(pair - reserved, name, sizeof name)) {
    return NULL;                             // fewer names than rows: the staff keep those
  }
  int len = CleanName(name, clean, 16);
  if (len == 0) return NULL;
  buf[0] = (uint8)((32 - len) / 2);
  buf[1] = (uint8)(2 * len - 1);
  int base = half ? 131 : 93, apos = half ? 157 : 119;
  for (int i = 0; i < len; i++)
    buf[2 + i] = clean[i] == ' ' ? 159 : clean[i] == '\'' ? (uint8)apos : (uint8)(base + (clean[i] - 'A'));
  return buf;
}

// Config-driven Link sprite selection (v1, no menu).
//
// The engine renders Link by DMA'ing tiles out of the kLinkGraphics asset
// (0x7000 bytes of uncompressed 4bpp SNES tiles, the same layout as the
// vanilla ROM's Link graphics region $0x80000-$0x86FFF, which is exactly
// what an ALTTPR .zspr file's pixel block covers) every NMI
// (src/nmi.c NMI_LinkGfxDMAs, see also kLinkDmaSources in src/misc.c).
// Link's colors come from the kPalette_ArmorAndGloves asset (10 palettes of
// 15 u15 BGR colors; the first 4 rows are the tunic/bunny palettes, loaded
// at runtime by Palette_Load_LinkArmorAndGloves / LoadGearPalettes in
// src/load_gfx.c) and kGlovesColor.
//
// So a whole-sprite swap is: copy the zspr pixel block over kLinkGraphics,
// and optionally the zspr palette block over ArmorAndGloves[0..3] and
// kGlovesColor. This mirrors what main.c ParseLinkGraphics() (the
// `link_graphics` zelda3.ini option) has always done; this module adds
// library-name resolution via sprites/library.json and is strictly
// non-fatal.

#include "sprites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "types.h"
#include "assets.h"
#include "load_gfx.h"
#include "util.h"
#include "settings_menu.h"

// --- tiny helpers -----------------------------------------------------------

static char *SkipWs(char *p) {
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

// Reads one `key=value` pair from an ini line. Returns 1 on success.
static int IniParseKeyValue(char *line, char **key, char **value) {
  line = SkipWs(line);
  if (*line == 0 || *line == '#' || *line == ';')
    return 0;
  char *eq = line;
  while (*eq && *eq != '=')
    eq++;
  if (*eq != '=')
    return 0;
  char *kend = eq;
  while (kend > line && (kend[-1] == ' ' || kend[-1] == '\t'))
    kend--;
  *kend = 0;
  char *v = SkipWs(eq + 1);
  char *vend = v + strlen(v);
  while (vend > v && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\r' || vend[-1] == '\n'))
    vend--;
  *vend = 0;
  *key = line;
  *value = v;
  return 1;
}

// Lowercases and strips everything that is not a letter or digit, so
// "Meatwad", "meatwad" and "Meat Wad" all match.
static void NormalizeName(const char *src, char *dst, size_t dst_size) {
  size_t n = 0;
  for (const char *p = src; *p && n + 1 < dst_size; p++) {
    if (isalnum((unsigned char)*p))
      dst[n++] = (char)tolower((unsigned char)*p);
  }
  dst[n] = 0;
}

// Extracts the file name part of a URL or path (".../meatwad.2.zspr").
static void BaseNameOf(const char *src, char *dst, size_t dst_size) {
  const char *slash = strrchr(src, '/');
  const char *bslash = strrchr(src, '\\');
  if (bslash > slash)
    slash = bslash;
  const char *base = slash ? slash + 1 : src;
  size_t n = strlen(base);
  while (n > 0 && (base[n - 1] == '\r' || base[n - 1] == '\n' || base[n - 1] == ' '))
    n--;
  if (n >= dst_size)
    n = dst_size - 1;
  memcpy(dst, base, n);
  dst[n] = 0;
}

// --- sprites/library.json name lookup ---------------------------------------
//
// The library is a copy of the ALTTPR sprite index JSON: an array of objects
// with "name" and "file" (a public S3 URL) keys, e.g.
//   {"name":"Meatwad", ..., "file":"https:\/\/...\/meatwad.2.zspr", ...}
// We do a targeted textual scan instead of a real JSON parser: find the
// next "name" key, read its string value, compare normalized; if it matches,
// read the following "file" key's string value and unescape it.

// Reads a JSON string literal starting at *pp (which must point at '"').
// Handles the \\ / \/ \" escapes the library actually uses. Returns a
// malloc'd string or NULL.
static char *JsonReadString(char **pp) {
  char *p = *pp;
  if (*p != '"')
    return NULL;
  p++;
  size_t cap = 128, n = 0;
  char *out = (char *)malloc(cap);
  if (!out)
    return NULL;
  while (*p && *p != '"') {
    char c = *p++;
    if (c == '\\' && (*p == '/' || *p == '\\' || *p == '"')) {
      c = *p++;  // unescape \/ \" \\ (all the library file URLs use \/)
    } else if (c == '\\' && *p) {
      c = *p++;  // any other escape: take the char raw (good enough here)
    }
    if (n + 2 > cap) {
      cap *= 2;
      char *tmp = (char *)realloc(out, cap);
      if (!tmp) {
        free(out);
        return NULL;
      }
      out = tmp;
    }
    out[n++] = c;
  }
  out[n] = 0;
  *pp = (*p == '"') ? p + 1 : p;
  return out;
}

// Scans forward for `"key"` followed by ':' and '"'. Returns a pointer at
// the opening quote of the value string, or NULL if not found.
static char *JsonFindStringField(char *p, const char *key) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  size_t pat_len = strlen(pat);
  while ((p = strchr(p, '"')) != NULL) {
    if (strncmp(p, pat, pat_len) == 0) {
      char *q = SkipWs(p + pat_len);
      if (*q != ':') {
        p += pat_len;
        continue;
      }
      q = SkipWs(q + 1);
      if (*q == '"')
        return q;
      p += pat_len;
      continue;
    }
    p++;
  }
  return NULL;
}

// Looks up `wanted` in the library json. On success fills file_url (the raw
// URL/path from the "file" field) and returns true.
static bool SpritesLibraryLookup(const char *json_path, const char *wanted,
                                 char *file_url, size_t file_url_size) {
  size_t length = 0;
  char want_norm[64];
  NormalizeName(wanted, want_norm, sizeof(want_norm));
  if (want_norm[0] == 0)
    return false;

  uint8 *data = ReadWholeFile(json_path, &length);
  if (data == NULL) {
    fprintf(stderr, "[sprites] library not found: %s\n", json_path);
    return false;
  }
  // NUL-terminate a private mutable copy.
  char *buf = (char *)malloc(length + 1);
  if (buf == NULL) {
    free(data);
    return false;
  }
  memcpy(buf, data, length);
  buf[length] = 0;
  free(data);

  char *p = buf;
  bool found = false;
  while (!found && (p = JsonFindStringField(p, "name")) != NULL) {
    char *name = JsonReadString(&p);
    if (name == NULL)
      break;
    char name_norm[64];
    NormalizeName(name, name_norm, sizeof(name_norm));
    free(name);
    if (strcmp(name_norm, want_norm) == 0) {
      char *f = JsonFindStringField(p, "file");
      if (f != NULL) {
        char *url = JsonReadString(&f);
        if (url != NULL) {
          BaseNameOf(url, file_url, file_url_size);
          found = file_url[0] != 0;
          free(url);
        }
      }
      if (!found)
        fprintf(stderr, "[sprites] '%s' found in library but has no file URL\n", wanted);
    }
  }
  free(buf);
  return found;
}

// --- zspr decode + apply ----------------------------------------------------

// Same layout as main.c ParseLinkGraphics (the `link_graphics` ini option):
// header <4xBHHIHIHH6x>: 'ZSPR', version u8, checksum u16, ~checksum u16,
// sprite_offset u32, sprite_size u16, palette_offset u32, palette_size u16,
// kind u16. Pixel block is 0x7000 bytes of uncompressed 4bpp tiles.
static bool ApplyZspr(const uint8 *file, size_t length, const char *display_name) {
  if (length < 27 || memcmp(file, "ZSPR", 4) != 0) {
    fprintf(stderr, "[sprites] %s: not a .zspr file\n", display_name);
    return false;
  }
  uint32 pixel_offs = DWORD(file[9]);
  uint32 pixel_length = WORD(file[13]);
  uint32 palette_offs = DWORD(file[15]);
  uint32 palette_length = WORD(file[19]);
  if ((uint64)pixel_offs + pixel_length > length ||
      (uint64)palette_offs + palette_length > length ||
      pixel_length != 0x7000) {
    fprintf(stderr, "[sprites] %s: bad zspr header/size\n", display_name);
    return false;
  }
  if (kPalette_ArmorAndGloves_SIZE != 150 || kLinkGraphics_SIZE != 0x7000) {
    fprintf(stderr, "[sprites] unexpected asset sizes (%d/%d), skipping\n",
            (int)kLinkGraphics_SIZE, (int)kPalette_ArmorAndGloves_SIZE);
    return false;
  }

  memcpy(kLinkGraphics, file + pixel_offs, 0x7000);
  if (palette_length >= 120)
    memcpy(kPalette_ArmorAndGloves, file + palette_offs, 120);
  if (palette_length >= 124)
    memcpy(kGlovesColor, file + palette_offs + 120, 4);

  fprintf(stderr, "[sprites] applied '%s' (%.1f KB graphics%s)\n", display_name,
          pixel_length / 1024.0,
          palette_length >= 120 ? " + palette" : ", graphics only (no palette block)");
  return true;
}

// --- sprite library catalog + in-game cycling --------------------------------
//
// The catalog is sprites/sprite_library.ini, generated by
// `tools/fetch_sprites.py --all` from sprites/library.json, one line per
// sprite (library order):
//   sprite=<display name>|<file name relative to sprites/>
// main.c routes F11 (next) / Shift+F11 (previous) into Sprite_Select_Cycle().
// Cycling reuses the exact ApplyZspr swap as boot, which is safe mid-game:
// kLinkGraphics is re-DMA'd into VRAM by NMI_DoUpdates every frame, and the
// armor palette rows are pushed on the next engine palette reload - made
// immediate here by calling Palette_Load_LinkArmorAndGloves (same call the
// engine makes at every area transition; runs on the same thread as the game
// loop, between frames). The pick is persisted by rewriting sprites.ini.

typedef struct SpriteLibEntry {
  char name[64];
  char file[256];
} SpriteLibEntry;

static SpriteLibEntry *g_sprlib;
static int g_sprlib_count;
static int g_sprlib_cur = -1;  // catalog index of the applied sprite; -1 = none/vanilla

// --- settings-menu row -------------------------------------------------------
//
// One SETTINGS row ("SPRITE") so the picker is reachable without the hotkeys:
// value_text shows the currently applied sprite's catalog name (uppercased,
// truncated to 28 chars so label + value stay within the menu's ~40-column
// row budget; "VANILLA" while nothing from the catalog is applied), and
// LEFT/RIGHT reuse Sprite_Select_Cycle, which applies live and persists to
// sprites.ini itself (no extra write here). F11/Shift+F11 (main.c) are
// unaffected. Registered only once, and only when the catalog actually
// loaded - without sprites/sprite_library.ini the feature is inert.

const char *Sprite_Select_CurrentName(void) {
  static char buf[29];  // 28 chars max: "SPRITE" + 4 + value + 2 <= 40 cols
  if (g_sprlib_cur < 0 || g_sprlib_cur >= g_sprlib_count)
    return "VANILLA";
  size_t n = 0;
  for (const char *p = g_sprlib[g_sprlib_cur].name; *p && n + 1 < sizeof(buf); p++)
    buf[n++] = (char)toupper((unsigned char)*p);
  buf[n] = 0;
  return buf;
}

static const char *SpriteRowValue(void) { return Sprite_Select_CurrentName(); }

static void SpriteRowAdjust(int delta) {
  if (delta > 0)
    Sprite_Select_Cycle(1);   // RIGHT
  else if (delta < 0)
    Sprite_Select_Cycle(-1);  // LEFT (RETURN = 0: nothing to activate)
}

static void SpritesRegisterMenuRow(void) {
  static bool registered;
  if (registered)
    return;
  static const SettingsRow kRowSprite = { "SPRITE", SpriteRowValue,
                                          SpriteRowAdjust };
  SettingsMenu_RegisterRow(&kRowSprite);
  registered = true;
}

static void SpritesLoadCatalog(void) {
  size_t length = 0;
  uint8 *data = ReadWholeFile("sprites/sprite_library.ini", &length);
  if (data == NULL) {
    fprintf(stderr, "[sprites] no catalog sprites/sprite_library.ini - sprite "
            "cycling off (generate with: python tools/fetch_sprites.py --all)\n");
    return;
  }
  char *text = (char *)malloc(length + 1);
  if (text == NULL) {
    free(data);
    return;
  }
  memcpy(text, data, length);
  text[length] = 0;
  free(data);

  int cap = 0;
  for (char *line = text; *line; ) {
    char *next = strchr(line, '\n');
    if (next)
      *next++ = 0;
    char *key, *value;
    if (IniParseKeyValue(line, &key, &value) && strcmp(key, "sprite") == 0) {
      char *bar = strrchr(value, '|');
      size_t name_len = bar ? (size_t)(bar - value) : 0;
      size_t file_len = bar ? strlen(bar + 1) : 0;
      if (bar != NULL && name_len > 0 && file_len > 0 &&
          name_len < sizeof(((SpriteLibEntry *)0)->name) &&
          file_len < sizeof(((SpriteLibEntry *)0)->file)) {
        if (g_sprlib_count == cap) {
          cap = cap ? cap * 2 : 64;
          SpriteLibEntry *tmp = (SpriteLibEntry *)realloc(g_sprlib, cap * sizeof(SpriteLibEntry));
          if (tmp == NULL)
            break;  // keep whatever was collected
          g_sprlib = tmp;
        }
        SpriteLibEntry *e = &g_sprlib[g_sprlib_count];
        memcpy(e->name, value, name_len);
        e->name[name_len] = 0;
        memcpy(e->file, bar + 1, file_len);
        e->file[file_len] = 0;
        g_sprlib_count++;
      }
    }
    line = next;
  }
  free(text);
  if (g_sprlib_count > 0) {
    fprintf(stderr, "[sprites] catalog: %d sprites (F11 = next, Shift+F11 = previous)\n",
            g_sprlib_count);
    SpritesRegisterMenuRow();  // SETTINGS row exists only with a live catalog
  } else {
    free(g_sprlib);
    g_sprlib = NULL;
  }
}

// Resolves the catalog index of the sprite named `name` (normalized compare),
// falling back to a file-basename match when `file` is given.
static void SpritesResolveCurrent(const char *name, const char *file) {
  if (g_sprlib == NULL)
    return;
  char want[64];
  NormalizeName(name, want, sizeof(want));
  if (want[0] != 0) {
    for (int i = 0; i < g_sprlib_count; i++) {
      char have[64];
      NormalizeName(g_sprlib[i].name, have, sizeof(have));
      if (strcmp(have, want) == 0) {
        g_sprlib_cur = i;
        return;
      }
    }
  }
  if (file[0] != 0) {
    char base[256], base_norm[256];
    BaseNameOf(file, base, sizeof(base));
    NormalizeName(base, base_norm, sizeof(base_norm));
    for (int i = 0; i < g_sprlib_count; i++) {
      char have[256];
      NormalizeName(g_sprlib[i].file, have, sizeof(have));
      if (strcmp(have, base_norm) == 0) {
        g_sprlib_cur = i;
        return;
      }
    }
  }
}

// Rewrites sprites.ini as `name=<picked>`, dropping any existing name=/file=
// lines (a stale file= would override the new name= at the next boot and
// defeat persistence) and preserving every other line verbatim. Written via
// sprites.ini.tmp + rename. Non-fatal: on failure the session keeps the
// sprite and stderr says so.
static void SpritesPersistIni(const char *name) {
  size_t length = 0;
  uint8 *data = ReadWholeFile("sprites.ini", &length);
  char *text = NULL;
  if (data != NULL) {
    text = (char *)malloc(length + 1);
    if (text != NULL) {
      memcpy(text, data, length);
      text[length] = 0;
    }
    free(data);
  }
  FILE *f = fopen("sprites.ini.tmp", "wb");
  if (f == NULL) {
    fprintf(stderr, "[sprites] could not persist pick (sprites.ini.tmp unwritable)\n");
    free(text);
    return;
  }
  if (text != NULL) {
    for (char *p = text; *p; ) {
      char *nl = strchr(p, '\n');
      size_t core = nl ? (size_t)(nl - p) : strlen(p);
      size_t whole = nl ? core + 1 : core;
      char tmp[600];
      char *kn, *kv;
      int is_sprite_key = 0;
      if (core < sizeof(tmp)) {
        memcpy(tmp, p, core);
        tmp[core] = 0;
        if (IniParseKeyValue(tmp, &kn, &kv) &&
            (strcmp(kn, "name") == 0 || strcmp(kn, "file") == 0))
          is_sprite_key = 1;
      }
      if (!is_sprite_key)
        fwrite(p, 1, whole, f);
      p += whole;
    }
  }
  fprintf(f, "%sname=%s\n", text != NULL ? "\n" : "", name);
  fclose(f);
  free(text);
  remove("sprites.ini");
  if (rename("sprites.ini.tmp", "sprites.ini") != 0)
    fprintf(stderr, "[sprites] could not replace sprites.ini with the new pick\n");
}

// Loads and applies one catalog entry. True only when fully applied -
// ApplyZspr validates everything before its first memcpy, so a false return
// means nothing was touched (never half-applied).
static bool SpriteTryApply(const SpriteLibEntry *e) {
  char path[600];
  snprintf(path, sizeof(path), "sprites/%s", e->file);
  size_t length = 0;
  uint8 *zspr = ReadWholeFile(path, &length);
  if (zspr == NULL) {
    fprintf(stderr, "[sprites] cycle skipping '%s' (missing: %s)\n", e->name, path);
    return false;
  }
  bool ok = ApplyZspr(zspr, length, e->name);
  free(zspr);
  if (!ok)
    return false;
  // Live palette push: re-read the (just overwritten) armor palette rows into
  // main_palette_buffer and flag the NMI CGRAM upload, exactly like the
  // engine's own area-transition reload.
  Palette_Load_LinkArmorAndGloves();
  return true;
}

void Sprite_Select_Cycle(int dir) {
  if (g_sprlib == NULL || g_sprlib_count == 0) {
    static bool complained;
    if (!complained) {
      complained = true;
      fprintf(stderr, "[sprites] cycle key pressed but no sprite catalog is loaded\n");
    }
    return;
  }
  int start = (g_sprlib_cur >= 0) ? g_sprlib_cur : (dir > 0 ? -1 : 0);
  for (int step = 1; step <= g_sprlib_count; step++) {
    int idx = start + dir * step;
    idx %= g_sprlib_count;
    if (idx < 0)
      idx += g_sprlib_count;
    if (idx == g_sprlib_cur)
      break;  // walked the whole catalog, nothing usable
    if (SpriteTryApply(&g_sprlib[idx])) {
      g_sprlib_cur = idx;
      SpritesPersistIni(g_sprlib[idx].name);
      return;
    }
  }
  fprintf(stderr, "[sprites] cycle: no usable sprite in the catalog, keeping current\n");
}

void Sprite_Select_Init(void) {
  // Feature off unless sprites.ini exists in the working directory.
  size_t ini_length = 0;
  uint8 *ini = ReadWholeFile("sprites.ini", &ini_length);
  if (ini == NULL)
    return;

  char name[64] = {0};
  char file[512] = {0};

  // Parse from a NUL-terminated copy (ReadWholeFile gives an exact-size block).
  char *text = (char *)malloc(ini_length + 1);
  if (text == NULL) {
    free(ini);
    return;
  }
  memcpy(text, ini, ini_length);
  text[ini_length] = 0;
  free(ini);
  char *buf = text;

  for (char *line = buf; *line; ) {
    char *next = strchr(line, '\n');
    if (next)
      *next++ = 0;
    char *key, *value;
    if (IniParseKeyValue(line, &key, &value)) {
      if (strcmp(key, "name") == 0) {
        snprintf(name, sizeof(name), "%s", value);
      } else if (strcmp(key, "file") == 0) {
        snprintf(file, sizeof(file), "%s", value);
      }
    }
    line = next;
  }
  free(text);

  if (name[0] == 0 && file[0] == 0) {
    fprintf(stderr, "[sprites] sprites.ini present but has no name=/file= key\n");
    SpritesLoadCatalog();  // still allow F11 cycling from vanilla
    return;
  }

  char path[600];
  if (file[0]) {
    snprintf(path, sizeof(path), "%s", file);
  } else {
    char file_url[512];
    if (!SpritesLibraryLookup("sprites/library.json", name, file_url, sizeof(file_url))) {
      SpritesLoadCatalog();  // allow F11 cycling even though this lookup failed
      return;
    }
    snprintf(path, sizeof(path), "sprites/%s", file_url);
    fprintf(stderr, "[sprites] '%s' -> %s\n", name, path);
  }

  size_t length = 0;
  uint8 *zspr = ReadWholeFile(path, &length);
  if (zspr == NULL) {
    fprintf(stderr, "[sprites] could not read %s - staying vanilla (run "
            "tools/fetch_sprites.py to download sprites)\n", path);
    SpritesLoadCatalog();
    return;
  }
  bool applied = ApplyZspr(zspr, length, file[0] ? file : name);
  free(zspr);
  // Load the catalog BEFORE resolving the applied sprite's index: the
  // resolver is a no-op while g_sprlib is NULL, which left the boot-applied
  // sprite reported as VANILLA on the settings row and the pause MODS page.
  SpritesLoadCatalog();
  if (applied)
    SpritesResolveCurrent(file[0] ? "" : name, file);
}

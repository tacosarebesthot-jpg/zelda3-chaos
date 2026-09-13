// Generic in-game SETTINGS MENU - see settings_menu.h for the adopter API and
// the keyboard map. Rendering is the minimal pixel-buffer overlay path proven
// by Twitch_PostDraw (main.c): ARGB8888, 4 bytes per pixel, pitch in bytes.
// A 5x7 uppercase bitmap font (classic LCD glyph forms) drawn at an integer
// scale derived from the framebuffer width keeps it readable at window scales
// 1..4 without pulling in the engine's PPU tile-font machinery.

#include <string.h>

#include <SDL.h>

#include "settings_menu.h"

// ---------------------------------------------------------------- state --

static SettingsRow g_sm_rows[SETTINGS_MENU_MAX_ROWS];
static int g_sm_row_count;
static int g_sm_cursor;
static bool g_sm_open;

static void (*g_sm_close_hooks[SETTINGS_MENU_MAX_CLOSE_HOOKS])(void);
static int g_sm_close_hook_count;

int SettingsMenu_RegisterRow(const SettingsRow *row) {
  if (!row || !row->label || g_sm_row_count >= SETTINGS_MENU_MAX_ROWS)
    return -1;
  g_sm_rows[g_sm_row_count++] = *row;
  return g_sm_row_count - 1;
}

int SettingsMenu_RegisterOnClose(void (*fn)(void)) {
  if (!fn || g_sm_close_hook_count >= SETTINGS_MENU_MAX_CLOSE_HOOKS)
    return -1;
  g_sm_close_hooks[g_sm_close_hook_count++] = fn;
  return g_sm_close_hook_count - 1;
}

bool SettingsMenu_IsOpen(void) { return g_sm_open; }

void SettingsMenu_Open(void) {
  if (g_sm_row_count == 0)
    return;                              // nothing registered: not buildable
  g_sm_open = true;
  if (g_sm_cursor >= g_sm_row_count)
    g_sm_cursor = 0;
}

void SettingsMenu_Close(void) {
  if (!g_sm_open)
    return;
  g_sm_open = false;
  for (int i = 0; i < g_sm_close_hook_count; i++)
    g_sm_close_hooks[i]();
}

void SettingsMenu_Toggle(void) {
  if (g_sm_open)
    SettingsMenu_Close();
  else
    SettingsMenu_Open();
}

// ---------------------------------------------------------------- input --

void SettingsMenu_Input(int sdl_key, int sdl_mod, bool pressed) {
  if (!g_sm_open || !pressed || sdl_mod != 0)
    return;
  if (sdl_key == SDLK_UP) {
    if (g_sm_cursor > 0)
      g_sm_cursor--;
    else
      g_sm_cursor = g_sm_row_count - 1;  // wrap
  } else if (sdl_key == SDLK_DOWN) {
    if (g_sm_cursor < g_sm_row_count - 1)
      g_sm_cursor++;
    else
      g_sm_cursor = 0;
  } else if (sdl_key == SDLK_LEFT || sdl_key == SDLK_RIGHT) {
    if (g_sm_rows[g_sm_cursor].on_adjust)
      g_sm_rows[g_sm_cursor].on_adjust(sdl_key == SDLK_RIGHT ? 1 : -1);
  } else if (sdl_key == SDLK_RETURN || sdl_key == SDLK_KP_ENTER) {
    if (g_sm_rows[g_sm_cursor].on_adjust)
      g_sm_rows[g_sm_cursor].on_adjust(0);
  } else if (sdl_key == SDLK_ESCAPE) {
    SettingsMenu_Close();
  }
  // F10/F12 close via main.c's SettingsMenu_Toggle intercept (never lands here).
}

// -------------------------------------------------------------- drawing --

static const uint8 *GlyphFor(char c) {
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
    return kUpper[c - 'a'];              // menu text is uppercase anyway
  if (c >= '0' && c <= '9')
    return kDigit[c - '0'];
  switch (c) {
  case '!': {
    static const uint8 g[7] = { 0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04 };
    return g;
  }
  case '(': {
    static const uint8 g[7] = { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 };
    return g;
  }
  case ')': {
    static const uint8 g[7] = { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 };
    return g;
  }
  case '-': {
    static const uint8 g[7] = { 0, 0, 0, 0x1F, 0, 0, 0 };
    return g;
  }
  case '.': {
    static const uint8 g[7] = { 0, 0, 0, 0, 0, 0x0C, 0x0C };
    return g;
  }
  case '/': {
    static const uint8 g[7] = { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 };
    return g;
  }
  case ':': {
    static const uint8 g[7] = { 0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0 };
    return g;
  }
  case '[': {
    static const uint8 g[7] = { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E };
    return g;
  }
  case ']': {
    static const uint8 g[7] = { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E };
    return g;
  }
  case '_': {
    static const uint8 g[7] = { 0, 0, 0, 0, 0, 0, 0x1F };
    return g;
  }
  default:
    return kSpace;
  }
}

// 5x7 glyph scaled x`s`, clipped to the framebuffer. bit 0 = leftmost column.
static void DrawGlyph(uint8 *pixels, int pitch, int width, int height,
                      int x, int y, char c, uint32 color, int s) {
  const uint8 *g = GlyphFor(c);
  for (int gy = 0; gy < 7; gy++) {
    for (int yy = 0; yy < s; yy++) {
      int py = y + (gy * s) + yy;
      if (py < 0 || py >= height)
        continue;
      uint32 *row = (uint32 *)(pixels + (size_t)py * pitch);
      uint8 bits = g[gy];
      for (int gx = 0; gx < 5; gx++) {
        // glyph rows are authored MSB-first (bit4 = leftmost column), so
        // read the bit mirrored to gx - otherwise every asymmetric glyph
        // renders horizontally flipped (E looks like 3)
        if (!(bits & (1 << (4 - gx))))
          continue;
        for (int xx = 0; xx < s; xx++) {
          int px = x + (gx * s) + xx;
          if (px >= 0 && px < width)
            row[px] = color;
        }
      }
    }
  }
}

static void DrawText(uint8 *pixels, int pitch, int width, int height,
                     int x, int y, const char *text, uint32 color, int sc) {
  for (; *text; text++, x += 6 * sc)
    DrawGlyph(pixels, pitch, width, height, x, y, *text, color, sc);
}

void SettingsMenu_PostDraw(uint8 *pixels, int pitch, int width, int height) {
  if (!g_sm_open || !pixels || width < 128 || height < 96)
    return;

  const int s = width / 512 < 1 ? 1 : width / 512;

  // dim the frame behind the menu (cheap 50% darken, alpha forced opaque)
  for (int y = 0; y < height; y++) {
    uint32 *row = (uint32 *)(pixels + (size_t)y * pitch);
    for (int x = 0; x < width; x++) {
      uint32 p = row[x];
      row[x] = 0xFF000000u | ((p >> 1) & 0x7F7F7Fu);
    }
  }

  // layout: title bar + one row per entry + footer
  const int char_w = 6 * s, row_h = 10 * s;
  const int title_lines = 1, footer_lines = 2;
  int max_cols = 12;                                   // strlen("SETTINGS")+
  for (int i = 0; i < g_sm_row_count; i++) {
    int n = (int)strlen(g_sm_rows[i].label) + 4;       // cursor + gap
    if (g_sm_rows[i].value_text) {
      const char *v = g_sm_rows[i].value_text();
      if (v)
        n += (int)strlen(v) + 2;
    }
    if (n > max_cols)
      max_cols = n;
  }
  // size to the screen: never wider than the framebuffer allows
  {
    int max_fit = width / (6 * s) - 4;
    if (max_cols > max_fit)
      max_cols = max_fit;
    if (max_cols < 12)
      max_cols = 12;
  }

  const int bw = (8 + max_cols * 6) * s;
  const int bh = (10 + (title_lines + g_sm_row_count + footer_lines) * row_h);
  const int bx = (width - bw) / 2;
  const int by = (height - bh) / 2 < 0 ? 0 : (height - bh) / 2;

  // panel + border
  for (int y = by; y < by + bh && y < height; y++) {
    if (y < 0)
      continue;
    uint32 *row = (uint32 *)(pixels + (size_t)y * pitch);
    for (int x = bx; x < bx + bw && x < width; x++) {
      if (x < 0)
        continue;
      bool border = (x == bx || x == bx + bw - 1 || y == by || y == by + bh - 1);
      row[x] = border ? 0xFF8088B8u : 0xF0141828u;
    }
  }

  const uint32 kWhite = 0xFFFFFFFFu, kGrey = 0xFF9098A8u,
               kGreen = 0xFF70E898u, kYellow = 0xFFE8D060u;
  int tx = bx + 6 * s;
  int ty = by + 5 * s;

  DrawText(pixels, pitch, width, height, tx, ty, "SETTINGS", kYellow, s);
  ty += row_h;

  for (int i = 0; i < g_sm_row_count; i++) {
    uint32 color = (i == g_sm_cursor) ? kWhite : kGrey;
    if (i == g_sm_cursor)
      DrawText(pixels, pitch, width, height, tx, ty, ">", kGreen, s);
    DrawText(pixels, pitch, width, height, tx + 2 * char_w, ty,
             g_sm_rows[i].label, color, s);
    if (g_sm_rows[i].value_text) {
      const char *v = g_sm_rows[i].value_text();
      if (v && v[0]) {
        int vw = (int)strlen(v) * char_w;
        DrawText(pixels, pitch, width, height,
                 bx + bw - 6 * s - vw, ty, v,
                 (i == g_sm_cursor) ? kGreen : kGrey, s);
      }
    }
    ty += row_h;
  }

  ty += 2 * s;
  DrawText(pixels, pitch, width, height, tx, ty,
           "[UP/DN] MOVE [LT/RT] ADJUST [RET] OK", kGrey, s);
  ty += row_h;
  DrawText(pixels, pitch, width, height, tx, ty,
           "[F10] CLOSE (SAVES ON CLOSE)", kGrey, s);
}

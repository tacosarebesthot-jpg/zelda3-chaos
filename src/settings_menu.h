#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

#include "types.h"

// Generic in-game SETTINGS MENU - a modal, full-screen menu substate drawn on
// the game's pixel buffer (same overlay path as Twitch_PostDraw, main.c
// DrawPpuFrameWithPerf). NOT a hotkey-only overlay: while open it consumes
// all keyboard input (main.c routes every key here first, the engine sees
// none), shows a cursor list and is reachable via F10 (or F12) on ANY screen,
// title/file-select included. Extensible by registration - adopters (Twitch
// OAuth, randomizer settings, sprite selection) NEVER edit this file:
//
//   1. keep a static SettingsRow per entry and register it once at init:
//        static const SettingsRow kRandoEnabled = {
//            "RANDOMIZER ENABLED", RandoRowValue, RandoRowAdjust };
//        SettingsMenu_RegisterRow(&kRandoEnabled);
//   2. value_text()  -> right-column string, called every frame while open
//      ("ON"/"OFF"/seed number). Return NULL for a blank value (action rows).
//   3. on_adjust(delta) -> LEFT = -1, RIGHT = +1, RETURN = 0 (use 0 as the
//      on-toggle / activate callback). Mutates LIVE state.
//   4. save semantics = COMMIT ON CLOSE: register
//        SettingsMenu_RegisterOnClose(&YourSaveFn);
//      and it fires (once per close, main thread) whenever the menu closes.
//
// Keyboard (all owned here): UP/DOWN move the cursor, LEFT/RIGHT adjust the
// selected row, RETURN activates it, F10/F12/ESC close. F10/F12 toggling is
// wired in main.c HandleInput (both call SettingsMenu_Toggle).

#define SETTINGS_MENU_MAX_ROWS 16
#define SETTINGS_MENU_MAX_CLOSE_HOOKS 4

typedef struct SettingsRow {
  const char *label;                    // left column, drawn verbatim
  const char *(*value_text)(void);      // right column, or NULL for blank
  void (*on_adjust)(int delta);         // -1 LEFT, +1 RIGHT, 0 RETURN
} SettingsRow;

// Register one row (call from your module's init, before first open).
// Returns the row index, or -1 when the table is full.
int SettingsMenu_RegisterRow(const SettingsRow *row);

// Register a commit hook - fires on every menu close (main thread).
// Returns the hook index, or -1 when the table is full.
int SettingsMenu_RegisterOnClose(void (*fn)(void));

void SettingsMenu_Toggle(void);
void SettingsMenu_Open(void);
void SettingsMenu_Close(void);
bool SettingsMenu_IsOpen(void);

// Modal input feed - main.c calls this for every key event while open
// (and nothing else reaches the engine while it returns).
void SettingsMenu_Input(int sdl_key, int sdl_mod, bool pressed);

// Pixel-buffer rendering (no-op while closed). Call after Twitch_PostDraw.
void SettingsMenu_PostDraw(uint8 *pixels, int pitch, int width, int height);

#endif

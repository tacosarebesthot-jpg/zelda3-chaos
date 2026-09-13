// Player controls (stream build): the OPTIONS entry on the player select
// opens a page that defines the pad buttons and the keyboard keys for the 12
// SNES inputs, tests them with live feedback, and saves controls.ini next to
// the exe. controls.ini overrides the Controls lines of zelda3.ini at boot;
// deleting it (or RESET on the page) goes back to zelda3.ini.
#pragma once
#include "types.h"

enum {
  kCtl_Up, kCtl_Down, kCtl_Left, kCtl_Right, kCtl_Select, kCtl_Start,
  kCtl_A, kCtl_B, kCtl_X, kCtl_Y, kCtl_L, kCtl_R, kCtl_Count,
};
enum { kCtlKind_Pad = 0, kCtlKind_Key = 1, kCtlKind_Test = 2 };
enum { kCtl_CaptureNone = -1, kCtl_CaptureCancel = -2 };

extern const char *const kControls_Names[kCtl_Count];   // UP, DOWN, ... (caps)

void Controls_Load(void);            // boot, after ParseConfigFile
bool Controls_Save(void);            // writes controls.ini and applies
void Controls_ResetDefaults(void);   // deletes controls.ini, back to zelda3.ini
bool Controls_HasIni(void);

// Bindings (live). Labels are A-Z and space only, 6 chars max, for the
// file-select font.
int  Controls_GetPad(int ctl);       // kGamepadBtn_* or -1
int  Controls_GetKey(int ctl);       // SDL_Keycode or 0
void Controls_SetPad(int ctl, int gamepad_btn);   // clears a duplicate elsewhere
void Controls_SetKey(int ctl, int keycode);
void Controls_Snapshot(void);        // remember the live map (before a define)
void Controls_Restore(void);         // undo to the snapshot (cancelled define)
const char *Controls_PadLabel(int gamepad_btn);
const char *Controls_KeyLabel(int keycode);

// Raw-event capture while a define page (or the test page) is open. main.c
// feeds every key/pad press here first; a consumed press never reaches the
// game. kCtlKind_Test consumes only Escape.
void Controls_CaptureBegin(int kind);
void Controls_CaptureEnd(void);
bool Controls_Capturing(void);
bool Controls_CaptureKey(int keycode);        // returns true when consumed
bool Controls_CapturePad(int gamepad_btn);
int  Controls_TakeCaptured(void);             // code, kCtl_CaptureNone, or kCtl_CaptureCancel

// The merged input word fed to the emulator this frame (main.c), so the test
// page shows exactly what the game sees.
void Controls_NoteInputs(int inputs);
bool Controls_InputHeld(int ctl);

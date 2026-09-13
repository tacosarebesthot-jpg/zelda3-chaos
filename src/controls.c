#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
#include "controls.h"
#include "config.h"

const char *const kControls_Names[kCtl_Count] = {
  "UP", "DOWN", "LEFT", "RIGHT", "SELECT", "START", "A", "B", "X", "Y", "L", "R",
};
// SNES joypad bit for each control, as main.c packs the input word
static const uint8 kCtl_InputBit[kCtl_Count] = { 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };

// config.c vocabulary for the pad buttons, indexed by kGamepadBtn_*
static const char *const kPadIniNames[kGamepadBtn_Count] = {
  "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "L1", "R1",
  "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "L2", "R2",
};
static const char *const kPadLabels[kGamepadBtn_Count] = {
  "A", "B", "X", "Y", "BACK", "GUIDE", "START", "LSTICK", "RSTICK", "LB", "RB",
  "DUP", "DDOWN", "DLEFT", "DRIGHT", "LT", "RT",
};

static int g_ctl_pad[kCtl_Count], g_ctl_key[kCtl_Count];
static int g_ctl_pad_snap[kCtl_Count], g_ctl_key_snap[kCtl_Count];
static bool g_ctl_has_ini;
static int g_ctl_capture_kind = -1, g_ctl_captured = kCtl_CaptureNone;
static int g_ctl_inputs;

static int PadFromIniName(const char *s) {
  for (int i = 0; i < kGamepadBtn_Count; i++)
    if (!strcasecmp(s, kPadIniNames[i])) return i;
  if (!strcasecmp(s, "Lb")) return kGamepadBtn_L1;
  if (!strcasecmp(s, "Rb")) return kGamepadBtn_R1;
  return -1;
}

// comma list -> 12 trimmed fields; missing fields stay empty
static void SplitCsv(const char *csv, char out[kCtl_Count][32]) {
  memset(out, 0, kCtl_Count * 32);
  int i = 0;
  const char *p = csv ? csv : "";
  while (i < kCtl_Count) {
    while (*p == ' ' || *p == '\t') p++;
    const char *e = p;
    while (*e && *e != ',' && *e != '\r' && *e != '\n') e++;
    size_t n = e - p;
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n >= 31) n = 31;
    memcpy(out[i], p, n);
    out[i][n] = 0;
    i++;
    if (*e != ',') break;
    p = e + 1;
  }
}

static void ApplyCsv(const char *keys, const char *pad) {
  char f[kCtl_Count][32];
  if (keys) {
    SplitCsv(keys, f);
    for (int i = 0; i < kCtl_Count; i++)
      g_ctl_key[i] = f[i][0] ? (int)SDL_GetKeyFromName(f[i]) : 0;
  }
  if (pad) {
    SplitCsv(pad, f);
    for (int i = 0; i < kCtl_Count; i++)
      g_ctl_pad[i] = f[i][0] ? PadFromIniName(f[i]) : -1;
  }
}

static void BuildCsv(char *keys, size_t kcap, char *pad, size_t pcap) {
  size_t kn = 0, pn = 0;
  for (int i = 0; i < kCtl_Count; i++) {
    const char *k = g_ctl_key[i] ? SDL_GetKeyName(g_ctl_key[i]) : "";
    const char *p = g_ctl_pad[i] >= 0 ? kPadIniNames[g_ctl_pad[i]] : "";
    kn += (size_t)snprintf(keys + kn, kcap > kn ? kcap - kn : 0, "%s%s", i ? "," : "", k);
    pn += (size_t)snprintf(pad + pn, pcap > pn ? pcap - pn : 0, "%s%s", i ? "," : "", p);
  }
}

// Push the live map into config.c's lookup tables (replaces the Controls
// bindings only; every other hotkey in zelda3.ini is untouched).
static void ApplyLive(void) {
  char keys[512], pad[512];
  BuildCsv(keys, sizeof keys, pad, sizeof pad);
  Config_ReplaceControls(keys, pad);
}

void Controls_Load(void) {
  ApplyCsv(Config_ControlsLine(kCtlKind_Key), Config_ControlsLine(kCtlKind_Pad));
  g_ctl_has_ini = false;
  FILE *f = fopen("controls.ini", "rb");
  if (!f) return;
  char line[600], keys[600] = {0}, pad[600] = {0};
  bool any = false;
  while (fgets(line, sizeof line, f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == ';' || *s == '\r' || *s == '\n' || !*s) continue;
    char *nl = strpbrk(s, "\r\n");
    if (nl) *nl = 0;
    if (!strncasecmp(s, "keys=", 5)) { snprintf(keys, sizeof keys, "%s", s + 5); any = true; }
    else if (!strncasecmp(s, "pad=", 4)) { snprintf(pad, sizeof pad, "%s", s + 4); any = true; }
  }
  fclose(f);
  if (!any) return;
  g_ctl_has_ini = true;
  ApplyCsv(keys[0] ? keys : NULL, pad[0] ? pad : NULL);
  ApplyLive();
  printf("[controls] controls.ini applied (keys=%s pad=%s)\n", keys, pad);
}

bool Controls_Save(void) {
  char keys[512], pad[512];
  BuildCsv(keys, sizeof keys, pad, sizeof pad);
  FILE *f = fopen("controls.ini.tmp", "wb");
  if (!f) return false;
  fprintf(f, "# Written by the game: OPTIONS on the player select. Delete this file to go\n"
             "# back to the Controls lines of zelda3.ini.\n"
             "# Order: Up, Down, Left, Right, Select, Start, A, B, X, Y, L, R\n"
             "keys=%s\npad=%s\n", keys, pad);
  bool ok = fclose(f) == 0;
#ifdef _WIN32
  if (ok) ok = MoveFileExA("controls.ini.tmp", "controls.ini", MOVEFILE_REPLACE_EXISTING) != 0;
#else
  if (ok) ok = rename("controls.ini.tmp", "controls.ini") == 0;
#endif
  if (!ok) { remove("controls.ini.tmp"); return false; }
  g_ctl_has_ini = true;
  ApplyLive();
  printf("[controls] saved controls.ini (keys=%s pad=%s)\n", keys, pad);
  return true;
}

void Controls_ResetDefaults(void) {
  remove("controls.ini");
  g_ctl_has_ini = false;
  ApplyCsv(Config_ControlsLine(kCtlKind_Key), Config_ControlsLine(kCtlKind_Pad));
  ApplyLive();
  printf("[controls] controls.ini removed, zelda3.ini controls restored\n");
}

bool Controls_HasIni(void) { return g_ctl_has_ini; }
int Controls_GetPad(int ctl) { return (unsigned)ctl < kCtl_Count ? g_ctl_pad[ctl] : -1; }
int Controls_GetKey(int ctl) { return (unsigned)ctl < kCtl_Count ? g_ctl_key[ctl] : 0; }

void Controls_SetPad(int ctl, int gamepad_btn) {
  if ((unsigned)ctl >= kCtl_Count) return;
  for (int i = 0; i < kCtl_Count; i++)
    if (i != ctl && g_ctl_pad[i] == gamepad_btn) g_ctl_pad[i] = -1;   // one button, one job
  g_ctl_pad[ctl] = gamepad_btn;
}
void Controls_SetKey(int ctl, int keycode) {
  if ((unsigned)ctl >= kCtl_Count) return;
  for (int i = 0; i < kCtl_Count; i++)
    if (i != ctl && g_ctl_key[i] == keycode) g_ctl_key[i] = 0;
  g_ctl_key[ctl] = keycode;
}
void Controls_Snapshot(void) {
  memcpy(g_ctl_pad_snap, g_ctl_pad, sizeof g_ctl_pad);
  memcpy(g_ctl_key_snap, g_ctl_key, sizeof g_ctl_key);
}
void Controls_Restore(void) {
  memcpy(g_ctl_pad, g_ctl_pad_snap, sizeof g_ctl_pad);
  memcpy(g_ctl_key, g_ctl_key_snap, sizeof g_ctl_key);
}

const char *Controls_PadLabel(int gamepad_btn) {
  return (unsigned)gamepad_btn < kGamepadBtn_Count ? kPadLabels[gamepad_btn] : "NONE";
}

// Key labels for the save-name font (A-Z and space only, 6 chars max).
const char *Controls_KeyLabel(int keycode) {
  static char buf[8];
  if (keycode == 0) return "NONE";
  switch (keycode) {
  case SDLK_RETURN: return "ENTER";
  case SDLK_KP_ENTER: return "KPENT";
  case SDLK_SPACE: return "SPACE";
  case SDLK_TAB: return "TAB";
  case SDLK_ESCAPE: return "ESC";
  case SDLK_BACKSPACE: return "BKSP";
  case SDLK_LSHIFT: return "LSHIFT";
  case SDLK_RSHIFT: return "RSHIFT";
  case SDLK_LCTRL: return "LCTRL";
  case SDLK_RCTRL: return "RCTRL";
  case SDLK_LALT: return "LALT";
  case SDLK_RALT: return "RALT";
  case SDLK_UP: return "UP";
  case SDLK_DOWN: return "DOWN";
  case SDLK_LEFT: return "LEFT";
  case SDLK_RIGHT: return "RIGHT";
  case SDLK_SEMICOLON: return "SEMI";
  case SDLK_COMMA: return "COMMA";
  case SDLK_PERIOD: return "DOT";
  case SDLK_SLASH: return "SLASH";
  case SDLK_BACKSLASH: return "BSLASH";
  case SDLK_QUOTE: return "QUOTE";
  case SDLK_LEFTBRACKET: return "LBRKT";
  case SDLK_RIGHTBRACKET: return "RBRKT";
  case SDLK_MINUS: return "MINUS";
  case SDLK_EQUALS: return "EQUAL";
  case SDLK_BACKQUOTE: return "TILDE";
  case SDLK_INSERT: return "INS";
  case SDLK_DELETE: return "DEL";
  case SDLK_HOME: return "HOME";
  case SDLK_END: return "END";
  case SDLK_PAGEUP: return "PGUP";
  case SDLK_PAGEDOWN: return "PGDN";
  case SDLK_CAPSLOCK: return "CAPS";
  case SDLK_KP_0: case SDLK_KP_1: case SDLK_KP_2: case SDLK_KP_3: case SDLK_KP_4:
  case SDLK_KP_5: case SDLK_KP_6: case SDLK_KP_7: case SDLK_KP_8: case SDLK_KP_9:
    return "KEYPAD";
  case SDLK_KP_PLUS: return "KPPLUS";
  case SDLK_KP_MINUS: return "KPMIN";
  case SDLK_KP_MULTIPLY: return "KPMUL";
  case SDLK_KP_DIVIDE: return "KPDIV";
  case SDLK_KP_PERIOD: return "KPDOT";
  default: break;
  }
  if (keycode >= '0' && keycode <= '9') {
    static const char *const kDigits[10] = {"ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE"};
    return kDigits[keycode - '0'];
  }
  if (keycode >= SDLK_F1 && keycode <= SDLK_F12) {
    static const char *const kF[12] = {"F ONE", "F TWO", "F THREE", "F FOUR", "F FIVE", "F SIX", "F SEVEN", "F EIGHT", "F NINE", "F TEN", "F ELEV", "F TWELV"};
    return kF[keycode - SDLK_F1];
  }
  const char *n = SDL_GetKeyName(keycode);
  int o = 0;
  for (; *n && o < 6; n++) {
    char c = *n;
    if (c >= 'a' && c <= 'z') c -= 32;
    if ((c >= 'A' && c <= 'Z') || c == ' ') buf[o++] = c;
  }
  buf[o] = 0;
  return o ? buf : "KEY";
}

void Controls_CaptureBegin(int kind) { g_ctl_capture_kind = kind; g_ctl_captured = kCtl_CaptureNone; }
void Controls_CaptureEnd(void) { g_ctl_capture_kind = -1; g_ctl_captured = kCtl_CaptureNone; }
bool Controls_Capturing(void) { return g_ctl_capture_kind >= 0; }

bool Controls_CaptureKey(int keycode) {
  if (g_ctl_capture_kind < 0) return false;
  if (keycode == SDLK_ESCAPE) { g_ctl_captured = kCtl_CaptureCancel; return true; }
  if (g_ctl_capture_kind == kCtlKind_Test) return false;   // the test page sees real input
  if (g_ctl_capture_kind == kCtlKind_Key && g_ctl_captured == kCtl_CaptureNone)
    g_ctl_captured = keycode;
  return true;                                             // a define page swallows every press
}
bool Controls_CapturePad(int gamepad_btn) {
  if (g_ctl_capture_kind < 0 || g_ctl_capture_kind == kCtlKind_Test) return false;
  if (g_ctl_capture_kind == kCtlKind_Pad && g_ctl_captured == kCtl_CaptureNone &&
      gamepad_btn != kGamepadBtn_Guide)
    g_ctl_captured = gamepad_btn;
  return true;
}
int Controls_TakeCaptured(void) {
  int c = g_ctl_captured;
  g_ctl_captured = kCtl_CaptureNone;
  return c;
}

void Controls_NoteInputs(int inputs) { g_ctl_inputs = inputs; }
bool Controls_InputHeld(int ctl) {
  return (unsigned)ctl < kCtl_Count && (g_ctl_inputs >> kCtl_InputBit[ctl] & 1);
}

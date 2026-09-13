#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "options.h"
#include "config.h"
#include "features.h"
#include "dialogue_override.h"
#include "music_player.h"

static const char *const kOnOff[] = { "OFF", "ON" };
static const char *const kScales[] = { "ONE X", "TWO X", "THREE X", "FOUR X", "FIVE X", "SIX X" };
static const char *const kOutputs[] = { "SDL", "SOFTWARE", "OPENGL", "GL ES" };
static const char *const kBuffers[] = { "TINY", "SMALL", "MEDIUM", "LARGE" };
static const char *const kHintModes[] = { "NORMAL", "REAL", "JOKES" };
static const char *const kCreditModes[] = { "STAFF", "CHAT" };
static const char *const kMsuModes[] = { "OFF", "ON", "DELUXE", "OPUZ", "DLX OPUZ" };
static const char *const kMsuIni[] = { "false", "true", "deluxe", "opuz", "deluxe-opuz" };
static const char *const kVolumes[] = { "ZERO", "LOW", "HALF", "HIGH", "FULL" };
static const char *const kSkipRow[] = { "PRESS LR" };
static const uint16 kBufferSamples[] = { 512, 1024, 2048, 4096 };

// ---- video ---------------------------------------------------------------
static int GetWide(void) { return g_config.extended_aspect_ratio != 0; }
static void SetWide(int v) {
  // the same numbers config.c derives from "16:9" / "4:3" (with the 240-line screen when TALL SCREEN is on)
  int h = g_config.extend_y ? 240 : 224;
  g_config.extended_aspect_ratio = v ? (uint8)((h * 16 / 9 - 256) / 2) : 0;
  uint32 bits = kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes;
  g_config.features0 = (g_config.features0 & ~bits) | (v ? bits : 0);
}
static int GetFull(void) { return g_config.fullscreen != 0; }
static void SetFull(int v) { g_config.fullscreen = (uint8)(v ? 1 : 0); }
static int GetScale(void) {
  int s = g_config.window_scale == 0 ? 2 : g_config.window_scale;
  return (s < 1 ? 1 : s > 6 ? 6 : s) - 1;
}
static void SetScale(int v) { g_config.window_scale = (uint8)(v + 1); }
static int GetNewRend(void) { return g_config.new_renderer; }
static void SetNewRend(int v) { g_config.new_renderer = v != 0; }
static int GetMode7(void) { return g_config.enhanced_mode7; }
static void SetMode7(int v) { g_config.enhanced_mode7 = v != 0; }
static int GetSprLimit(void) { return !g_config.no_sprite_limits; }       // ON = the real SNES limit
static void SetSprLimit(int v) { g_config.no_sprite_limits = v == 0; }
static int GetSmooth(void) { return g_config.linear_filtering; }
static void SetSmooth(int v) { g_config.linear_filtering = v != 0; }
static int GetOutput(void) {
  switch (g_config.output_method) {
  case kOutputMethod_SDLSoftware: return 1;
  case kOutputMethod_OpenGL: return 2;
  case kOutputMethod_OpenGL_ES: return 3;
  default: return 0;
  }
}
static void SetOutput(int v) {
  static const uint8 k[] = { kOutputMethod_SDL, kOutputMethod_SDLSoftware, kOutputMethod_OpenGL, kOutputMethod_OpenGL_ES };
  g_config.output_method = k[v & 3];
}
static int GetCheckpoint(void) { return g_config.crash_checkpoint; }
static void SetCheckpoint(int v) { g_config.crash_checkpoint = v != 0; }
static int GetStretch(void) { return g_config.ignore_aspect_ratio; }
static void SetStretch(int v) { g_config.ignore_aspect_ratio = v != 0; }
static int GetTall(void) { return g_config.extend_y; }
static void SetTall(int v) { g_config.extend_y = v != 0; SetWide(GetWide()); }
static int GetPerfTitle(void) { return g_config.display_perf_title; }
static void SetPerfTitle(int v) { g_config.display_perf_title = v != 0; }
static int GetAutosave(void) { return g_config.autosave; }
static void SetAutosave(int v) { g_config.autosave = v != 0; }
static int GetMsu(void) {
  int e = g_config.enable_msu;
  if (!e) return 0;
  if ((e & kMsuEnabled_MsuDeluxe) && (e & kMsuEnabled_Opuz)) return 4;
  if (e & kMsuEnabled_Opuz) return 3;
  if (e & kMsuEnabled_MsuDeluxe) return 2;
  return 1;
}
static void SetMsu(int v) {
  static const uint8 k[] = { 0, kMsuEnabled_Msu, kMsuEnabled_MsuDeluxe, kMsuEnabled_Opuz, kMsuEnabled_MsuDeluxe | kMsuEnabled_Opuz };
  g_config.enable_msu = k[v % 5];
}
static int GetMsuResume(void) { return g_config.resume_msu; }
static void SetMsuResume(int v) { g_config.resume_msu = v != 0; }
static int GetMsuVolume(void) { int v = g_config.msuvolume; return v <= 0 ? 0 : v <= 25 ? 1 : v <= 50 ? 2 : v <= 75 ? 3 : 4; }
static void SetMsuVolume(int v) { static const uint8 k[] = { 0, 25, 50, 75, 100 }; g_config.msuvolume = k[v % 5]; }
static int GetFrameDelay(void) { return !g_config.disable_frame_delay; }
static void SetFrameDelay(int v) { g_config.disable_frame_delay = v == 0; }

// ---- feature bits --------------------------------------------------------
#define BIT_ITEM(name, mask) \
  static int Get##name(void) { return (g_config.features0 & (mask)) != 0; } \
  static void Set##name(int v) { g_config.features0 = (g_config.features0 & ~(uint32)(mask)) | (v ? (mask) : 0); }
BIT_ITEM(DimFlashes, kFeatures0_DimFlashes)
BIT_ITEM(SwitchLR, kFeatures0_SwitchLR)
BIT_ITEM(SwitchLRLimit, kFeatures0_SwitchLRLimit)
BIT_ITEM(TurnDash, kFeatures0_TurnWhileDashing)
BIT_ITEM(MirrorDW, kFeatures0_MirrorToDarkworld)
BIT_ITEM(SwordCollect, kFeatures0_CollectItemsWithSword)
BIT_ITEM(SwordPots, kFeatures0_BreakPotsWithSword)
BIT_ITEM(SkipIntro, kFeatures0_SkipIntroOnKeypress)
BIT_ITEM(YellowMax, kFeatures0_ShowMaxItemsInYellow)
BIT_ITEM(FourBombs, kFeatures0_MoreActiveBombs)
BIT_ITEM(MoreRupees, kFeatures0_CarryMoreRupees)
BIT_ITEM(MiscFixes, kFeatures0_MiscBugFixes)
BIT_ITEM(BigFixes, kFeatures0_GameChangingBugFixes)
BIT_ITEM(CancelBird, kFeatures0_CancelBirdTravel)
#undef BIT_ITEM
static int GetBeep(void) { return !(g_config.features0 & kFeatures0_DisableLowHealthBeep); }   // ON = beeps
static void SetBeep(int v) {
  g_config.features0 = (g_config.features0 & ~(uint32)kFeatures0_DisableLowHealthBeep) | (v ? 0 : kFeatures0_DisableLowHealthBeep);
}

// ---- audio ---------------------------------------------------------------
static int GetAudio(void) { return g_config.enable_audio; }
static void SetAudio(int v) { g_config.enable_audio = v != 0; }
static int GetStereo(void) { return g_config.audio_channels >= 2; }
static void SetStereo(int v) { g_config.audio_channels = v ? 2 : 1; }
static int GetBuffer(void) {
  for (int i = 3; i > 0; i--)
    if (g_config.audio_samples >= kBufferSamples[i]) return i;
  return 0;
}
static void SetBuffer(int v) { g_config.audio_samples = kBufferSamples[v & 3]; }

// ---- music folder (src/music_player.c, config in music.ini) --------------
// Enabled / volume / shuffle all apply live; the folder is SCANNED once at
// boot, so new files still need a restart (said so in the row descriptions).
static int GetFolderMusic(void) { return MusicPlayer_GetEnabled(); }
static void SetFolderMusic(int v) { MusicPlayer_SetEnabled(v); }
static int GetFolderVol(void) {
  int v = MusicPlayer_GetVolume();
  return v <= 0 ? 0 : v <= 32 ? 1 : v <= 64 ? 2 : v <= 96 ? 3 : 4;
}
static void SetFolderVol(int v) { static const int k[] = { 0, 32, 64, 96, 128 }; MusicPlayer_SetVolume(k[v % 5]); }
static int GetFolderShuffle(void) { return MusicPlayer_GetShuffle(); }
static void SetFolderShuffle(int v) { MusicPlayer_SetShuffle(v); }
// Action row: one value, so Options_Cycle's (get()+delta) % 1 is always 0 and
// LEFT/RIGHT just fire the setter. Cheapest possible "button" in this model.
static int GetSkipTrack(void) { return 0; }
static void SetSkipTrack(int v) { (void)v; MusicPlayer_Skip(); }

static const OptItem kVideo[] = {
  { "WIDESCREEN",   "SHOWS MORE OF THE",     "WORLD LEFT AND RIGHT",  kOnOff, 2, GetWide, SetWide, false },
  { "FULLSCREEN",   "FILLS THE WHOLE",       "SCREEN",                kOnOff, 2, GetFull, SetFull, false },
  { "WINDOW SIZE",  "HOW BIG THE WINDOW",    "IS WHEN NOT FULLSCREEN", kScales, 6, GetScale, SetScale, false },
  { "NEW RENDERER", "FASTER DRAWING  OFF",   "IF SOMETHING LOOKS OFF", kOnOff, 2, GetNewRend, SetNewRend, false },
  { "MODE SEVEN",   "SHARPER WORLD MAP",     "AND MODE SEVEN SCENES", kOnOff, 2, GetMode7, SetMode7, false },
  { "SPRITE LIMIT", "ON IS THE REAL SNES",   "FLICKER  OFF SHOWS ALL", kOnOff, 2, GetSprLimit, SetSprLimit, false },
  { "SMOOTHING",    "BLURS THE PIXELS",      "OFF IS CRISP",          kOnOff, 2, GetSmooth, SetSmooth, false },
  { "OUTPUT",       "HOW THE PICTURE IS",    "DRAWN  NEEDS A RESTART", kOutputs, 4, GetOutput, SetOutput, true },
  { "DIM FLASHES",  "SOFTENS BRIGHT",        "FLASHING EFFECTS",      kOnOff, 2, GetDimFlashes, SetDimFlashes, false },
  { "FRAME DELAY",  "OFF SKIPS THE WAIT",    "BETWEEN FRAMES",        kOnOff, 2, GetFrameDelay, SetFrameDelay, false },
  { "STRETCH",      "FILLS THE WINDOW AND",  "IGNORES THE SHAPE",     kOnOff, 2, GetStretch, SetStretch, false },
  { "TALL SCREEN",  "SHOWS THE HIDDEN ROWS", "TOP AND BOTTOM",        kOnOff, 2, GetTall, SetTall, false },
  { "FPS IN TITLE", "FRAME RATE IN THE",     "WINDOW TITLE BAR",      kOnOff, 2, GetPerfTitle, SetPerfTitle, false },
};
static int GetHints(void) { return DialogueOverride_HintsMode(); }
static void SetHints(int v) { DialogueOverride_SetHintsMode(v); }
static int GetGameJokes(void) { return DialogueOverride_GameJokes(); }
static void SetGameJokes(int v) { DialogueOverride_SetGameJokes(v); }
static int GetCredits(void) { return DialogueOverride_CreditsMode(); }
static void SetCredits(int v) { DialogueOverride_SetCreditsMode(v); }

static const OptItem kFeatures[] = {
  { "HINTS",        "TILES AND THE FORTUNE", "TELLER TELL THE TRUTH", kHintModes, 3, GetHints, SetHints, false },
  { "CREDITS",      "PORT INFO PATRONS AND", "TOP CHATTERS AT END",   kCreditModes, 2, GetCredits, SetCredits, false },
  { "GAME JOKES",   "RL EVERQUEST AND UO", "LINES ON OR OFF", kOnOff, 2, GetGameJokes, SetGameJokes, false },
  { "CHECKPOINT",   "SAVES EVERY MINUTE  A",  "CRASH RESTORES IT",     kOnOff, 2, GetCheckpoint, SetCheckpoint, false },
  { "AUTOSAVE",     "SAVES A STATE ON EXIT", "AND LOADS IT ON BOOT",  kOnOff, 2, GetAutosave, SetAutosave, false },
  { "ITEM SWITCH",  "L AND R CYCLE ITEMS",   "Y PLUS DIR REORDERS",   kOnOff, 2, GetSwitchLR, SetSwitchLR, false },
  { "SWITCH LIMIT", "ITEM SWITCH ONLY",      "USES THE FIRST FOUR",   kOnOff, 2, GetSwitchLRLimit, SetSwitchLRLimit, false },
  { "TURN IN DASH", "TURN WHILE",            "DASHING",               kOnOff, 2, GetTurnDash, SetTurnDash, false },
  { "MIRROR TO DW", "THE MIRROR CAN WARP",   "TO THE DARK WORLD",     kOnOff, 2, GetMirrorDW, SetMirrorDW, false },
  { "SWORD PICKUP", "THE SWORD COLLECTS",    "HEARTS AND DROPS",      kOnOff, 2, GetSwordCollect, SetSwordCollect, false },
  { "SWORD POTS",   "LEVEL TWO SWORD",       "AND UP BREAKS POTS",    kOnOff, 2, GetSwordPots, SetSwordPots, false },
  { "LOW HP BEEP",  "THE LOW HEALTH",        "BEEP",                  kOnOff, 2, GetBeep, SetBeep, false },
  { "SKIP INTRO",   "ANY BUTTON SKIPS",      "THE OPENING",           kOnOff, 2, GetSkipIntro, SetSkipIntro, false },
  { "YELLOW MAX",   "FULL RUPEES BOMBS",     "ARROWS SHOW IN YELLOW", kOnOff, 2, GetYellowMax, SetYellowMax, false },
  { "FOUR BOMBS",   "FOUR BOMBS OUT",        "AT ONCE INSTEAD OF TWO", kOnOff, 2, GetFourBombs, SetFourBombs, false },
  { "MORE RUPEES",  "CARRY UP TO NINE",      "THOUSAND RUPEES",       kOnOff, 2, GetMoreRupees, SetMoreRupees, false },
  { "MISC FIXES",   "SMALL BUG FIXES",       "FROM THE PORT",         kOnOff, 2, GetMiscFixes, SetMiscFixes, false },
  { "BIG FIXES",    "BUG FIXES THAT",        "CHANGE HOW IT PLAYS",   kOnOff, 2, GetBigFixes, SetBigFixes, false },
  { "CANCEL BIRD",  "X CANCELS THE",         "BIRD TRAVEL",           kOnOff, 2, GetCancelBird, SetCancelBird, false },
};
static const OptItem kAudio[] = {
  { "SOUND",        "ALL GAME AUDIO",        "CHANGES AFTER RESTART", kOnOff, 2, GetAudio, SetAudio, true },
  { "STEREO",       "OFF IS MONO",           "CHANGES AFTER RESTART", kOnOff, 2, GetStereo, SetStereo, true },
  { "SOUND BUFFER", "BIGGER IF IT CRACKLES", "CHANGES AFTER RESTART", kBuffers, 4, GetBuffer, SetBuffer, true },
  { "MSU MUSIC",    "MSU PACK IN THE MSU",   "FOLDER  NEEDS RESTART", kMsuModes, 5, GetMsu, SetMsu, true },
  { "MSU RESUME",   "RETURNS TO THE SAME",   "SPOT IN THE TRACK",     kOnOff, 2, GetMsuResume, SetMsuResume, true },
  { "MSU VOLUME",   "MSU MUSIC LOUDNESS",    "CHANGES AFTER RESTART", kVolumes, 5, GetMsuVolume, SetMsuVolume, true },
  { "MUSIC FOLDER", "PLAYS YOUR OWN SONGS",  "FROM THE MUSIC FOLDER", kOnOff, 2, GetFolderMusic, SetFolderMusic, false },
  { "FOLDER VOL",   "MUSIC FOLDER LOUDNESS", "APPLIES RIGHT AWAY",    kVolumes, 5, GetFolderVol, SetFolderVol, false },
  { "SHUFFLE",      "MUSIC FOLDER PLAYS IN", "RANDOM ORDER",          kOnOff, 2, GetFolderShuffle, SetFolderShuffle, false },
  { "NEXT TRACK",   "SKIPS TO ANOTHER SONG", "NEW FILES NEED RESTART", kSkipRow, 1, GetSkipTrack, SetSkipTrack, false },
};

static const OptItem *const kPages[kOptPage_Count] = { kVideo, kFeatures, kAudio };
static const int kPageCounts[kOptPage_Count] = {
  (int)(sizeof(kVideo) / sizeof(kVideo[0])), (int)(sizeof(kFeatures) / sizeof(kFeatures[0])), (int)(sizeof(kAudio) / sizeof(kAudio[0])),
};
static const char *const kPageTitles[kOptPage_Count] = { "VIDEO", "GAME FEATURES", "AUDIO" };

int Options_Count(int page) { return (unsigned)page < kOptPage_Count ? kPageCounts[page] : 0; }
const OptItem *Options_Item(int page, int i) {
  return (unsigned)page < kOptPage_Count && (unsigned)i < (unsigned)kPageCounts[page] ? &kPages[page][i] : NULL;
}
const char *Options_PageTitle(int page) { return (unsigned)page < kOptPage_Count ? kPageTitles[page] : ""; }

void Options_Cycle(int page, int i, int delta) {
  const OptItem *it = Options_Item(page, i);
  if (!it) return;
  int v = (it->get() + delta) % it->nvals;
  if (v < 0) v += it->nvals;
  it->set(v);
  if (!it->restart) Video_Apply();
  Options_Save();
}

static const char *B(bool v) { return v ? "1" : "0"; }
static const char *Bit(uint32 mask) { return (g_config.features0 & mask) ? "1" : "0"; }

bool Options_Save(void) {
  FILE *f = fopen("options.ini.tmp", "wb");
  if (!f) return false;
  static const char *const kOut[] = { "SDL", "SDL-Software", "OpenGL", "OpenGL ES" };
  fprintf(f,
    "# Written by the game: OPTIONS on the player select. Read after zelda3.ini and\n"
    "# overrides it. Delete this file to go back to zelda3.ini.\n"
    "[General]\n"
    "ExtendedAspectRatio = %s%s\n"
    "DisableFrameDelay = %s\n"
    "CrashCheckpoint = %s\n"
    "DisplayPerfInTitle = %s\n"
    "Autosave = %s\n"
    "[Graphics]\n"
    "IgnoreAspectRatio = %s\n"
    "Fullscreen = %d\n"
    "WindowScale = %d\n"
    "NewRenderer = %s\n"
    "EnhancedMode7 = %s\n"
    "NoSpriteLimits = %s\n"
    "LinearFiltering = %s\n"
    "OutputMethod = %s\n"
    "DimFlashes = %s\n"
    "[Sound]\n"
    "EnableAudio = %s\n"
    "AudioChannels = %d\n"
    "AudioSamples = %d\n"
    "EnableMSU = %s\n"
    "ResumeMSU = %s\n"
    "MSUVolume = %d\n"
    "[Features]\n"
    "ItemSwitchLR = %s\n"
    "ItemSwitchLRLimit = %s\n"
    "TurnWhileDashing = %s\n"
    "MirrorToDarkworld = %s\n"
    "CollectItemsWithSword = %s\n"
    "BreakPotsWithSword = %s\n"
    "DisableLowHealthBeep = %s\n"
    "SkipIntroOnKeypress = %s\n"
    "ShowMaxItemsInYellow = %s\n"
    "MoreActiveBombs = %s\n"
    "CarryMoreRupees = %s\n"
    "MiscBugFixes = %s\n"
    "GameChangingBugFixes = %s\n"
    "CancelBirdTravel = %s\n",
    g_config.extend_y ? "extend_y, " : "", g_config.extended_aspect_ratio ? "16:9" : "4:3",
    B(g_config.disable_frame_delay),
    B(g_config.crash_checkpoint),
    B(g_config.display_perf_title),
    B(g_config.autosave),
    B(g_config.ignore_aspect_ratio),
    g_config.fullscreen ? 1 : 0,
    g_config.window_scale == 0 ? 2 : g_config.window_scale,
    B(g_config.new_renderer), B(g_config.enhanced_mode7), B(g_config.no_sprite_limits),
    B(g_config.linear_filtering), kOut[GetOutput()], Bit(kFeatures0_DimFlashes),
    B(g_config.enable_audio), g_config.audio_channels >= 2 ? 2 : 1, g_config.audio_samples,
    kMsuIni[GetMsu()], B(g_config.resume_msu), (int)g_config.msuvolume,
    Bit(kFeatures0_SwitchLR), Bit(kFeatures0_SwitchLRLimit), Bit(kFeatures0_TurnWhileDashing),
    Bit(kFeatures0_MirrorToDarkworld), Bit(kFeatures0_CollectItemsWithSword), Bit(kFeatures0_BreakPotsWithSword),
    Bit(kFeatures0_DisableLowHealthBeep), Bit(kFeatures0_SkipIntroOnKeypress), Bit(kFeatures0_ShowMaxItemsInYellow),
    Bit(kFeatures0_MoreActiveBombs), Bit(kFeatures0_CarryMoreRupees), Bit(kFeatures0_MiscBugFixes),
    Bit(kFeatures0_GameChangingBugFixes), Bit(kFeatures0_CancelBirdTravel));
  bool ok = fclose(f) == 0;
#ifdef _WIN32
  if (ok) ok = MoveFileExA("options.ini.tmp", "options.ini", MOVEFILE_REPLACE_EXISTING) != 0;
#else
  if (ok) ok = rename("options.ini.tmp", "options.ini") == 0;
#endif
  if (!ok) remove("options.ini.tmp");
  return ok;
}

void Options_LoadOverlay(void) {
  if (!Config_ParseExtraFile("options.ini")) return;
  // "4:3" in the overlay only zeroes the width; the widescreen feature bits
  // zelda3.ini set stay on unless cleared here
  if (g_config.extended_aspect_ratio == 0)
    g_config.features0 &= ~(uint32)(kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes);
  printf("[options] options.ini applied over zelda3.ini\n");
}

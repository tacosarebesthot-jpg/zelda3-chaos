// Port options (stream build): the VIDEO / GAME FEATURES / AUDIO pages under
// OPTIONS on the player select. Every row is one zelda3.ini setting of the
// PC port (widescreen, renderer, the [Features] toggles...). Changes apply
// live where the port allows it and are written to options.ini next to the
// exe, which is read after zelda3.ini at boot and overrides it. Delete
// options.ini to go back to zelda3.ini.
#pragma once
#include "types.h"

enum { kOptPage_Video, kOptPage_Features, kOptPage_Audio, kOptPage_Count };

typedef struct OptItem {
  const char *label;            // 12 caps max (file-select font: A-Z, space)
  const char *desc1, *desc2;    // 22 caps max each
  const char *const *names;     // value labels, 10 caps max
  int nvals;
  int (*get)(void);
  void (*set)(int v);
  bool restart;                 // takes effect on the next boot
} OptItem;

void Options_LoadOverlay(void);         // boot, right after ParseConfigFile
bool Options_Save(void);                // options.ini
int Options_Count(int page);
const OptItem *Options_Item(int page, int i);
const char *Options_PageTitle(int page);
// cycle a row's value by delta, apply live, save
void Options_Cycle(int page, int i, int delta);

// main.c: push g_config's video state to the running window / renderer / ppu
void Video_Apply(void);

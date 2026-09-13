#include "zelda_rtl.h"
#include "variables.h"
#include "load_gfx.h"
#include "select_file.h"
#include "randomizer.h"
#include "snes/snes_regs.h"
#include "overworld.h"
#include "messaging.h"
#include "sprite.h"
#include "controls.h"
#include "options.h"

static bool s_fs_moved;   // stream build: COPY/ERASE moved up for OPTIONS (FileSelect_LayoutBottom)

#define selectfile_R16 g_ram[0xc8]
#define selectfile_R17 g_ram[0xc9]
#define selectfile_R18 WORD(g_ram[0xca])
#define selectfile_R20 WORD(g_ram[0xcc])
static const uint8 kSelectFile_Draw_Y[3] = {0x43, 0x63, 0x83};
bool Intro_CheckCksum(const uint8 *s) {
  const uint16 *src = (const uint16 *)s;
  uint16 sum = 0;
  for (int i = 0; i < 0x280; i++)
    sum += src[i];
  return sum == 0x5a5a;

}

uint16 *SelectFile_Func1() {
  static const uint16 kSelectFile_Func1_Tab[4] = {0x3581, 0x3582, 0x3591, 0x3592};
  uint16 *dst = (uint16 *)&g_ram[0x1002];
  *dst++ = 0x10;
  *dst++ = 0xff07;
  for (int i = 0; i < 1024; i++)
    *dst++ = kSelectFile_Func1_Tab[((i & 0x20) >> 4) + (i & 1)];
  return dst;
}

void SelectFile_Func5_DrawOams(int k) {
  static const uint8 kSelectFile_Draw_OamIdx[3] = {0x28, 0x3c, 0x50};
  static const uint8 kSelectFile_Draw_SwordChar[4] = {0x85, 0xa1, 0xa1, 0xa1};
  static const uint8 kSelectFile_Draw_ShieldChar[3] = {0xc4, 0xca, 0xe0};
  static const uint8 kSelectFile_Draw_Flags[3] = {0x72, 0x76, 0x7a};
  static const uint8 kSelectFile_Draw_Flags2[3] = {0x32, 0x36, 0x3a};
  static const uint8 kSelectFile_Draw_Flags3[3] = {0x30, 0x34, 0x38};

  link_dma_graphics_index = 0x116 * 2;
  uint8 *sram = g_zenv.sram + 0x500 * k;

  OamEnt *oam = oam_buf + kSelectFile_Draw_OamIdx[k] / 4;
  uint8 x = 0x34;
  uint8 y = kSelectFile_Draw_Y[k];

  uint8 sword = sram[kSrmOffs_Sword] - 1;
  uint8 swordchar = kSelectFile_Draw_SwordChar[sign8(sword) ? 0 : sword];
  SetOamPlain(oam + 0, x + 0xc, y - 5, swordchar, kSelectFile_Draw_Flags[k], 0);
  SetOamPlain(oam + 1, x + 0xc, y + 3, swordchar + 16, kSelectFile_Draw_Flags[k], 0);
  if (sign8(sword))
    oam[1].y = oam[0].y = 0xf0;
  uint8 shield = sram[kSrmOffs_Shield] - 1;
  SetOamPlain(oam + 2, x - 5, y + 10, kSelectFile_Draw_ShieldChar[sign8(shield) ? 0 : shield], kSelectFile_Draw_Flags2[k], 2);
  if (sign8(shield))
    oam[2].y = 0xf0;
  SetOamPlain(oam + 3, x, y + 0, 0, kSelectFile_Draw_Flags3[k], 2);
  SetOamPlain(oam + 4, x, y + 8, 2, kSelectFile_Draw_Flags3[k] | 0x40, 2);
}

void SelectFile_Func6_DrawOams2(int k) {
  static const uint8 kSelectFile_DrawDigit_Char[10] = {0xd0, 0xac, 0xad, 0xbc, 0xbd, 0xae, 0xaf, 0xbe, 0xbf, 0xc0};
  static const int8 kSelectFile_DrawDigit_OamIdx[3] = {4, 16, 28};
  static const int8 kSelectFile_DrawDigit_X[3] = {12, 4, -4};

  uint8 *sram = g_zenv.sram + 0x500 * k;
  uint8 x = 0x34;
  uint8 y = kSelectFile_Draw_Y[k];

  int died_ctr = WORD(sram[kSrmOffs_DiedCounter]);
  if (died_ctr == 0xffff)
    return;

  if (died_ctr > 999)
    died_ctr = 999;

  uint8 digits[3];
  digits[2] = died_ctr / 100;
  died_ctr %= 100;
  digits[1] = died_ctr / 10;
  digits[0] = died_ctr % 10;

  int i = (digits[2] != 0) ? 2 : (digits[1] != 0) ? 1 : 0;
  OamEnt *oam = oam_buf + kSelectFile_DrawDigit_OamIdx[k] / 4;
  do {
    SetOamPlain(oam, x + kSelectFile_DrawDigit_X[i], y + 0x10, kSelectFile_DrawDigit_Char[digits[i]], 0x3c, 0);
  } while (oam++, --i >= 0);
}

void SelectFile_Func17(int k) {
  static const uint16 kSelectFile_DrawName_VramOffs[3] = {8, 0x5c, 0xb0};
  static const uint16 kSelectFile_DrawName_HealthVramOffs[3] = {0x16, 0x6a, 0xbe};
  uint8 *sram = g_zenv.sram + 0x500 * k;
  uint16 *name = (uint16 *)(sram + kSrmOffs_Name);
  uint16 *dst = vram_upload_data + kSelectFile_DrawName_VramOffs[k] / 2;
  for (int i = 5; i >= 0; i--) {
    uint16 t = *name++ + 0x1800;
    dst[0] = t;
    dst[21] = t + 0x10;
    dst++;
  }
  int health = sram[kSrmOffs_Health] >> 3;
  dst = vram_upload_data + kSelectFile_DrawName_HealthVramOffs[k] / 2;
  uint16 *dst_org = dst;
  int row = 10;
  do {
    *dst++ = 0x520;
    if (--row == 0)
      dst = dst_org + 21;
  } while (--health);
}

void SelectFile_Func16() {
  static const uint8 kSelectFile_Func16_FaerieY[2] = {175, 191};
  FileSelect_DrawFairy(0x1c, kSelectFile_Func16_FaerieY[selectfile_R16]);

  int k = selectfile_R16;
  if (filtered_joypad_H & 0x2c) {
    k += (filtered_joypad_H & 0x24) ? 1 : -1;
    selectfile_R16 = k & 1;
    sound_effect_2 = 0x20;
  }

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0;
  if (a != 0) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 0) {
      sound_effect_2 = 0x22;
      sound_effect_1 = 0x0;
      int k = subsubmodule_index;
      selectfile_arr1[k] = 0;
      memset(g_zenv.sram + k * 0x500, 0, 0x500);
      memset(g_zenv.sram + k * 0x500 + 0xf00, 0, 0x500);
      Randomizer_SlotErased(k);
      ZeldaWriteSram();
    }
    ReturnToFileSelect();
    subsubmodule_index = 0;
  }
}

void Module_NamePlayer_1() {
  uint16 *dst = SelectFile_Func1();
  dst[0] = 0xffff;
  nmi_load_bg_from_vram = 1;
  submodule_index++;
}

void Module_NamePlayer_2() {
  nmi_load_bg_from_vram = 5;
  submodule_index++;
  INIDISP_copy = 15;
  nmi_disable_core_updates = 0;
}

void Intro_FixCksum(uint8 *s) {
  uint16 *src = (uint16 *)s;
  uint16 sum = 0;
  for (int i = 0; i < 0x27f; i++)
    sum += src[i];
  src[0x27f] = 0x5a5a - sum;
}

void LoadFileSelectGraphics() {  // 80e4e9
  Decomp_spr(&g_ram[0x14000], 0x5e);
  Do3To4High(&g_zenv.vram[0x5000], &g_ram[0x14000]);

  Decomp_spr(&g_ram[0x14000], 0x5f);
  Do3To4High(&g_zenv.vram[0x5400], &g_ram[0x14000]);

  TransferFontToVRAM();

  Decomp_spr(&g_ram[0x14000], 0x6b);
  memcpy(&g_zenv.vram[0x7800], &g_ram[0x14000], 0x300 * sizeof(uint16));
}

void Intro_ValidateSram() {  // 828054
  uint8 *cart = g_zenv.sram;
  for (int i = 0; i < 3; i++) {
    uint8 *c = cart + i * 0x500;
    if (!Intro_CheckCksum(c)) {
      if (Intro_CheckCksum(c + 0xf00)) {
        memcpy(c, c + 0xf00, 0x500);
      } else {
        memset(c, 0, 0x500);
        memset(c + 0xf00, 0, 0x500);
      }
    }
  }
  memset(&g_ram[0xd00], 0, 256 * 3);
}

void Module01_FileSelect() {  // 8ccd7d
  BG3HOFS_copy2 = 0;
  BG3VOFS_copy2 = 0;
  switch (submodule_index) {
  case 0: Module_SelectFile_0(); break;
  case 1: FileSelect_ReInitSaveFlagsAndEraseTriforce(); break;
  case 2: Module_EraseFile_1(); break;
  case 3: FileSelect_TriggerStripesAndAdvance(); break;
  case 4: FileSelect_TriggerNameStripesAndAdvance(); break;
  case 5: FileSelect_Main(); break;
  case 8: FileSelect_RandoPage(); break;   // stream build: RANDOMIZER page
  case 9: FileSelect_OptionsPage(); break; // stream build: OPTIONS (controller setup)
  }
}

void Module_SelectFile_0() {  // 8ccd9d
  EnableForceBlank();
  is_nmi_thread_active = 0;
  nmi_flag_update_polyhedral = 0;
  music_control = 11;
  submodule_index++;
  overworld_palette_aux_or_main = 0x200;
  palette_main_indoors = 6;
  nmi_disable_core_updates = 6;
  Palette_Load_DungeonSet();
  Palette_Load_OWBG3();
  hud_palette = 0;
  Palette_Load_HUD();
  hud_cur_item = 0;
  misc_sprites_graphics_index = 1;
  main_tile_theme_index = 35;
  aux_tile_theme_index = 81;
  LoadDefaultGraphics();
  InitializeTilesets();
  LoadFileSelectGraphics();
  Intro_ValidateSram();
  DecompressEnemyDamageSubclasses();
}

void FileSelect_ReInitSaveFlagsAndEraseTriforce() {  // 8ccdf2
  memset(selectfile_arr1, 0, 6);
  FileSelect_EraseTriforce();
}

void FileSelect_EraseTriforce() {  // 8ccdf9
  nmi_disable_core_updates = 128;
  EnableForceBlank();
  EraseTileMaps_triforce();
  Palette_LoadForFileSelect();
  flag_update_cgram_in_nmi++;
  submodule_index++;
}

void Module_EraseFile_1() {  // 8cce53
  static const uint8 kSelectFile_Gfx0[224] = {
    0x10, 0x42,    0, 0x27, 0x89, 0x35, 0x8a, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35,
    0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35,
    0x8b, 0x35, 0x8c, 0x35, 0x8b, 0x35, 0x8c, 0x35, 0x8a, 0x75, 0x89, 0x75, 0x10, 0x62,    0,    3,
    0x99, 0x35, 0x9a, 0x35, 0x10, 0x64, 0x40, 0x1e, 0x7f, 0x34, 0x10, 0x74,    0,    3, 0x9a, 0x75,
    0x99, 0x75, 0x10, 0x82,    0,    3, 0xa9, 0x35, 0xaa, 0x35, 0x10, 0x84, 0x40, 0x1e, 0x7f, 0x34,
    0x10, 0x94,    0,    3, 0xaa, 0x75, 0xa9, 0x75, 0x10, 0xa2,    0, 0x27, 0x9d, 0x35, 0xad, 0x35,
    0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35,
    0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35, 0x9b, 0x35, 0x9c, 0x35,
    0xad, 0x75, 0x9d, 0x75, 0x10, 0xc2,    0, 0x27, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35,
    0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35,
    0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x35, 0xac, 0x35, 0xab, 0x75, 0xac, 0x75,
    0x10, 0xe2,    0,    1, 0x83, 0x35, 0x10, 0xe3, 0x40, 0x32, 0x85, 0x35, 0x10, 0xfd,    0,    1,
    0x84, 0x35, 0x11,    2, 0xc0, 0x22, 0x86, 0x35, 0x11, 0x1d, 0xc0, 0x22, 0x96, 0x35, 0x13, 0x42,
       0,    1, 0x93, 0x35, 0x13, 0x43, 0x40, 0x32, 0x95, 0x35, 0x13, 0x5d,    0,    1, 0x94, 0x35,
  };
  uint16 *dst = SelectFile_Func1();
  memcpy(dst, kSelectFile_Gfx0, 224);
  dst += 224 / 2;
  uint16 t = 0x1103;
  for (int i = 17; i >= 0; i--) {
    *dst++ = swap16(t);
    t += 0x20;
    *dst++ = 0x3240;
    *dst++ = 0x347f;
  }
  *(uint8 *)dst = 0xff;
  submodule_index++;
  nmi_load_bg_from_vram = 1;
}

void FileSelect_TriggerStripesAndAdvance() {  // 8ccea5
  selectfile_R16 = selectfile_var2;
  submodule_index++;
  nmi_load_bg_from_vram = 6;
}

void FileSelect_TriggerNameStripesAndAdvance() {  // 8cceb1
  s_fs_moved = false;   // stream build: COPY/ERASE are back on their ROM rows after this upload
  static const uint8 kSelectFile_Func3_Data[253] = {
    0x61, 0x29,    0, 0x25, 0xe7, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0x49,    0, 0x25, 0xf7, 0x18,
    0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x61, 0xa9,    0, 0x25, 0xe8, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xc9,
       0, 0x25, 0xf8, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x29,    0, 0x25, 0xe9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0x62, 0x49,    0, 0x25, 0xf9, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  memcpy(vram_upload_data, kSelectFile_Func3_Data, 253);
  INIDISP_copy = 0xf;
  nmi_disable_core_updates = 0;
  submodule_index++;
  nmi_load_bg_from_vram = 6;
}

// ---- RANDOMIZER ON/OFF + SEED rows on the file-select screen ----
// Text pokes the same BG layer the save names use (tilemap base 0x1000,
// dialogue-font chars at 0x7000, palette 6 => tileword chr|0x1800, blank
// 0x18a9), in rows the per-frame name stripes never touch (rows 8-25 of
// the box interior).  Letter->chr from the naming keyboard's own tilemap:
// it spells REGISTER(I=0xaf) YOUR NAME / END with these tile numbers, so
// A-P are 0x00-0x0f, Q-Z 0x20-0x29.  The font has no digits, so the seed
// number renders with the death-counter digit sprites instead (same table
// as SelectFile_Func6_DrawOams2).  The fill runs at process boot
// (Randomizer_Init), so edits apply the NEXT boot: values show "NEXT
// BOOT" while the pending value differs from what this boot started
// with, and every change commits randomizer.ini right away (atomic;
// console banners come from randomizer.c).
static int s_fs_new_file_slot = -1;  // a file was just named: open its RANDO page once
static const uint8 kFileSel_AsciiChr[26] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xaf, 0x09,  // A-J
  0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,                          // K-P
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,  // Q-Z
};
static const uint8 kFileSel_SeedDigitChar[10] = {  // == kSelectFile_DrawDigit_Char
  0xd0, 0xac, 0xad, 0xbc, 0xbd, 0xae, 0xaf, 0xbe, 0xbf, 0xc0,
};

// The save-name font is 8x16: every letter is TWO tiles, chr on the top
// row and chr+0x10 on the row below (exactly how SelectFile_Func17 draws
// the file names). Writing only the top tile rendered the labels as a
// stripe of garbage, which is what the first live check showed.
static void FileSel_DrawText(int row, int col, const char *s) {
  // the file-select box (and the save names) live on the tilemap at VRAM
  // 0x6000: the name stripes upload to 0x6129 etc (kSelectFile_Func3_Data).
  // 0x1000 is a different layer with the cave tileset - hence the garbage.
  uint16 *top = &g_zenv.vram[0x6000 + row * 32 + col];
  uint16 *bot = top + 32;
  while (*s != 0) {
    char c = *s++;
    if (c == ' ') {
      *top++ = 0x18a9;
      *bot++ = 0x18a9;
    } else {
      uint16 t = (uint16)(0x1800 | kFileSel_AsciiChr[c - 'A']);
      *top++ = t;
      *bot++ = t + 0x10;
    }
  }
}

// ---- RANDOMIZER page (stream build) --------------------------------------
// A real in-game screen in the file-select style: the same box, the 8x16
// save-name font, the fairy as cursor, seed digits as sprites. It opens right
// after a new file is named (BEGIN starts that file with the settings fixed).
// The rows come from randomizer.c (Randomizer_PageRow): RANDOMIZER, SEED,
// LOGIC, START, every extra logic setting, BEGIN. Six rows a page; L/R flip
// pages except on the SEED row where they jump the seed by 100. START begins
// from any page. When the chosen combination has no rule folder yet, BEGIN
// first shows a notice and builds it (python_embed, a few seconds).
static int s_rp_slot = -1;                       // file being created, -1 = defaults page
static int s_rp_cursor, s_rp_hold, s_rp_building;
enum { kRP_PerPage = 6 };

static void FileSelect_OpenRandoPage(int slot) {
  s_rp_slot = slot;
  s_rp_cursor = 0;
  s_rp_hold = 0;
  s_rp_building = 0;
  submodule_index = 8;
}

static void RandoPage_HideOam(int from, int to) {
  for (int i = from; i < to; i++)
    SetOamPlain(&oam_buf[i], 0, 0xf0, 0, 0, 0);
}

static void RandoPage_Draw(void) {
  // clear the box interior (rows 4-26, cols 3-28); the frame tiles stay
  for (int row = 4; row <= 26; row++)
    for (int col = 3; col <= 28; col++)
      g_zenv.vram[0x6000 + row * 32 + col] = 0x18a9;
  RandoPage_HideOam(1, 128);
  FileSel_DrawText(5, 4, s_rp_slot >= 0 ? "NEW FILE SETTINGS" : "RANDOMIZER DEFAULTS");
  if (s_rp_building) {
    RandoPage_HideOam(0, 1);
    FileSel_DrawText(12, 4, "BUILDING THE RULES FOR");
    FileSel_DrawText(14, 4, "THESE SETTINGS");
    FileSel_DrawText(16, 4, "ONE MOMENT");
    return;
  }
  int n = Randomizer_PageRows();
  int pages = (n + kRP_PerPage - 1) / kRP_PerPage;
  int page = s_rp_cursor / kRP_PerPage;
  const char *label, *d1, *d2;
  char val[16];
  for (int i = 0; i < kRP_PerPage; i++) {
    int idx = page * kRP_PerPage + i;
    if (idx >= n) break;
    int kind = Randomizer_PageRow(idx, &label, val, sizeof val, &d1, &d2);
    int row = 8 + i * 2;
    char line[16];
    snprintf(line, sizeof line, "%-12.12s", kind == kRPKind_Begin ? (s_rp_slot >= 0 ? "BEGIN" : "OK") : label);
    FileSel_DrawText(row, 4, line);
    if (kind == kRPKind_Seed) {
      int seed = Randomizer_SettingsSeed();
      if (seed > 0) {
        uint8 digits[9];
        int nd = 0, v = seed;
        while (v != 0) { digits[nd++] = (uint8)(v % 10); v /= 10; }
        for (int k = 0; k < nd; k++)
          SetOamPlain(&oam_buf[1 + k], (uint8)(232 - nd * 8 + k * 8), row * 8 + 4,
                      kFileSel_SeedDigitChar[digits[nd - 1 - k]], 0x3c, 0);
      } else {
        FileSel_DrawText(row, 17, "RANDOM");
      }
    } else if (kind == kRPKind_Enum) {
      snprintf(line, sizeof line, "%-11.11s", val);
      FileSel_DrawText(row, 17, line);
    }
  }
  {
    int kind = Randomizer_PageRow(s_rp_cursor, &label, val, sizeof val, &d1, &d2);
    if (kind == kRPKind_Begin) {
      if (s_rp_slot >= 0) { d1 = "STARTS THE GAME WITH"; d2 = "THESE SETTINGS LOCKED"; }
      else { d1 = "SAVES THESE DEFAULTS"; d2 = "FOR THE NEXT NEW FILE"; }
    }
    char pad1[24], pad2[24];
    snprintf(pad1, sizeof pad1, "%-22.22s", d1);
    snprintf(pad2, sizeof pad2, "%-22.22s", d2);
    FileSel_DrawText(20, 4, pad1);
    FileSel_DrawText(22, 4, pad2);
  }
  if (pages > 1) {
    FileSel_DrawText(24, 4, "LR PAGE   OF   START GO");
    SetOamPlain(&oam_buf[10], 12 * 8, 24 * 8 + 4, kFileSel_SeedDigitChar[(page + 1) % 10], 0x3c, 0);
    SetOamPlain(&oam_buf[11], 17 * 8, 24 * 8 + 4, kFileSel_SeedDigitChar[pages % 10], 0x3c, 0);
  } else {
    FileSel_DrawText(24, 4, s_rp_slot >= 0 ? "START TO BEGIN  B BACK" : "START OR A OK   B BACK");
  }
  FileSelect_DrawFairy(0x14, (8 + (s_rp_cursor % kRP_PerPage) * 2) * 8 + 2);
  // no nmi_load_bg_from_vram here: that flag re-uploads the file-select
  // stripe (names, hearts, 1./2./3.) over the page every frame; the tiles
  // written above go straight to VRAM
}

static void RandoPage_Accept(void) {
  if (!s_rp_building && Randomizer_SettingsEnabled() && !Randomizer_RulesReady()) {
    s_rp_building = 1;                           // notice first, build next frame
    return;
  }
  int slot = s_rp_slot;
  s_rp_slot = -1;
  s_rp_building = 0;
  Randomizer_SettingsCommit();                   // defaults for the next file
  sound_effect_1 = 0x2c;
  if (slot >= 0) {
    Randomizer_NewFileStamp(slot);               // fixed for this file from here on
    FileSelect_BeginFile(slot);
  } else {
    ReturnToFileSelect();
  }
}

void FileSelect_RandoPage() {
  RandoPage_Draw();
  if (s_rp_building) {
    if (s_rp_building++ >= 3) {                  // the notice has been on screen
      Randomizer_PrepareRules();                 // blocks while python runs
      RandoPage_Accept();
    }
    return;
  }
  uint8 h = filtered_joypad_H, l = filtered_joypad_L;
  int n = Randomizer_PageRows();
  const char *label, *d1, *d2;
  char val[16];
  int kind = Randomizer_PageRow(s_rp_cursor, &label, val, sizeof val, &d1, &d2);
  if (h & kJoypadH_Up) {
    s_rp_cursor = (s_rp_cursor + n - 1) % n;
    sound_effect_2 = 0x20;
  } else if (h & kJoypadH_Down) {
    s_rp_cursor = (s_rp_cursor + 1) % n;
    sound_effect_2 = 0x20;
  }
  int delta = 0;
  if (h & kJoypadH_Left) delta = -1;
  if (h & kJoypadH_Right) delta = 1;
  if (kind == kRPKind_Seed) {
    if (l & kJoypadL_L) delta = -100;
    if (l & kJoypadL_R) delta = 100;
  } else if (l & (kJoypadL_L | kJoypadL_R)) {
    int pages = (n + kRP_PerPage - 1) / kRP_PerPage;
    int page = (s_rp_cursor / kRP_PerPage + ((l & kJoypadL_R) ? 1 : pages - 1)) % pages;
    s_rp_cursor = page * kRP_PerPage;
    sound_effect_2 = 0x20;
  }
  int held = joypad1H_last & (kJoypadH_Left | kJoypadH_Right);
  if (held) {
    if (++s_rp_hold >= 20 && (s_rp_hold & 3) == 0)
      delta = (held & kJoypadH_Left) ? -10 : 10;
  } else {
    s_rp_hold = 0;
  }
  if (delta != 0) {
    sound_effect_2 = 0x20;
    if (kind == kRPKind_Seed) Randomizer_SettingsSeedAdjustFrom(delta);
    else if (kind == kRPKind_Enum) Randomizer_PageRowCycle(s_rp_cursor, delta);
  }
  if (l & kJoypadL_A) {
    sound_effect_2 = 0x20;
    if (kind == kRPKind_Seed) Randomizer_SettingsSeedToggleRandom();
    else if (kind == kRPKind_Enum) Randomizer_PageRowCycle(s_rp_cursor, 1);
    else { RandoPage_Accept(); return; }
  }
  if (h & kJoypadH_Start) { RandoPage_Accept(); return; }
  if (h & kJoypadH_B) {
    Randomizer_SettingsCommit();
    s_rp_slot = -1;
    sound_effect_1 = 0x2c;
    ReturnToFileSelect();
    return;
  }
}

// ---- OPTIONS page (stream build): controller setup -----------------------
// Opens from the OPTIONS line under COPY / ERASE PLAYER. A menu in the
// file-select style: define the pad buttons, define the keyboard keys (one
// press per SNES input, in order), test them with live ON markers, or reset
// to the zelda3.ini lines. Bindings live in controls.ini (src/controls.c).
enum { kOP_Top, kOP_Menu, kOP_DefinePad, kOP_DefineKey, kOP_Test, kOP_List };
enum { kOP_MenuRows = 5 };   // PAD BUTTONS, KEYBOARD KEYS, TEST BUTTONS, RESET TO DEFAULT, BACK
enum { kOP_TopRows = 5 };    // CONTROLS, VIDEO, GAME FEATURES, AUDIO, BACK
enum { kOP_PerPage = 6 };    // settings rows per page (rows 8..19, two tile rows each)
static int s_op_state, s_op_cursor, s_op_ctl, s_op_timer;
static int s_op_top, s_op_page, s_op_row;   // top-level cursor; settings page kind and row
static const char *const kOP_TopText[kOP_TopRows] = { "CONTROLS", "VIDEO", "GAME FEATURES", "AUDIO", "BACK" };
static const char *const kOP_TopDesc[kOP_TopRows][2] = {
  { "PAD AND KEYBOARD", "SETUP AND TEST" },
  { "WIDESCREEN WINDOW", "FULLSCREEN AND MORE" },
  { "THE PC PORT EXTRAS", "ITEM SWITCH FIXES ETC" },
  { "SOUND SETTINGS", "" },
  { "TO THE PLAYER SELECT", "" },
};
static const char *s_op_note;           // one-shot message under the menu (SAVED / RESET DONE)
static const uint8 kOP_MenuRowY[kOP_MenuRows] = { 8, 11, 14, 17, 20 };
static const char *const kOP_MenuText[kOP_MenuRows] = {
  "PAD BUTTONS", "KEYBOARD KEYS", "TEST BUTTONS", "RESET TO DEFAULT", "BACK",
};
static const char *const kOP_MenuDesc[kOP_MenuRows][2] = {
  { "PRESS ONE PAD BUTTON", "FOR EACH GAME BUTTON" },
  { "PRESS ONE KEY FOR", "EACH GAME BUTTON" },
  { "SHOWS WHAT THE GAME", "SEES WHEN YOU PRESS" },
  { "FORGETS YOUR SETUP", "BACK TO THE INI FILE" },
  { "BACK TO OPTIONS", "" },
};

static void FileSelect_OpenOptionsPage(void) {
  s_op_state = kOP_Top;
  s_op_top = 0;
  s_op_cursor = 0;
  s_op_ctl = 0;
  s_op_timer = 0;
  s_op_note = NULL;
  submodule_index = 9;
}

static void OptionsPage_ClearBox(void) {
  for (int row = 4; row <= 26; row++)
    for (int col = 3; col <= 28; col++)
      g_zenv.vram[0x6000 + row * 32 + col] = 0x18a9;
  RandoPage_HideOam(1, 128);
}

// The 12 controls in two columns (UP..START left, A..R right): name, then the
// binding (or ON while held on the test page). |blink| is the control being
// defined: its value field flashes so the eye lands on the right row.
static void OptionsPage_DrawBindings(int kind, int blink) {
  for (int i = 0; i < kCtl_Count; i++) {
    int row = 8 + (i % 6) * 2, col = i < 6 ? 3 : 16;
    const char *val;
    if (kind == kCtlKind_Test) val = Controls_InputHeld(i) ? "ON" : "";
    else if (kind == kCtlKind_Pad) val = Controls_PadLabel(Controls_GetPad(i));
    else val = Controls_KeyLabel(Controls_GetKey(i));
    if (i == blink && (frame_counter & 16)) val = "";
    char line[16];
    snprintf(line, sizeof line, "%-7.7s%-6.6s", kControls_Names[i], val);
    FileSel_DrawText(row, col, line);
  }
}

// page number as digit sprites (the font has none): "LR PAGE n OF m" bottom row
static void OptionsPage_DrawPageNo(int page, int pages) {
  FileSel_DrawText(24, 4, "LR PAGE   OF    B BACK");
  SetOamPlain(&oam_buf[1], 12 * 8, 24 * 8 + 4, kFileSel_SeedDigitChar[(page + 1) % 10], 0x3c, 0);
  SetOamPlain(&oam_buf[2], 17 * 8, 24 * 8 + 4, kFileSel_SeedDigitChar[pages % 10], 0x3c, 0);
}

static void OptionsPage_DrawList(void) {
  int n = Options_Count(s_op_page);
  int pages = (n + kOP_PerPage - 1) / kOP_PerPage;
  int page = s_op_row / kOP_PerPage;
  FileSel_DrawText(5, 4, Options_PageTitle(s_op_page));
  for (int i = 0; i < kOP_PerPage; i++) {
    int idx = page * kOP_PerPage + i;
    const OptItem *it = Options_Item(s_op_page, idx);
    if (!it) break;
    char line[26];
    snprintf(line, sizeof line, "%-13.13s%-10.10s", it->label, it->names[it->get()]);
    FileSel_DrawText(8 + i * 2, 4, line);
  }
  const OptItem *cur = Options_Item(s_op_page, s_op_row);
  if (cur) {
    FileSel_DrawText(20, 4, cur->desc1);
    FileSel_DrawText(22, 4, cur->desc2);
  }
  if (pages > 1) OptionsPage_DrawPageNo(page, pages);
  else FileSel_DrawText(24, 4, "LEFT RIGHT SETS  B BACK");
  FileSelect_DrawFairy(0x14, (8 + (s_op_row % kOP_PerPage) * 2) * 8 + 2);
}

static void OptionsPage_Draw(void) {
  OptionsPage_ClearBox();
  switch (s_op_state) {
  case kOP_Top:
    FileSel_DrawText(5, 4, "OPTIONS");
    for (int i = 0; i < kOP_TopRows; i++)
      FileSel_DrawText(kOP_MenuRowY[i], 4, kOP_TopText[i]);
    FileSel_DrawText(22, 4, kOP_TopDesc[s_op_top][0]);
    FileSel_DrawText(24, 4, kOP_TopDesc[s_op_top][1]);
    FileSelect_DrawFairy(0x14, kOP_MenuRowY[s_op_top] * 8 + 2);
    break;
  case kOP_List:
    OptionsPage_DrawList();
    break;
  case kOP_Menu:
    FileSel_DrawText(5, 4, "CONTROLS");
    for (int i = 0; i < kOP_MenuRows; i++)
      FileSel_DrawText(kOP_MenuRowY[i], 4, kOP_MenuText[i]);
    if (s_op_note && s_op_timer > 0) {
      FileSel_DrawText(22, 4, s_op_note);
    } else {
      FileSel_DrawText(22, 4, kOP_MenuDesc[s_op_cursor][0]);
      FileSel_DrawText(24, 4, kOP_MenuDesc[s_op_cursor][1]);
    }
    FileSelect_DrawFairy(0x14, kOP_MenuRowY[s_op_cursor] * 8 + 2);
    break;
  case kOP_DefinePad:
  case kOP_DefineKey: {
    bool pad = s_op_state == kOP_DefinePad;
    RandoPage_HideOam(0, 1);   // no fairy: the flashing value field marks the row
    FileSel_DrawText(5, 4, pad ? "SET PAD BUTTONS" : "SET KEYBOARD KEYS");
    OptionsPage_DrawBindings(pad ? kCtlKind_Pad : kCtlKind_Key, s_op_ctl);
    char prompt[26];
    snprintf(prompt, sizeof prompt, "%s %s", pad ? "PAD BUTTON FOR" : "KEY FOR", kControls_Names[s_op_ctl]);
    FileSel_DrawText(22, 4, prompt);
    FileSel_DrawText(24, 4, "ESC KEY CANCELS");
    break;
  }
  case kOP_Test:
    RandoPage_HideOam(0, 1);
    FileSel_DrawText(5, 4, "TEST BUTTONS");
    OptionsPage_DrawBindings(kCtlKind_Test, -1);
    FileSel_DrawText(22, 4, "PRESS BUTTONS TO TEST");
    FileSel_DrawText(24, 4, "EXIT ESC OR START SELECT");
    break;
  }
  if (s_op_timer > 0) s_op_timer--;
}

static void OptionsPage_LeaveDefine(bool keep) {
  Controls_CaptureEnd();
  if (keep) {
    Controls_Save();
    s_op_note = "SAVED";
  } else {
    Controls_Restore();
    s_op_note = "NOT CHANGED";
  }
  s_op_timer = 120;
  s_op_state = kOP_Menu;
  sound_effect_1 = 0x2c;
}

void FileSelect_OptionsPage() {
  OptionsPage_Draw();
  uint8 h = filtered_joypad_H, l = filtered_joypad_L;
  switch (s_op_state) {
  case kOP_Top:
    if (h & kJoypadH_Up) {
      s_op_top = (s_op_top + kOP_TopRows - 1) % kOP_TopRows;
      sound_effect_2 = 0x20;
    } else if (h & kJoypadH_Down) {
      s_op_top = (s_op_top + 1) % kOP_TopRows;
      sound_effect_2 = 0x20;
    }
    if (h & kJoypadH_B) {
      sound_effect_1 = 0x2c;
      ReturnToFileSelect();
      return;
    }
    if ((l & kJoypadL_A) || (h & kJoypadH_Start)) {
      sound_effect_1 = 0x2c;
      switch (s_op_top) {
      case 0: s_op_state = kOP_Menu; s_op_cursor = 0; s_op_timer = 0; s_op_note = NULL; break;
      case 1: case 2: case 3:
        s_op_state = kOP_List;
        s_op_page = s_op_top - 1;    // kOptPage_Video, Features, Audio
        s_op_row = 0;
        break;
      default:
        ReturnToFileSelect();
        return;
      }
    }
    break;
  case kOP_List: {
    int n = Options_Count(s_op_page);
    if (h & kJoypadH_Up) {
      s_op_row = (s_op_row + n - 1) % n;
      sound_effect_2 = 0x20;
    } else if (h & kJoypadH_Down) {
      s_op_row = (s_op_row + 1) % n;
      sound_effect_2 = 0x20;
    }
    if (l & kJoypadL_L) {          // previous page (wraps)
      int pages = (n + kOP_PerPage - 1) / kOP_PerPage;
      int page = (s_op_row / kOP_PerPage + pages - 1) % pages;
      s_op_row = page * kOP_PerPage;
      sound_effect_2 = 0x20;
    } else if (l & kJoypadL_R) {
      int pages = (n + kOP_PerPage - 1) / kOP_PerPage;
      int page = (s_op_row / kOP_PerPage + 1) % pages;
      s_op_row = page * kOP_PerPage;
      sound_effect_2 = 0x20;
    }
    int delta = 0;
    if (h & kJoypadH_Left) delta = -1;
    if (h & kJoypadH_Right) delta = 1;
    if (l & kJoypadL_A) delta = 1;
    if (delta) {
      Options_Cycle(s_op_page, s_op_row, delta);
      sound_effect_2 = 0x20;
    }
    if (h & kJoypadH_B) {
      sound_effect_1 = 0x2c;
      s_op_state = kOP_Top;
    }
    break;
  }
  case kOP_Menu:
    if (h & kJoypadH_Up) {
      s_op_cursor = (s_op_cursor + kOP_MenuRows - 1) % kOP_MenuRows;
      sound_effect_2 = 0x20;
    } else if (h & kJoypadH_Down) {
      s_op_cursor = (s_op_cursor + 1) % kOP_MenuRows;
      sound_effect_2 = 0x20;
    }
    if (h & kJoypadH_B) {
      sound_effect_1 = 0x2c;
      s_op_state = kOP_Top;
      break;
    }
    if ((l & kJoypadL_A) || (h & kJoypadH_Start)) {
      sound_effect_1 = 0x2c;
      switch (s_op_cursor) {
      case 0:
      case 1:
        Controls_Snapshot();
        s_op_ctl = 0;
        s_op_state = s_op_cursor == 0 ? kOP_DefinePad : kOP_DefineKey;
        Controls_CaptureBegin(s_op_cursor == 0 ? kCtlKind_Pad : kCtlKind_Key);
        break;
      case 2:
        s_op_state = kOP_Test;
        Controls_CaptureBegin(kCtlKind_Test);
        break;
      case 3:
        Controls_ResetDefaults();
        s_op_note = "RESET DONE";
        s_op_timer = 120;
        break;
      default:
        s_op_state = kOP_Top;
        break;
      }
    }
    break;
  case kOP_DefinePad:
  case kOP_DefineKey: {
    int c = Controls_TakeCaptured();
    if (c == kCtl_CaptureCancel) {
      OptionsPage_LeaveDefine(false);
    } else if (c != kCtl_CaptureNone) {
      if (s_op_state == kOP_DefinePad) Controls_SetPad(s_op_ctl, c);
      else Controls_SetKey(s_op_ctl, c);
      sound_effect_2 = 0x20;
      if (++s_op_ctl >= kCtl_Count)
        OptionsPage_LeaveDefine(true);
    }
    break;
  }
  case kOP_Test: {
    int c = Controls_TakeCaptured();
    if (c == kCtl_CaptureCancel ||
        (Controls_InputHeld(kCtl_Start) && Controls_InputHeld(kCtl_Select))) {
      Controls_CaptureEnd();
      s_op_state = kOP_Menu;
      sound_effect_1 = 0x2c;
    }
    break;
  }
  }
}

// ---- bottom of the player select ------------------------------------------
// COPY PLAYER / ERASE PLAYER come from the ROM tilemap on rows 22-25 and the
// box ends at row 26, so OPTIONS gets its line by moving those two up two
// rows (recognised by the C of COPY on row 22 and the E of ERASE on row 24,
// once per tilemap upload). If the tiles are ever not where this expects,
// nothing moves and OPTIONS is drawn on the free band at row 19 instead.
static int s_fs_optcol = 4;
static void FileSelect_LayoutBottom(void) {
  uint16 *v = g_zenv.vram + 0x6000;
  if (!s_fs_moved) {
    int c = -1;
    for (int col = 3; col <= 24; col++)
      if ((v[22 * 32 + col] & 0x3ff) == 0x02 && (v[24 * 32 + col] & 0x3ff) == 0x04) { c = col; break; }
    if (c >= 0) {
      for (int row = 20; row <= 23; row++)
        memcpy(v + row * 32 + 3, v + (row + 2) * 32 + 3, 26 * sizeof(uint16));
      for (int row = 24; row <= 25; row++)
        for (int col = 3; col <= 28; col++) v[row * 32 + col] = 0x18a9;
      s_fs_optcol = c;
      s_fs_moved = true;
    }
  }
  FileSel_DrawText(s_fs_moved ? 24 : 19, s_fs_optcol, "OPTIONS");
}

void FileSelect_Main() {  // 8ccebd
  // slots 3-5 = COPY, ERASE, OPTIONS; the last three depend on whether the
  // ROM rows were moved up (FileSelect_LayoutBottom)
  static const uint8 kSelectFile_Faerie_Y_Moved[6] = {0x4a, 0x6a, 0x8a, 0x9f, 0xaf, 0xbf};
  static const uint8 kSelectFile_Faerie_Y_Rom[6]   = {0x4a, 0x6a, 0x8a, 0xaf, 0xbf, 0x97};

  const uint8 *cart = g_zenv.sram;

  if (selectfile_R16 < 3)
    selectfile_var2 = selectfile_R16;

  for (int k = 0; k < 3; k++) {
    if (*(uint16 *)(cart + k * 0x500 + 0x3E5) == 0x55AA) {
      selectfile_arr1[k] = 1;
      SelectFile_Func5_DrawOams(k);
      SelectFile_Func6_DrawOams2(k);
      SelectFile_Func17(k);
    }
  }

  FileSelect_LayoutBottom();
  {
    int slot = selectfile_R16 > 5 ? 5 : selectfile_R16;
    const uint8 *ys = s_fs_moved ? kSelectFile_Faerie_Y_Moved : kSelectFile_Faerie_Y_Rom;
    FileSelect_DrawFairy((slot == 5 && !s_fs_moved) ? 0x14 : 0x1c, ys[slot]);
  }
  nmi_load_bg_from_vram = 1;

  // a file was just named: its RANDOMIZER page opens over the file select
  if (s_fs_new_file_slot >= 0) {
    int slot = s_fs_new_file_slot;
    s_fs_new_file_slot = -1;
    FileSelect_OpenRandoPage(slot);
    return;
  }
  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    // files, COPY, ERASE, then OPTIONS (slot 5): A opens the controller setup
    sound_effect_2 = 0x20;
    if (a & 8) { if (sign8(--selectfile_R16)) selectfile_R16 = 5; }
    else       { if (++selectfile_R16 > 5) selectfile_R16 = 0; }
  } else if (a != 0) {
    if (selectfile_R16 == 5) {
      sound_effect_1 = 0x2c;
      FileSelect_OpenOptionsPage();
    } else {
      sound_effect_1 = 0x2c;
      if (selectfile_R16 < 3) {
        selectfile_R17 = 0;
        if (!selectfile_arr1[selectfile_R16]) {
          main_module_index = 4;
          submodule_index = 0;
          subsubmodule_index = 0;
        } else {
          music_control = 0xf1;
          srm_var1 = selectfile_R16 * 2 + 2;
          WORD(g_ram[0]) = selectfile_R16 * 0x500;
          CopySaveToWRAM();
        }
      } else if (selectfile_arr1[0] | selectfile_arr1[1] | selectfile_arr1[2]) {
        main_module_index = (selectfile_R16 == 3) ? 2 : 3;
        selectfile_R16 = 0;
        submodule_index = 0;
        subsubmodule_index = 0;
      } else {
        sound_effect_1 = 0x3c;
      }
    }
  }
}

// Start playing file |slot| (0-2): the same path as picking it on the screen.
void FileSelect_BeginFile(int slot) {
  selectfile_R17 = 0;
  music_control = 0xf1;
  srm_var1 = slot * 2 + 2;
  WORD(g_ram[0]) = slot * 0x500;
  CopySaveToWRAM();
}

void Module02_CopyFile() {  // 8cd053
  selectfile_var2 = 0;
  switch (submodule_index) {
  case 0: FileSelect_EraseTriforce(); break;
  case 1: Module_EraseFile_1(); break;
  case 2: Module_CopyFile_2(); break;
  case 3: CopyFile_ChooseSelection(); break;
  case 4: CopyFile_ChooseTarget(); break;
  case 5: CopyFile_ConfirmSelection(); break;
  }
}

void Module_CopyFile_2() {  // 8cd06e
  nmi_load_bg_from_vram = 7;
  submodule_index++;
  INIDISP_copy = 0xf;
  nmi_disable_core_updates = 0;
  int i = 0;
  for (; selectfile_arr1[i] == 0; i++) {}
  selectfile_R16 = i;
}

void CopyFile_ChooseSelection() {  // 8cd087
  CopyFile_SelectionAndBlinker();
  if (submodule_index == 3 && !(frame_counter & 0x30))
    FilePicker_DeleteHeaderStripe();
  nmi_load_bg_from_vram = 1;
}

void CopyFile_ChooseTarget() {  // 8cd0a2
  CopyFile_TargetSelectionAndBlink();
  if (submodule_index == 4 && !(frame_counter & 0x30))
    FilePicker_DeleteHeaderStripe();
  nmi_load_bg_from_vram = 1;
}

void CopyFile_ConfirmSelection() {  // 8cd0b9
  CopyFile_HandleConfirmation();
  nmi_load_bg_from_vram = 1;
}

void FilePicker_DeleteHeaderStripe() {  // 8cd0c6
  static const uint16 kFilePicker_DeleteHeaderStripe_Dst[2] = {4, 0x1e};
  for (int j = 1; j >= 0; j--) {
    uint16 *dst = vram_upload_data + kFilePicker_DeleteHeaderStripe_Dst[j] / 2;
    for (int i = 0; i != 11; i++)
      dst[i] = 0xa9;
  }
}

void CopyFile_SelectionAndBlinker() {  // 8cd13f
  static const uint8 kCopyFile_SelectionAndBlinker_Tab[173] = {
    0x61,    4,    0, 0x15, 0x85, 0x18, 0x26, 0x18,    7, 0x18, 0xaf, 0x18,    2, 0x18,    7, 0x18,
    0x6f, 0x18, 0x86, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0x24,    0, 0x15, 0x95, 0x18,
    0x36, 0x18, 0x17, 0x18, 0xbf, 0x18, 0x12, 0x18, 0x17, 0x18, 0x7f, 0x18, 0x96, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x61, 0x67,    0,  0xf, 0xe7, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0x87,    0,  0xf, 0xf7, 0x18, 0x91, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xc7,    0,  0xf,
    0xe8, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0x61, 0xe7,    0,  0xf, 0xf8, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x62, 0x27,    0,  0xf, 0xe9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x47,    0,  0xf, 0xf9, 0x18, 0x91, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  static const uint8 kCopyFile_SelectionAndBlinker_Tab1[73] = {
    0x61, 0x67, 0x40,  0xe, 0xa9,    0, 0x61, 0x87, 0x40,  0xe, 0xa9,    0, 0x61, 0xc7, 0x40,  0xe,
    0xa9,    0, 0x61, 0xe7, 0x40,  0xe, 0xa9,    0, 0x11, 0x30,    0,    1, 0x83, 0x35, 0x11, 0x31,
    0x40, 0x14, 0x85, 0x35, 0x11, 0x3c,    0,    1, 0x84, 0x35, 0x11, 0x50, 0xc0,  0xe, 0x86, 0x35,
    0x11, 0x5c, 0xc0,  0xe, 0x96, 0x35, 0x12, 0x50,    0,    1, 0x93, 0x35, 0x12, 0x51, 0x40, 0x14,
    0x95, 0x35, 0x12, 0x5c,    0,    1, 0x94, 0x35, 0xff,
  };
  static const uint16 kCopyFile_SelectionAndBlinker_Dst[3] = {0x3c, 0x64, 0x8c};
  static const uint8 kCopyFile_SelectionAndBlinker_FaerieX[4] = {36, 36, 36, 28};
  static const uint8 kCopyFile_SelectionAndBlinker_FaerieY[4] = {87, 111, 135, 191};

  vram_upload_offset = 0xac;
  memcpy(vram_upload_data, kCopyFile_SelectionAndBlinker_Tab, 173);

  for (int k = 0; k != 3; k++) {
    if (selectfile_arr1[k] & 1) {
      const uint16 *name = (uint16 *)(g_zenv.sram + 0x500 * k + kSrmOffs_Name);
      uint16 *dst = vram_upload_data + kCopyFile_SelectionAndBlinker_Dst[k] / 2;
      for (int i = 0; i != 6; i++) {
        uint16 t = *name++ + 0x1800;
        dst[0] = t;
        dst[10] = t + 0x10;
        dst++;
      }
    }
  }
  FileSelect_DrawFairy(kCopyFile_SelectionAndBlinker_FaerieX[selectfile_R16], kCopyFile_SelectionAndBlinker_FaerieY[selectfile_R16]);

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    uint8 k = selectfile_R16;
    if (a & 8) {
      do {
        if (--k < 0) {
          k = 3;
          break;
        }
      } while (!selectfile_arr1[k]);
    } else {
      do {
        k++;
        if (k >= 4)
          k = 0;
      } while (k != 3 && !selectfile_arr1[k]);
    }
    selectfile_R16 = k;
    sound_effect_2 = 0x20;
  } else if (a != 0) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 3) {
      ReturnToFileSelect();
      return;
    }
    selectfile_R20 = selectfile_R16 * 2;
    memcpy(vram_upload_data + 26, kCopyFile_SelectionAndBlinker_Tab1, 73);
    if (selectfile_R16 != 2) {
      uint16 *dst = vram_upload_data + selectfile_R16 * 6;
      dst[26] = 0x2762;
      dst[29] = 0x4762;
    }
    submodule_index++;
    selectfile_R16 = 0;
  }
}

void ReturnToFileSelect() {  // 8cd22d
  main_module_index = 1;
  submodule_index = 1;
  subsubmodule_index = 0;
  selectfile_R16 = 0;
}

void CopyFile_TargetSelectionAndBlink() {  // 8cd27b
  {
    int k = 1, t = 4;
    do {
      if (t != selectfile_R20)
        selectfile_arr2[k--] = t;
    } while ((t -= 2) >= 0);
  }

  static const uint8 kCopyFile_TargetSelectionAndBlink_Tab0[133] = {
    0x61, 0x51,    0, 0x15, 0x85, 0x18, 0x23, 0x18,  0xe, 0x18, 0xa9, 0x18, 0x26, 0x18,    7, 0x18,
    0xaf, 0x18,    2, 0x18,    7, 0x18, 0x6f, 0x18, 0x86, 0x18, 0x61, 0x71,    0, 0x15, 0x95, 0x18,
    0x33, 0x18, 0x1e, 0x18, 0xb9, 0x18, 0x36, 0x18, 0x17, 0x18, 0xbf, 0x18, 0x12, 0x18, 0x17, 0x18,
    0x7f, 0x18, 0x96, 0x18, 0x61, 0xb4,    0,  0xf, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xd4,    0,  0xf, 0xa9, 0x18, 0x91, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x14,    0,  0xf,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0x62, 0x34,    0,  0xf, 0xa9, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  static const uint8 kCopyFile_TargetSelectionAndBlink_Tab2[49] = {
    0x61, 0xb4, 0x40,  0xe, 0xa9,    0, 0x61, 0xd4, 0x40,  0xe, 0xa9,    0, 0x62, 0xc6,    0,  0xd,
       2, 0x18,  0xe, 0x18,  0xf, 0x18, 0x28, 0x18, 0xa9, 0x18,  0xe, 0x18,  0xa, 0x18, 0x62, 0xe6,
       0,  0xd, 0x12, 0x18, 0x1e, 0x18, 0x1f, 0x18, 0x38, 0x18, 0xa9, 0x18, 0x1e, 0x18, 0x1a, 0x18,
    0xff,
  };
  static const uint8 kCopyFile_TargetSelectionAndBlink_FaerieX[3] = {0x8c, 0x8c, 0x1c};
  static const uint8 kCopyFile_TargetSelectionAndBlink_FaerieY[3] = {0x67, 0x7f, 0xbf};
  static const uint16 kCopyFile_TargetSelectionAndBlink_Dst[2] = {0x38, 0x60};
  static const uint16 kCopyFile_TargetSelectionAndBlink_Tab1[3] = {0x18e7, 0x18e8, 0x18e9};
  memcpy(vram_upload_data, kCopyFile_TargetSelectionAndBlink_Tab0, 133);

  for (int k = 0, j = 0; k != 3; k++) {
    if (k * 2 == selectfile_R20)
      continue;

    uint16 *dst = vram_upload_data + kCopyFile_TargetSelectionAndBlink_Dst[j++] / 2;
    uint16 t = kCopyFile_TargetSelectionAndBlink_Tab1[k];
    dst[0] = t;
    dst[10] = t + 0x10;
    dst += 2;
    if (selectfile_arr1[k]) {
      const uint16 *name = (uint16 *)(g_zenv.sram + 0x500 * k + kSrmOffs_Name);
      for (int i = 0; i != 6; i++) {
        uint16 t = *name++ + 0x1800;
        dst[0] = t;
        dst[10] = t + 0x10;
        dst++;
      }
    }
  }

  vram_upload_offset = 132;

  FileSelect_DrawFairy(kCopyFile_TargetSelectionAndBlink_FaerieX[selectfile_R16], kCopyFile_TargetSelectionAndBlink_FaerieY[selectfile_R16]);

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    uint8 k = selectfile_R16;
    if (a & 8) {
      if (sign8(--k))
        k = 2;
    } else {
      if (++k >= 3)
        k = 0;
    }
    selectfile_R16 = k;
    sound_effect_2 = 0x20;
  } else if (a) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 2) {
      ReturnToFileSelect();
      selectfile_R16 = 0;
      return;
    }
    selectfile_R18 = selectfile_arr2[selectfile_R16];
    memcpy(vram_upload_data + 26, kCopyFile_TargetSelectionAndBlink_Tab2, 49);
    if (selectfile_R16 == 0) {
      uint16 *dst = vram_upload_data;
      dst[26] = 0x1462;
      dst[29] = 0x3462;
    }
    submodule_index++;
    selectfile_R16 = 0;
  }
}

void CopyFile_HandleConfirmation() {  // 8cd371
  static const uint8 kCopyFile_HandleConfirmation_FaerieY[2] = {0xaf, 0xbf};
  FileSelect_DrawFairy(0x1c, kCopyFile_HandleConfirmation_FaerieY[selectfile_R16]);

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xfc;
  if (a & 0x2c) {
    sound_effect_2 = 0x20;
    if (a & 0x24) {
      if (++selectfile_R16 >= 2)
        selectfile_R16 = 0;
    } else {
      if (sign8(--selectfile_R16))
        selectfile_R16 = 1;
    }
  } else if (a != 0) {
    sound_effect_1 = 0x2c;
    if (selectfile_R16 == 0) {
      memcpy(g_zenv.sram + (selectfile_R18 >> 1) * 0x500, g_zenv.sram + (selectfile_R20 >> 1) * 0x500, 0x500);
      Randomizer_SlotCopied(selectfile_R18 >> 1, selectfile_R20 >> 1);
      selectfile_arr1[(selectfile_R18 >> 1)] = 1;
      ZeldaWriteSram();
    }
    ReturnToFileSelect();
    selectfile_R16 = 0;
  }
}

void Module03_KILLFile() {  // 8cd485
  switch (submodule_index) {
  case 0: FileSelect_EraseTriforce(); break;
  case 1: Module_EraseFile_1(); break;
  case 2: KILLFile_SetUp(); break;
  case 3: KILLFile_HandleSelection(); break;
  case 4: KILLFile_HandleConfirmation(); break;
  }
}

void KILLFile_SetUp() {  // 8cd49a
  nmi_load_bg_from_vram = 8;
  submodule_index++;
  INIDISP_copy = 0xf;
  nmi_disable_core_updates = 0;
  int i = 0;
  for (; selectfile_arr1[i] == 0; i++) {}
  selectfile_R16 = i;
}

void KILLFile_HandleSelection() {  // 8cd49f
  if (selectfile_R16 < 3)
    selectfile_var2 = selectfile_R16;
  KILLFile_ChooseTarget();
  nmi_load_bg_from_vram = 1;
}

void KILLFile_HandleConfirmation() {  // 8cd4b1
  SelectFile_Func16();
  nmi_load_bg_from_vram = 1;
}

void KILLFile_ChooseTarget() {  // 8cd4ba
  static const uint8 kKILLFile_ChooseTarget_Tab[253] = {
    0x61, 0xa7,    0, 0x25, 0xe7, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x61, 0xc7,    0, 0x25, 0xf7, 0x18,
    0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0x62,    7,    0, 0x25, 0xe8, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x27,
       0, 0x25, 0xf8, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0x62, 0x67,    0, 0x25, 0xe9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0x62, 0x87,    0, 0x25, 0xf9, 0x18, 0x91, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18,
    0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xa9, 0x18, 0xff,
  };
  static const uint8 kKILLFile_ChooseTarget_Tab2[101] = {
    0x61, 0xa7, 0x40, 0x24, 0xa9,    0, 0x61, 0xc7, 0x40, 0x24, 0xa9,    0, 0x62,    7, 0x40, 0x24,
    0xa9,    0, 0x62, 0x27, 0x40, 0x24, 0xa9,    0, 0x62, 0xc6,    0, 0x21,    4, 0x18, 0x21, 0x18,
       0, 0x18, 0x22, 0x18,    4, 0x18, 0xa9, 0x18, 0x23, 0x18,    7, 0x18, 0xaf, 0x18, 0x22, 0x18,
    0xa9, 0x18,  0xf, 0x18,  0xb, 0x18,    0, 0x18, 0x28, 0x18,    4, 0x18, 0x21, 0x18, 0x62, 0xe6,
       0, 0x21, 0x14, 0x18, 0x31, 0x18, 0x10, 0x18, 0x32, 0x18, 0x14, 0x18, 0xa9, 0x18, 0x33, 0x18,
    0x17, 0x18, 0xbf, 0x18, 0x32, 0x18, 0xa9, 0x18, 0x1f, 0x18, 0x1b, 0x18, 0x10, 0x18, 0x38, 0x18,
    0x14, 0x18, 0x31, 0x18, 0xff,
  };
  static const uint8 kKILLFile_ChooseTarget_FaerieX[4] = {36, 36, 36, 28};
  static const uint8 kKILLFile_ChooseTarget_FaerieY[4] = {103, 127, 151, 191};
  memcpy(vram_upload_data, kKILLFile_ChooseTarget_Tab, 253);
  for (int k = 0; k < 3; k++) {
    if (selectfile_arr1[k])
      SelectFile_Func17(k);
  }

  FileSelect_DrawFairy(kKILLFile_ChooseTarget_FaerieX[selectfile_R16], kKILLFile_ChooseTarget_FaerieY[selectfile_R16]);

  int k = selectfile_R16;
  if (filtered_joypad_H & 0x2c) {
    if (!(filtered_joypad_H & 0x24)) {
      do {
        if (--k < 0) {
          k = 3;
          break;
        }
      } while (!selectfile_arr1[k]);
    } else {
      do {
        k++;
        if (k >= 4)
          k = 0;
      } while (k != 3 && !selectfile_arr1[k]);
    }
    sound_effect_2 = 0x20;
  }
  selectfile_R16 = k;

  uint8 a = (filtered_joypad_L & 0xc0 | filtered_joypad_H) & 0xd0;
  if (a) {
    sound_effect_1 = 0x2c;
    if (k == 3) {
      ReturnToFileSelect();
      return;
    }

    memcpy(vram_upload_data, kKILLFile_ChooseTarget_Tab2, 101);
    submodule_index++;
    if (selectfile_R16 != 2) {
      uint16 *dst = vram_upload_data + selectfile_R16 * 6;
      dst[0] = 0x6762;
      dst[3] = 0x8762;
    }
    subsubmodule_index = selectfile_R16;
    selectfile_R16 = 0;
  }
}

void FileSelect_DrawFairy(uint8 x, uint8 y) {  // 8cd7a5
  SetOamPlain(&oam_buf[0], x, y, frame_counter & 8 ? 0xaa : 0xa8, 0x7e, 2);
}

void Module04_NameFile() {  // 8cd88a
  switch (submodule_index) {
  case 0: NameFile_EraseSave(); break;
  case 1: Module_NamePlayer_1(); break;
  case 2: Module_NamePlayer_2(); break;
  case 3: NameFile_DoTheNaming(); break;
  }
}

void NameFile_EraseSave() {  // 8cd89c
  FileSelect_EraseTriforce();
  irq_flag = 1;
  selectfile_var3 = 0;
  selectfile_var4 = 0;
  selectfile_var5 = 0;
  selectfile_arr2[0] = 0;
  selectfile_var6 = 0;
  selectfile_var7 = 0x83;
  selectfile_var8 = 0x1f0;
  BG3HOFS_copy2 = 0;
  int offs = selectfile_R16 * 0x500;
  attract_legend_ctr = offs;
  memset(g_zenv.sram + offs, 0, 0x500);
  Randomizer_SlotErased(selectfile_R16);
  uint16 *name = (uint16 *)(g_zenv.sram + offs + kSrmOffs_Name);
  name[0] = name[1] = name[2] = name[3] = name[4] = name[5] = 0xa9;
}

void NameFile_DoTheNaming() {  // 8cda4d
  static const int16 kNamePlayer_Tab1[26] = {
    -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1,
    -2, 2, -2, 2, -2, 2, -2, 2, -4, 4,
  };
  static const uint8 kNamePlayer_Tab2[4] = {131, 147, 163, 179};
  static const int8 kNamePlayer_X[6] = {31, 47, 63, 79, 95, 111};
  static const int16 kNamePlayer_Tab0[32] = {
    0x1f0,     0,  0x10,  0x20,  0x30,  0x40,  0x50,  0x60,  0x70,  0x80,  0x90,  0xa0,  0xb0,  0xc0,  0xd0,  0xe0,
     0xf0, 0x100, 0x110, 0x120, 0x130, 0x140, 0x150, 0x160, 0x170, 0x180, 0x190, 0x1a0, 0x1b0, 0x1c0, 0x1d0, 0x1e0,
  };
  static const int8 kNamePlayer_Tab3[128] = {
       6,    7, 0x5f,    9, 0x59, 0x59, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x60, 0x23,
    0x59, 0x59, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x59, 0x59, 0x59,    0,    1,    2,    3,    4,    5,
    0x10, 0x11, 0x12, 0x13, 0x59, 0x59, 0x24, 0x5f, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x59, 0x59, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x59, 0x59, 0x59,  0xa,  0xb,  0xc,  0xd,  0xe,  0xf,
    0x40, 0x41, 0x42, 0x59, 0x59, 0x59, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x59,
    0x59, 0x59, 0x61, 0x3f, 0x45, 0x46, 0x59, 0x59, 0x59, 0x59, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x44, 0x59, 0x6f, 0x6f, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x5a, 0x44, 0x59, 0x6f, 0x6f,
    0x59, 0x59, 0x5a, 0x44, 0x59, 0x6f, 0x6f, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x59, 0x5a,
  };
  for (;;) {
    int j = selectfile_var9;
    if (j == 0) {
      NameFile_CheckForScrollInputX();
      break;
    }
    if (j != 0x31)
      selectfile_var9 += 4;
    j--;
    if (kNamePlayer_Tab0[selectfile_var3] == selectfile_var8) {
      selectfile_var9 = (joypad1H_last & 3) ? 0x30 : 0;
      NameFile_CheckForScrollInputX();
      continue;
    }
    if (!selectfile_var10)
      j += 2;
    selectfile_var8 = (selectfile_var8 + WORD(((uint8*)&kNamePlayer_Tab1)[j])) & 0x1ff;
    break;
  }

  for (;;) {
    if (selectfile_var11 == 0) {
      NameFile_CheckForScrollInputY();
      break;
    }
    uint8 diff = selectfile_var7 - kNamePlayer_Tab2[selectfile_var5];
    if (diff != 0) {
      selectfile_var7 += sign8(diff) ? 2 : -2;
      break;
    }
    selectfile_var11 = 0;
    NameFile_CheckForScrollInputY();
  }

  OamEnt *oam = oam_buf;
  for (int i = 0; i != 26; i++) {
    SetOamPlain(oam, 0x18 + i * 8, selectfile_var7, 0x2e, 0x3c, 0);
    oam++;
  }
  SetOamPlain(oam, kNamePlayer_X[selectfile_var4], 0x58, 0x29, 0xc, 0);

  if (selectfile_var9 | selectfile_var11)
    return;

  if (!(filtered_joypad_H & 0x10)) {
    if (!(filtered_joypad_H & 0xc0 || filtered_joypad_L & 0xc0))
      return;

    sound_effect_1 = 0x2b;
    uint8 t = kNamePlayer_Tab3[selectfile_var3 + selectfile_var5 * 0x20];
    if (t == 0x5a) {
      if (!selectfile_var4)
        selectfile_var4 = 5;
      else
        selectfile_var4--;
      return;
    } else if (t == 0x44) {
      if (++selectfile_var4 == 6)
        selectfile_var4 = 0;
      return;
    } else if (t != 0x6f) {
      int p = selectfile_var4 * 2 + attract_legend_ctr;
      uint16 chr = (t & 0xfff0) * 2 + (t & 0xf);
      WORD(g_zenv.sram[p + kSrmOffs_Name]) = chr;
      NameFile_DrawSelectedCharacter(selectfile_var4, chr);
      if (++selectfile_var4 == 6)
        selectfile_var4 = 0;
      return;
    }
  }
  int i = 0;
  for(;;) {
    uint16 a = WORD(g_zenv.sram[i * 2 + attract_legend_ctr + kSrmOffs_Name]);
    if (a != 0xa9)
      break;
    if (++i == 6) {
      sound_effect_1 = 0x3c;
      return;
    }
  }
  srm_var1 = selectfile_R16 * 2 + 2;
  uint8 *sram = &g_zenv.sram[selectfile_R16 * 0x500];
  WORD(sram[0x3e5]) = 0x55aa;
  WORD(sram[0x20c]) = 0xf000;
  WORD(sram[0x20e]) = 0xf000;
  WORD(sram[kSrmOffs_DiedCounter]) = 0xffff;
  static const uint8 kSramInit_Normal[60] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0,    0,    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0,    0,    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0x18, 0x18, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf8, 0, 0,
  };
  memcpy(sram + 0x340, kSramInit_Normal, 60);
  sram[0x4E0] = 1;                     // settings live in saves/rando_slots.ini
  Randomizer_NewFileInitSram(sram);    // OPEN start bytes when the pending START says so
  Randomizer_NewFileStamp(selectfile_R16);   // defaults; the page that follows can change them
  s_fs_new_file_slot = selectfile_R16;        // FileSelect_Main opens the RANDO page for it
  Intro_FixCksum(sram);
  ZeldaWriteSram();
  ReturnToFileSelect();
  irq_flag = 0xff;
  sound_effect_1 = 0x2c;
}

void NameFile_CheckForScrollInputX() {  // 8cdc8c
  static const uint16 kNameFile_CheckForScrollInputX_Add[2] = {1, 0xff};
  static const int16 kNameFile_CheckForScrollInputX_Cmp[2] = {0x20, 0xff};
  static const int16 kNameFile_CheckForScrollInputX_Set[2] = {0, 0x1f};
  if (joypad1H_last & 3) {
    int k = (joypad1H_last & 3) - 1;
    selectfile_var10 = k;
    selectfile_var9++;
    uint8 t = selectfile_var3 + kNameFile_CheckForScrollInputX_Add[k];
    if (t == kNameFile_CheckForScrollInputX_Cmp[k])
      t = kNameFile_CheckForScrollInputX_Set[k];
    selectfile_var3 = t;
  }
}

void NameFile_CheckForScrollInputY() {  // 8cdcbf
  static const int8 kNameFile_CheckForScrollInputY_Add[2] = {1, -1};
  static const int8 kNameFile_CheckForScrollInputY_Cmp[2] = {4, -1};
  static const int8 kNameFile_CheckForScrollInputY_Set[2] = {0, 3};

  uint8 a = joypad1H_last & 0xc;
  if (a) {
    if ((a * 2 | selectfile_var5) == 0x10 || (a * 4 | selectfile_var5) == 0x13) {
      selectfile_arr2[1] = a;
      return;
    }
     a >>= 2;
    int t = selectfile_var5 + kNameFile_CheckForScrollInputY_Add[a-1];
    if (t == kNameFile_CheckForScrollInputY_Cmp[a-1])
      t = kNameFile_CheckForScrollInputY_Set[a-1];
    selectfile_var5 = t;

    selectfile_var11++;
    selectfile_arr2[1] = a;

  } else {
    selectfile_arr2[0] = 0;
  }
}

void NameFile_DrawSelectedCharacter(int k, uint16 chr) {  // 8cdd30
  static const uint16 kNameFile_DrawSelectedCharacter_Tab[6] = {0x84, 0x86, 0x88, 0x8a, 0x8c, 0x8e};
  uint16 *dst = vram_upload_data;
  uint16 a = kNameFile_DrawSelectedCharacter_Tab[k] | 0x6100;
  dst[0] = swap16(a);
  dst[1] = 0x100;
  dst[2] = 0x1800 | chr;
  dst[3] = swap16(a + 0x20);
  dst[4] = 0x100;
  dst[5] = (0x1800 | chr) + 0x10;
  BYTE(dst[6]) = 0xff;
  nmi_load_bg_from_vram = 1;
}


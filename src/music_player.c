// music_player.c - options-driven background music for zelda3 (new-file
// prototype; nothing existing is modified - integration points are documented
// in tools/MUSIC_INTEGRATION.md).
//
// DESIGN / THREADING
// ------------------
// The engine owns exactly one SDL audio device whose callback (main.c
// AudioCallback) renders SPC samples via ZeldaRenderAudio. Any extra audio
// source must therefore be MIXED into that stream. The existing MSU player
// (src/audio.c MsuPlayer_Mix) does blocking fread + opus decode directly on
// the callback thread while holding g_audio_mutex - it works, but a slow disk
// (or an AV scanner) stalls audio AND the emulated CPU (the emu thread waits
// on the same mutex in ZeldaRunFrame).
//
// This module deliberately does NOT decode on the callback thread. Instead:
//
//   producer thread                    SDL audio callback thread
//   ---------------------------        ---------------------------
//   open next track (files, decode)    MusicPlayer_Mix():
//   convert -> s16 stereo scratch        memcpy out of ring (non-blocking)
//   resample -> output rate              volume scale + saturating add
//   push into bounded ring buffer
//
// One SDL_mutex + one condvar protect the ring (classic bounded buffer; the
// consumer never waits, only the producer waits for space, with a timeout so
// shutdown stays responsive). Tradeoffs vs the alternatives:
//
//   * whole-file preload: zero callback risk, but unbounded memory (a 5 min
//     stereo 44.1kHz 16-bit WAV is ~52 MB; a folder of those would eat RAM).
//   * decode-in-callback (MSU style): no threads, but blocking I/O in the
//     audio path risks glitches and stalls the emu thread via the mutex.
//   * decode thread + ring (chosen): bounded memory (1 MB ring), the callback
//     does a fixed-size copy, and track advance is decided by the producer,
//     which keeps the ring topped up across track boundaries (near-gapless).
//
// FORMATS
//   .wav : decoded natively (PCM u8/s16/s24/s32 + IEEE float32, mono or
//          multichannel - channels beyond the first two are folded into
//          stereo). Sample rates other than the engine rate are resampled
//          with nearest-neighbor duplication/drop (deliberately trivial).
//   .ogg : decoded with stb_vorbis (vendored at third_party/stb/
//          stb_vorbis.c, public domain / MIT). Compiled only when
//          MUSIC_PLAYER_HAVE_VORBIS is defined (build_msvc.cmd does);
//          without it, .ogg files are counted and skipped.
//   .mp3 : decoded with dr_mp3 (vendored at third_party/stb/dr_mp3.h,
//          public domain / MIT-0), on by default; define
//          MUSIC_PLAYER_NO_MP3 to compile it out (then .mp3 files are
//          counted and skipped).
//   All three produce the same s16 interleaved frames, which flow through
//   the shared channel-fold + resample + ring buffer + advance logic.
//
//  A file that fails to OPEN (corrupt/truncated/unsupported) is logged to
//  stderr and skipped at play time - that is the only remaining "skip"
//  behavior; the old scan-time .ogg/.mp3 skip is gone for enabled formats.
//
// NOTE ON INCLUDE ORDER: system/SDL headers come FIRST and no zelda3 header
// is included at all. src/variables.h defines R10/R12/R14 macros that collide
// with winnt.h's CONTEXT struct, so this file must never include zelda3
// headers after (or before) <windows.h>.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL.h>

// Its own header only pulls <stdint.h>/<stdbool.h>, so it is safe here (and
// keeps the prototypes and the definitions below from drifting apart).
#include "music_player.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// Shorthands identical to the engine's src/types.h. Defined locally so this
// module stays fully standalone (types.h is never included here; music_player.h
// itself only uses <stdint.h> names, so no integration-time conflicts).
typedef int16_t int16;
typedef int32_t int32;
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef uint64_t uint64;

// ---------------------------------------------------------------------------
// Optional OGG support (needs MUSIC_PLAYER_HAVE_VORBIS, set by
// build_msvc.cmd): third_party/stb/stb_vorbis.c is vendored there and pulled
// in stb-style (header + implementation in this one translation unit).
// STB_VORBIS_NO_PUSHDATA_API drops the push API we do not use; the stdio /
// pull API IS used (stb_vorbis_open_filename) so stdio stays enabled.
// ---------------------------------------------------------------------------
#ifdef MUSIC_PLAYER_HAVE_VORBIS
#ifndef STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_PUSHDATA_API
#endif
#include "third_party/stb/stb_vorbis.c"
#endif

// ---------------------------------------------------------------------------
// MP3 support (dr_mp3, vendored at third_party/stb/dr_mp3.h, public domain /
// MIT-0, from https://github.com/mackron/dr_libs). Enabled by default with
// dr_wav-style single-header integration: the implementation macro is defined
// here so this translation unit is the only place the implementation lands.
// Define MUSIC_PLAYER_NO_MP3 to compile it out.
// ---------------------------------------------------------------------------
#ifndef MUSIC_PLAYER_NO_MP3
#define DR_MP3_IMPLEMENTATION
#include "third_party/stb/dr_mp3.h"
#endif

enum {
  kMusicRingFrames = 1 << 17,        // 131072 frames (~3 s @44.1kHz, 512 KB stereo s16)
  kMusicChunkFrames = 2048,          // file frames decoded per producer iteration
  kMusicMaxBytesPerFrame = 32,       // up to 8ch * 32bit
  kMusicMaxTracks = 4096,
};

// ---------------------------------------------------------------- buckets --
// "Sounds area linked" (the ZALiA / Zelda 2 pool model): the folder holds one
// subfolder per game context, and the vanilla music request the game sends to
// the APU picks which one plays. The loose files in the folder ROOT are the
// fallback pool, used whenever a bucket folder is missing or empty.
enum {
  kMusicBucket_Title, kMusicBucket_Overworld, kMusicBucket_Darkworld,
  kMusicBucket_Dungeon, kMusicBucket_Boss, kMusicBucket_Town,
  kMusicBucket_Cave, kMusicBucket_Ending,
  kMusicBucket_Count,
  kMusicPool_Root = kMusicBucket_Count,   // loose files in the folder root
  kMusicPool_Count
};

static const char *const kMusicBucketDirs[kMusicBucket_Count] = {
  "title", "overworld", "darkworld", "dungeon", "boss", "town", "cave", "ending",
};

// Vanilla ALTTP music request -> bucket. These are the byte values the game
// writes to APUI00 (music_control in src/nmi.c); they are the same 1..32
// numbering an MSU-1 pack uses for its .pcm files, which is why the engine can
// hand music_control straight to MsuPlayer_Open in src/audio.c.
// -1 = never taken over: the short fanfares and stings (pedestal pull, heart
// container, Zelda rescued, the mirror warp...) stay as the SNES wrote them,
// because swapping a three second jingle for a two minute ogg reads as a bug.
#define B_(x) (signed char)kMusicBucket_##x
static const signed char kMusicBuckets[] = {
  -1,             // 00  no change (never reaches us)
  B_(Title),      // 01  title screen
  B_(Overworld),  // 02  light world overworld
  B_(Title),      // 03  beginning / rainy intro
  B_(Darkworld),  // 04  bunny link
  B_(Overworld),  // 05  lost woods
  B_(Title),      // 06  legend of zelda (prologue)
  B_(Town),       // 07  kakariko village
  -1,             // 08  mirror / portal warp (sting)
  B_(Darkworld),  // 09  dark world
  -1,             // 0a  master sword pedestal (fanfare)
  B_(Title),      // 0b  file select
  B_(Overworld),  // 0c  guards summoned (chase)
  B_(Darkworld),  // 0d  dark death mountain
  B_(Town),       // 0e  minigame
  B_(Dungeon),    // 0f  hyrule castle
  B_(Dungeon),    // 10  pendant dungeon
  B_(Cave),       // 11  cave
  -1,             // 12  boss victory / heart container (fanfare)
  B_(Town),       // 13  sanctuary
  B_(Boss),       // 14  boss
  B_(Dungeon),    // 15  crystal dungeon
  -1,             // 16  fortune teller (sting)
  B_(Cave),       // 17  cave 2
  -1,             // 18  zelda rescued (fanfare)
  -1,             // 19  crystal maiden rescued (fanfare)
  B_(Cave),       // 1a  fairy fountain
  B_(Boss),       // 1b  agahnim
  B_(Boss),       // 1c  ganon reveals himself
  B_(Boss),       // 1d  ganon battle
  B_(Ending),     // 1e  triforce room
  B_(Ending),     // 1f  ending
  B_(Ending),     // 20  credits / staff roll
};
#undef B_

typedef struct MusicTrack {
  char *path;
  bool warned_zero_decode;  // set once by the producer thread when this file
                            // opened but decoded no audio (written only by
                            // the producer; the tracks snapshot is stable)
} MusicTrack;

typedef struct MusicPool {
  MusicTrack *tracks;
  int count;
  int cursor;               // last index played (producer thread only)
} MusicPool;

static struct {
  // config (music.ini)
  char folder[1024];
  bool enabled;
  int volume;               // 0..128
  bool shuffle;

  // track list snapshot taken at init: one pool per bucket + the root pool
  MusicPool pools[kMusicPool_Count];
  int total_tracks;
  int mp3_skipped, ogg_skipped;

  // What the game asked for. cur_pool is the RESOLVED pool index (the bucket,
  // or kMusicPool_Root after fallback); -1 = nothing taken over, so the SNES
  // sings and the producer idles. `gen` is bumped whenever the producer must
  // drop what it is doing (pool change, or NEXT TRACK / !skipsong).
  int cur_pool;
  unsigned gen;
  bool resume_flag;         // set when we stop taking over; nmi.c consumes it

  // "NOW PLAYING" plumbing (producer writes under the lock, main thread polls)
  char now_playing[96];
  bool now_playing_new;

  // output format (engine side)
  int out_freq, out_channels;

  // ring buffer (frames; rd/wr are monotonic 64-bit frame counters)
  int16 *ring;              // kMusicRingFrames * out_channels
  volatile uint64 ring_rd, ring_wr;

  // producer thread
  SDL_Thread *thread;
  SDL_mutex *lock;          // guards ring counters, pool/gen, quit flag
  SDL_cond *space;          // signalled when the consumer drains frames
  volatile bool quit;
} g_music;

// ----------------------------------------------------------------- helpers --

static char *MusicStrDup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

static char MusicLower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool MusicHasExtension(const char *name, const char *ext) {
  size_t nl = strlen(name), el = strlen(ext);
  if (nl < el)
    return false;
  for (size_t i = 0; i < el; i++)
    if (MusicLower(name[nl - el + i]) != ext[i])
      return false;
  return true;
}

static int MusicStrCaseCmp(const char *a, const char *b) {
  while (*a && MusicLower(*a) == MusicLower(*b))
    a++, b++;
  return (unsigned char)MusicLower(*a) - (unsigned char)MusicLower(*b);
}

// Compare by filename (after the last path separator), case-insensitive.
static int MusicTrackCompare(const void *pa, const void *pb) {
  const char *a = ((const MusicTrack *)pa)->path;
  const char *b = ((const MusicTrack *)pb)->path;
  const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
  const char *ra = strrchr(sa ? sa : a, '\\'), *rb = strrchr(sb ? sb : b, '\\');
  return MusicStrCaseCmp(ra ? ra + 1 : (sa ? sa + 1 : a), rb ? rb + 1 : (sb ? sb + 1 : b));
}

// ------------------------------------------------------------- folder scan --

static void MusicAddTrack(int pool, const char *folder, const char *name) {
  MusicPool *mp = &g_music.pools[pool];
  if (mp->count >= kMusicMaxTracks)
    return;
  size_t fl = strlen(folder);
  // Join with '/' (accepted by both Win32 and POSIX APIs).
  const char *sep = (fl != 0 && folder[fl - 1] != '/' && folder[fl - 1] != '\\') ? "/" : "";
  char path[1100];
  if (snprintf(path, sizeof(path), "%s%s%s", folder, sep, name) >= (int)sizeof(path))
    return;
  char *dup = MusicStrDup(path);
  if (!dup)
    return;
  MusicTrack *grown = (MusicTrack *)realloc(mp->tracks, sizeof(MusicTrack) * (size_t)(mp->count + 1));
  if (!grown) {
    free(dup);
    return;
  }
  mp->tracks = grown;
  mp->tracks[mp->count].path = dup;
  mp->tracks[mp->count].warned_zero_decode = false;
  mp->count++;
  g_music.total_tracks++;
}

// One directory -> one pool. Still non-recursive; the bucket subfolders are
// simply scanned one after the other, each into its own pool.
static void MusicScanDir(int pool, const char *dir) {
#ifdef _WIN32
  char pattern[1100];
  if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >= (int)sizeof(pattern))
    return;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    const char *name = fd.cFileName;
    if (MusicHasExtension(name, ".wav"))
      MusicAddTrack(pool, dir, name);
    else if (MusicHasExtension(name, ".ogg"))
#ifdef MUSIC_PLAYER_HAVE_VORBIS
      MusicAddTrack(pool, dir, name);
#else
      g_music.ogg_skipped++;   // playable only with MUSIC_PLAYER_HAVE_VORBIS
#endif
    else if (MusicHasExtension(name, ".mp3"))
#ifndef MUSIC_PLAYER_NO_MP3
      MusicAddTrack(pool, dir, name);
#else
      g_music.mp3_skipped++;   // playable unless MUSIC_PLAYER_NO_MP3
#endif
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    char path[1100];
    if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
      continue;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    const char *name = ent->d_name;
    if (MusicHasExtension(name, ".wav"))
      MusicAddTrack(pool, dir, name);
    else if (MusicHasExtension(name, ".ogg"))
#ifdef MUSIC_PLAYER_HAVE_VORBIS
      MusicAddTrack(pool, dir, name);
#else
      g_music.ogg_skipped++;
#endif
    else if (MusicHasExtension(name, ".mp3"))
#ifndef MUSIC_PLAYER_NO_MP3
      MusicAddTrack(pool, dir, name);
#else
      g_music.mp3_skipped++;
#endif
  }
  closedir(d);
#endif
  MusicPool *mp = &g_music.pools[pool];
  if (mp->count > 1)
    qsort(mp->tracks, (size_t)mp->count, sizeof(MusicTrack), MusicTrackCompare);
  mp->cursor = -1;
}

static void MusicScanFolder(void) {
  for (int i = 0; i < kMusicPool_Count; i++)
    g_music.pools[i].cursor = -1;
  MusicScanDir(kMusicPool_Root, g_music.folder);
  for (int b = 0; b < kMusicBucket_Count; b++) {
    char dir[1100];
    if (snprintf(dir, sizeof(dir), "%s/%s", g_music.folder, kMusicBucketDirs[b]) >= (int)sizeof(dir))
      continue;
    MusicScanDir(b, dir);
  }
}

// bucket -> the pool that actually has files (the bucket, else the root pool,
// else -1 meaning "we have nothing for this, let the SNES play").
static int MusicResolvePool(int bucket) {
  if (bucket >= 0 && bucket < kMusicBucket_Count && g_music.pools[bucket].count > 0)
    return bucket;
  if (g_music.pools[kMusicPool_Root].count > 0)
    return kMusicPool_Root;
  return -1;
}

// ------------------------------------------------------------------ config --

// Trims whitespace, strips an optional layer of quotes. Returns s.
static char *MusicTrim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
    *--e = 0;
  if (e > s + 1 && ((s[0] == '"' && e[-1] == '"') || (s[0] == '\'' && e[-1] == '\''))) {
    s++;
    *--e = 0;
  }
  return s;
}

static void MusicLoadConfig(void) {
  strcpy(g_music.folder, "music");
  g_music.enabled = false;
  g_music.volume = SDL_MIX_MAXVOLUME / 2;  // default 64
  g_music.shuffle = false;

  FILE *f = fopen("music.ini", "r");
  if (!f) {
    printf("[music] no music.ini - music player idle\n");
    return;
  }
  g_music.enabled = true;  // presence of the file is the opt-in
  char line[1200];
  while (fgets(line, sizeof(line), f)) {
    char *s = line;
    while (*s == ' ' || *s == '\t')
      s++;
    if (*s == '#' || *s == ';' || *s == '\n' || *s == '\r' || *s == 0)
      continue;
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = 0;
    char *key = MusicTrim(s);
    char *val = MusicTrim(eq + 1);
    if (MusicStrCaseCmp(key, "folder") == 0) {
      if (*val)
        snprintf(g_music.folder, sizeof(g_music.folder), "%s", val);
    } else if (MusicStrCaseCmp(key, "enabled") == 0) {
      g_music.enabled = atoi(val) != 0;
    } else if (MusicStrCaseCmp(key, "volume") == 0) {
      int v = atoi(val);
      g_music.volume = v < 0 ? 0 : v > SDL_MIX_MAXVOLUME ? SDL_MIX_MAXVOLUME : v;
    } else if (MusicStrCaseCmp(key, "shuffle") == 0) {
      g_music.shuffle = atoi(val) != 0;
    }
  }
  fclose(f);

  // Strip trailing slashes (keep drive roots like "C:\").
  size_t fl = strlen(g_music.folder);
  while (fl > 3 && (g_music.folder[fl - 1] == '/' || g_music.folder[fl - 1] == '\\'))
    g_music.folder[--fl] = 0;
}

// --------------------------------------------------------------- wav decode --

typedef struct WavInfo {
  FILE *f;
  int format_tag;    // 1 = PCM, 3 = float
  int channels;
  int sample_rate;
  int bits;
  int bytes_per_frame;
  long data_offs;
  uint32 data_bytes;
} WavInfo;

// Reads a little-endian value of 1..4 bytes.
static uint32 MusicReadLe(const uint8 *p, int n) {
  uint32 v = 0;
  for (int i = n - 1; i >= 0; i--)
    v = (v << 8) | p[i];
  return v;
}

static bool MusicWavOpen(WavInfo *w, const char *path) {
  memset(w, 0, sizeof(*w));
  w->f = fopen(path, "rb");
  if (!w->f)
    return false;
  uint8 hdr[12];
  if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    goto FAIL;
  bool have_fmt = false;
  for (;;) {
    uint8 ch[8];
    if (fread(ch, 1, 8, w->f) != 8)
      goto FAIL;
    uint32 size = MusicReadLe(ch + 4, 4);
    long next = ftell(w->f) + (long)size + (long)(size & 1);  // chunks are word-aligned
    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8 fmt[40] = { 0 };
      if (size < 16 || fread(fmt, 1, size > (uint32)sizeof(fmt) ? (uint32)sizeof(fmt) : size, w->f) < 16)
        goto FAIL;
      w->format_tag = (int)MusicReadLe(fmt + 0, 2);
      w->channels = (int)MusicReadLe(fmt + 2, 2);
      w->sample_rate = (int)MusicReadLe(fmt + 4, 4);
      w->bits = (int)MusicReadLe(fmt + 14, 2);
      if (w->format_tag == 0xfffe && size >= 40)  // WAVE_FORMAT_EXTENSIBLE
        w->format_tag = (int)MusicReadLe(fmt + 24, 2);  // SubFormat GUID first word
      have_fmt = true;
    } else if (memcmp(ch, "data", 4) == 0) {
      w->data_offs = ftell(w->f);
      if (size == 0) {  // streamed writers lie about size; use the file size
        fseek(w->f, 0, SEEK_END);
        long end = ftell(w->f);
        size = (uint32)(end - w->data_offs);
        fseek(w->f, w->data_offs, SEEK_SET);
      }
      w->data_bytes = size;
      break;
    }
    fseek(w->f, next, SEEK_SET);
  }
  if (!have_fmt || w->data_bytes == 0)
    goto FAIL;
  w->bytes_per_frame = w->channels * (w->bits / 8);
  if (w->channels < 1 || w->channels > 8 || w->bytes_per_frame < 1 ||
      w->bytes_per_frame > kMusicMaxBytesPerFrame || w->sample_rate < 4000 || w->sample_rate > 192000)
    goto FAIL;
  if (fseek(w->f, w->data_offs, SEEK_SET) != 0)
    goto FAIL;
  return true;
FAIL:
  fclose(w->f);
  w->f = NULL;
  return false;
}

// Convert one raw sample to s16 according to the wav format.
static int16 MusicWavSample(const uint8 *p, int format_tag, int bits) {
  switch (format_tag) {
  case 1:  // PCM
    switch (bits) {
    case 8:  return (int16)(((int)p[0] - 128) << 8);
    case 16: return (int16)MusicReadLe(p, 2);
    case 24: {int32 v = (int32)MusicReadLe(p, 3) << 8; v >>= 16; return (int16)v; }
    case 32: {int32 v = (int32)MusicReadLe(p, 4); return (int16)(v >> 16); }
    }
    break;
  case 3:  // IEEE float32
    if (bits == 32) {
      float f;
      uint32 v = MusicReadLe(p, 4);
      memcpy(&f, &v, 4);
      if (f > 32767.0f) f = 32767.0f;
      if (f < -32768.0f) f = -32768.0f;
      return (int16)(f + (f < 0 ? -0.5f : 0.5f));
    }
    break;
  }
  return 0;
}

// Decode up to max_frames file frames into stereo s16 at the file rate.
// Returns frames decoded; 0 on EOF/error (caller advances to the next track).
static int MusicWavDecode(WavInfo *w, int16 *dst, int max_frames) {
  if (w->data_bytes == 0)
    return 0;
  uint8 raw[kMusicChunkFrames * kMusicMaxBytesPerFrame];
  int want = max_frames;
  uint32 bytes_left = w->data_bytes;
  if ((uint32)want * (uint32)w->bytes_per_frame > bytes_left)
    want = (int)(bytes_left / (uint32)w->bytes_per_frame);
  if (want <= 0)
    return 0;
  size_t got = fread(raw, 1, (size_t)want * (size_t)w->bytes_per_frame, w->f);
  int frames = (int)(got / (size_t)w->bytes_per_frame);
  w->data_bytes -= (uint32)(frames * w->bytes_per_frame);
  for (int i = 0; i < frames; i++) {
    const uint8 *p = raw + (size_t)i * (size_t)w->bytes_per_frame;
    int16 l = MusicWavSample(p, w->format_tag, w->bits);
    int16 r = w->channels >= 2 ? MusicWavSample(p + w->bits / 8, w->format_tag, w->bits) : l;
    dst[i * 2 + 0] = l;
    dst[i * 2 + 1] = r;
  }
  return frames;
}

// --------------------------------------------------------------- ogg decode --
// (compiled only when stb_vorbis is vendored; see top of file)
#ifdef MUSIC_PLAYER_HAVE_VORBIS

typedef struct OggInfo {
  stb_vorbis *v;
  int channels, sample_rate;
  short scratch[kMusicChunkFrames * 2];  // interleaved at the ogg's channel count
} OggInfo;

static bool MusicOggOpen(OggInfo *o, const char *path) {
  memset(o, 0, sizeof(*o));
  int err = 0;
  o->v = stb_vorbis_open_filename(path, &err, NULL);
  if (!o->v)
    return false;
  stb_vorbis_info info = stb_vorbis_get_info(o->v);
  o->channels = info.channels;
  o->sample_rate = info.sample_rate;
  if (o->channels < 1 || o->channels > 8 || o->sample_rate < 4000) {
    stb_vorbis_close(o->v);
    o->v = NULL;
    return false;
  }
  return true;
}

static int MusicOggDecode(OggInfo *o, int16 *dst, int max_frames) {
  int frames = stb_vorbis_get_frame_short_interleaved(o->v, o->channels, o->scratch,
                                                      (int)(sizeof(o->scratch) / sizeof(o->scratch[0])));
  // `frames` counts FILE frames at the ogg's channel count, while the
  // interleaved write below consumes frames * o->channels scratch shorts and
  // produces frames * 2 shorts in dst. Clamp to the caller's stereo-frame
  // budget AND to what the scratch actually holds at the file's channel count
  // (for a mono ogg the scratch holds 2x as many FILE frames as stereo frames
  // of dst, so the max_frames clamp alone is not enough to keep the scratch
  // indexing sane).
  int scratch_frames = (int)(sizeof(o->scratch) / sizeof(o->scratch[0])) / (o->channels > 0 ? o->channels : 1);
  if (frames > scratch_frames)
    frames = scratch_frames;
  if (frames > max_frames)
    frames = max_frames;
  for (int i = 0; i < frames; i++) {
    const short *p = o->scratch + (size_t)i * o->channels;
    dst[i * 2 + 0] = p[0];
    dst[i * 2 + 1] = o->channels >= 2 ? p[1] : p[0];
  }
  return frames;
}

#endif  // MUSIC_PLAYER_HAVE_VORBIS

// --------------------------------------------------------------- mp3 decode --
// (compiled unless MUSIC_PLAYER_NO_MP3; see top of file)
#ifndef MUSIC_PLAYER_NO_MP3

typedef struct Mp3Info {
  drmp3 mp3;
  bool open;                                 // true after a successful drmp3_init_file
  int channels, sample_rate;
  drmp3_int16 scratch[kMusicChunkFrames * 2];  // interleaved at the mp3's channel count
} Mp3Info;

static void MusicMp3Close(Mp3Info *m) {
  if (m->open) {
    drmp3_uninit(&m->mp3);
    m->open = false;
  }
  memset(&m->mp3, 0, sizeof(m->mp3));
}

static bool MusicMp3Open(Mp3Info *m, const char *path) {
  memset(m, 0, sizeof(*m));
  if (!drmp3_init_file(&m->mp3, path, NULL))
    return false;
  m->open = true;
  m->channels = (int)m->mp3.channels;
  m->sample_rate = (int)m->mp3.sampleRate;
  if (m->channels < 1 || m->channels > 8 || m->sample_rate < 4000) {
    MusicMp3Close(m);
    return false;
  }
  return true;
}

// Same contract as MusicWavDecode/MusicOggDecode: up to max_frames file
// frames out as stereo s16 (at the file rate); 0 on EOF/error.
static int MusicMp3Decode(Mp3Info *m, int16 *dst, int max_frames) {
  int want = (int)(sizeof(m->scratch) / sizeof(m->scratch[0])) / m->channels;
  if (want > max_frames)
    want = max_frames;
  if (want <= 0)
    return 0;
  uint64 frames = drmp3_read_pcm_frames_s16(&m->mp3, (drmp3_uint64)want, m->scratch);
  if (frames > (drmp3_uint64)max_frames)
    frames = (drmp3_uint64)max_frames;  // defensive; want already clamps
  for (uint64 i = 0; i < frames; i++) {
    const drmp3_int16 *p = m->scratch + (size_t)i * m->channels;
    dst[i * 2 + 0] = p[0];
    dst[i * 2 + 1] = m->channels >= 2 ? p[1] : p[0];
  }
  return (int)frames;
}

#endif  // MUSIC_PLAYER_NO_MP3

// ------------------------------------------------------------- ring buffer --

static uint32 MusicRingAvail(void) {
  return (uint32)(g_music.ring_wr - g_music.ring_rd);
}

// Drop everything still buffered. Called by the producer when the pool
// changes (or the track is skipped) so the switch is heard immediately
// instead of ~3 s later. Caller must NOT hold the lock.
static void MusicRingFlush(void) {
  if (!g_music.lock)
    return;
  SDL_LockMutex(g_music.lock);
  g_music.ring_rd = g_music.ring_wr;
  SDL_CondSignal(g_music.space);
  SDL_UnlockMutex(g_music.lock);
}

// Push `frames` stereo s16 frames (at src_rate) into the ring, resampling to
// the output rate with nearest-neighbor duplication/drop. `pos` is a fixed
// point source position: the integer part indexes the current scratch chunk
// (relative to its start), the fractional part carries across chunks. Waits
// for ring space (in 100 ms slices) unless shutting down.
// Returns false when the caller's generation went stale (pool change / skip /
// shutdown) - the chunk is then abandoned, which is the point.
static bool MusicRingPush(const int16 *src, int frames, int src_rate, uint64 *pos, unsigned my_gen) {
  if (frames <= 0 || g_music.out_freq <= 0)
    return true;
  const int ch = g_music.out_channels;
  const uint64 step = ((uint64)src_rate << 32) / (uint64)g_music.out_freq;
  const uint32 mask = kMusicRingFrames - 1;

  while (!g_music.quit) {
    SDL_LockMutex(g_music.lock);
    if (g_music.gen != my_gen) {
      SDL_UnlockMutex(g_music.lock);
      return false;
    }
    uint32 space = kMusicRingFrames - MusicRingAvail();
    uint32 emitted = 0;
    while (emitted < space && (*pos >> 32) < (uint64)frames) {
      const int16 *s = src + (size_t)(*pos >> 32) * 2;
      int16 *d = g_music.ring + (size_t)(g_music.ring_wr & mask) * ch;
      if (ch == 2) {
        d[0] = s[0];
        d[1] = s[1];
      } else {
        d[0] = (int16)(((int32)s[0] + s[1]) >> 1);
      }
      g_music.ring_wr++;
      *pos += step;
      emitted++;
    }
    bool chunk_done = (*pos >> 32) >= (uint64)frames;
    if (!chunk_done && emitted == 0) {
      // Ring full. SDL_CondWaitTimeout REQUIRES the mutex to be held on entry
      // (it atomically releases it while sleeping and reacquires it before
      // returning), otherwise it returns immediately on Windows and the loop
      // degenerates into a 100% CPU busy-spin. Sleeping here with the lock
      // released by SDL lets the mixer callback take the lock, drain the ring
      // and signal `space`, so this cannot deadlock.
      SDL_CondWaitTimeout(g_music.space, g_music.lock, 100);
    }
    SDL_UnlockMutex(g_music.lock);
    if (chunk_done)
      break;
  }
  if (g_music.quit)
    return false;
  // Carry the residual position (< 1 source frame) into the next chunk.
  *pos -= (uint64)frames << 32;
  return true;
}

// ---------------------------------------------------------- producer thread --

typedef struct DecoderState {
  int cur_pool;           // pool the open track came from, -1 = none
  int cur_track;          // index into that pool, -1 = none yet
  WavInfo wav;            // for .wav (valid when kind == 1)
#ifdef MUSIC_PLAYER_HAVE_VORBIS
  OggInfo ogg;            // for .ogg (valid when kind == 2)
#endif
#ifndef MUSIC_PLAYER_NO_MP3
  Mp3Info mp3;            // for .mp3 (valid when kind == 3)
#endif
  int kind;               // 0 = none, 1 = wav, 2 = ogg, 3 = mp3
  int src_rate;
  uint64 resample_pos;    // fixed point position into the current chunk
  bool decoded_any;       // false until the current track has yielded > 0 frames
  bool warned_all_failed;
} DecoderState;

static void MusicDecoderClose(DecoderState *ds) {
  if (ds->wav.f) {
    fclose(ds->wav.f);
    ds->wav.f = NULL;
  }
#ifdef MUSIC_PLAYER_HAVE_VORBIS
  if (ds->ogg.v) {
    stb_vorbis_close(ds->ogg.v);
    ds->ogg.v = NULL;
  }
#endif
#ifndef MUSIC_PLAYER_NO_MP3
  MusicMp3Close(&ds->mp3);
#endif
  ds->kind = 0;
  ds->src_rate = 0;
  ds->resample_pos = 0;
  ds->decoded_any = false;
}

static int MusicPickNextIndex(int pool, int cur) {
  int n = g_music.pools[pool].count;
  if (n <= 0)
    return -1;
  if (n == 1)
    return 0;
  if (g_music.shuffle) {
    int next;
    do {
      next = rand() % n;
    } while (next == cur);
    return next;
  }
  return (cur + 1) % n;
}

// "music/boss/ff6_decisive_battle.ogg" -> "FF6 DECISIVE BATTLE". The file
// select font only draws A-Z, 0-9 and space, so everything else folds to a
// space and runs of spaces collapse.
static void MusicMakeNowPlaying(const char *path, char *out, size_t outlen) {
  const char *sa = strrchr(path, '/'), *sb = strrchr(path, '\\');
  const char *base = sa > sb ? sa + 1 : (sb ? sb + 1 : path);
  const char *dot = strrchr(base, '.');
  size_t n = dot ? (size_t)(dot - base) : strlen(base);
  size_t w = 0;
  for (size_t i = 0; i < n && w + 1 < outlen; i++) {
    char c = base[i];
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
    else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      c = ' ';
    if (c == ' ' && (w == 0 || out[w - 1] == ' '))
      continue;
    out[w++] = c;
  }
  while (w > 0 && out[w - 1] == ' ')
    w--;
  out[w] = 0;
}

static void MusicSetNowPlaying(const char *path) {
  char text[96];
  MusicMakeNowPlaying(path, text, sizeof(text));
  SDL_LockMutex(g_music.lock);
  snprintf(g_music.now_playing, sizeof(g_music.now_playing), "%s", text);
  g_music.now_playing_new = true;
  SDL_UnlockMutex(g_music.lock);
}

static void MusicClearNowPlaying(void) {
  SDL_LockMutex(g_music.lock);
  g_music.now_playing[0] = 0;
  g_music.now_playing_new = false;
  SDL_UnlockMutex(g_music.lock);
}

// Open the next playable track from `pool`. Tries at most pool->count entries
// so a folder full of unreadable files cannot spin forever.
static bool MusicOpenNextTrack(DecoderState *ds, int pool) {
  MusicPool *mp = &g_music.pools[pool];
  int n = mp->count;
  if (n <= 0)
    return false;
  for (int attempt = 0; attempt < n; attempt++) {
    int idx = MusicPickNextIndex(pool, mp->cursor);
    const char *path = mp->tracks[idx].path;
    MusicDecoderClose(ds);
    mp->cursor = idx;         // don't retry the same broken file first
    ds->cur_pool = pool;
    ds->cur_track = idx;
    bool ok = false;
    if (MusicHasExtension(path, ".wav")) {
      if (MusicWavOpen(&ds->wav, path)) {
        ds->kind = 1;
        ds->src_rate = ds->wav.sample_rate;
        ok = true;
      }
    } else if (MusicHasExtension(path, ".ogg")) {
#ifdef MUSIC_PLAYER_HAVE_VORBIS
      if (MusicOggOpen(&ds->ogg, path)) {
        ds->kind = 2;
        ds->src_rate = ds->ogg.sample_rate;
        ok = true;
      }
#endif
    } else if (MusicHasExtension(path, ".mp3")) {
#ifndef MUSIC_PLAYER_NO_MP3
      if (MusicMp3Open(&ds->mp3, path)) {
        ds->kind = 3;
        ds->src_rate = ds->mp3.sample_rate;
        ok = true;
      }
#endif
    }
    if (ok) {
      ds->warned_all_failed = false;
      printf("[music] playing %s\n", path);
      MusicSetNowPlaying(path);
      return true;
    }
    fprintf(stderr, "[music] unable to open %s\n", path);
  }
  return false;
}

static int SDLCALL MusicProducerThread(void *arg) {
  DecoderState ds;
  memset(&ds, 0, sizeof(ds));
  ds.cur_track = -1;
  ds.cur_pool = -1;
  (void)arg;
  srand((unsigned)(SDL_GetPerformanceCounter() & 0x7fffffff));

  int16 conv[kMusicChunkFrames * 2];  // stereo s16 at the file's rate
  unsigned my_gen = 0;

  while (!g_music.quit) {
    SDL_LockMutex(g_music.lock);
    unsigned gen = g_music.gen;
    int pool = g_music.cur_pool;
    SDL_UnlockMutex(g_music.lock);

    if (gen != my_gen) {
      // Pool change or NEXT TRACK: abandon the open file and the buffered
      // audio, so the new bucket is heard right away.
      my_gen = gen;
      MusicDecoderClose(&ds);
      MusicRingFlush();
    }
    if (pool < 0 || !g_music.enabled) {
      // Nothing taken over (or the player was switched off): stay quiet and
      // let the SNES tune play. Poll cheaply.
      if (ds.kind != 0) {
        MusicDecoderClose(&ds);
        MusicRingFlush();
      }
      MusicClearNowPlaying();
      SDL_Delay(20);
      continue;
    }
    if (ds.kind == 0) {
      // No decoder open: pick and open the next track from this pool. The
      // decoder then stays open across loop iterations and is only advanced
      // at EOF/error, so a track is not abandoned after a single chunk.
      if (!MusicOpenNextTrack(&ds, pool)) {
        if (!ds.warned_all_failed) {
          ds.warned_all_failed = true;
          fprintf(stderr, "[music] no playable tracks in pool %d - retrying every 30 s\n", pool);
        }
        // Interruptible sleep so Shutdown cannot block on SDL_WaitThread.
        for (int i = 0; i < 300 && !g_music.quit && g_music.gen == my_gen; i++)
          SDL_Delay(100);
        continue;
      }
    }
    int frames = 0;
    if (ds.kind == 1) {
      frames = MusicWavDecode(&ds.wav, conv, kMusicChunkFrames);
    }
#ifdef MUSIC_PLAYER_HAVE_VORBIS
    else if (ds.kind == 2) {
      frames = MusicOggDecode(&ds.ogg, conv, kMusicChunkFrames);
    }
#endif
#ifndef MUSIC_PLAYER_NO_MP3
    else if (ds.kind == 3) {
      frames = MusicMp3Decode(&ds.mp3, conv, kMusicChunkFrames);
    }
#endif
    if (frames <= 0) {
      // End of track (or decode error): close it and open the next one from
      // the SAME pool, so the ring keeps playing across track boundaries.
      bool silent_track = !ds.decoded_any;
      MusicPool *mp = (ds.cur_pool >= 0) ? &g_music.pools[ds.cur_pool] : NULL;
      MusicTrack *tr = (mp && ds.cur_track >= 0 && ds.cur_track < mp->count)
                           ? &mp->tracks[ds.cur_track] : NULL;
      MusicDecoderClose(&ds);  // kind = 0 -> the next iteration opens a new track
      if (silent_track) {
        // The file opened but yielded no audio at all. Back off briefly so a
        // hollow file cannot be reopened at full speed, and warn once per file.
        if (tr != NULL && !tr->warned_zero_decode) {
          tr->warned_zero_decode = true;
          fprintf(stderr, "[music] %s opened but decoded 0 frames - backing off 250 ms\n", tr->path);
        }
        for (int i = 0; i < 25 && !g_music.quit; i++)
          SDL_Delay(10);
      }
      continue;
    }
    ds.decoded_any = true;
    MusicRingPush(conv, frames, ds.src_rate, &ds.resample_pos, my_gen);
  }
  MusicDecoderClose(&ds);
  return 0;
}

// ---------------------------------------------------------------- consumer --

static inline int16 MusicAddClamp(int16 dst, int32 add) {
  int v = dst + add;
  if (v > 32767)
    v = 32767;
  else if (v < -32768)
    v = -32768;
  return (int16)v;
}

static void MusicMixSpan(int16 *dst, const int16 *src, uint32 frames, int ch, int vol) {
  for (uint32 i = 0; i < frames; i++) {
    for (int c = 0; c < ch; c++) {
      int32 s = src[i * (uint32)ch + c];
      dst[i * (uint32)ch + c] = MusicAddClamp(dst[i * (uint32)ch + c], (s * vol) >> 7);
    }
  }
}

// ------------------------------------------------------------------ public --

void MusicPlayer_SetOutputFormat(int freq, int channels) {
  if (freq >= 8000 && freq <= 192000 && (channels == 1 || channels == 2)) {
    g_music.out_freq = freq;
    g_music.out_channels = channels;
  }
}

void MusicPlayer_Init(void) {
  memset(&g_music, 0, sizeof(g_music));
  g_music.out_freq = 44100;   // matches main.c kDefaultFreq; refined via
  g_music.out_channels = 2;   // MusicPlayer_SetOutputFormat(have.freq, have.channels)
  g_music.cur_pool = -1;
  // The lock exists from boot: MusicPlayer_TakeOverTrack runs on the emu
  // thread and can fire long before the first audio callback lazily starts
  // the decode thread.
  g_music.lock = SDL_CreateMutex();
  g_music.space = SDL_CreateCond();
  if (!g_music.lock || !g_music.space) {
    fprintf(stderr, "[music] no mutex - music player disabled\n");
    g_music.enabled = false;
    g_music.quit = true;
    return;
  }
  MusicLoadConfig();
  // Always scan: OPTIONS > AUDIO can switch MUSIC FOLDER on at runtime, and
  // the scan is a snapshot (adding files still needs a restart).
  MusicScanFolder();
  if (!g_music.enabled)
    return;
  printf("[music] folder='%s' tracks=%d volume=%d shuffle=%d", g_music.folder,
         g_music.total_tracks, g_music.volume, g_music.shuffle ? 1 : 0);
  for (int b = 0; b < kMusicBucket_Count; b++)
    if (g_music.pools[b].count)
      printf(" %s=%d", kMusicBucketDirs[b], g_music.pools[b].count);
  if (g_music.pools[kMusicPool_Root].count)
    printf(" root=%d", g_music.pools[kMusicPool_Root].count);
  if (g_music.ogg_skipped)
    printf(" ogg_skipped=%d (no stb_vorbis - see tools/MUSIC_INTEGRATION.md)", g_music.ogg_skipped);
  if (g_music.mp3_skipped)
    printf(" mp3_skipped=%d (mp3 compiled out via MUSIC_PLAYER_NO_MP3)", g_music.mp3_skipped);
  printf("\n");
  // The decode thread is started lazily on the first MusicPlayer_Mix call so
  // it always runs with the engine's real output format.
}

static void MusicStartProducer(void) {
  if (g_music.thread || g_music.quit || !g_music.lock)
    return;
  g_music.ring = (int16 *)malloc(sizeof(int16) * (size_t)kMusicRingFrames * (size_t)g_music.out_channels);
  if (!g_music.ring) {
    fprintf(stderr, "[music] out of memory - music player disabled\n");
    g_music.quit = true;
    return;
  }
  g_music.thread = SDL_CreateThread(MusicProducerThread, "music_player", NULL);
  if (!g_music.thread) {
    fprintf(stderr, "[music] failed to start decode thread - music player disabled\n");
    g_music.quit = true;
  }
}

void MusicPlayer_Mix(int16 *stream, int samples) {
  if (!g_music.enabled || g_music.quit || samples <= 0 || stream == NULL)
    return;
  if (g_music.total_tracks == 0)
    return;
  if (g_music.thread == NULL)
    MusicStartProducer();  // lazy start, runs once
  if (g_music.ring == NULL || g_music.lock == NULL)
    return;

  int ch = g_music.out_channels;
  int vol = g_music.volume;  // 0..128 -> >> 7 scale

  SDL_LockMutex(g_music.lock);
  uint64 avail = g_music.ring_wr - g_music.ring_rd;
  uint32 n = avail < (uint64)samples ? (uint32)avail : (uint32)samples;
  if (n != 0) {
    uint32 mask = kMusicRingFrames - 1;
    uint32 start = (uint32)(g_music.ring_rd & mask);
    uint32 first = kMusicRingFrames - start;
    if (first > n)
      first = n;
    const int16 *src = g_music.ring + (size_t)start * ch;
    MusicMixSpan(stream, src, first, ch, vol);
    if (n > first)
      MusicMixSpan(stream + first * ch, g_music.ring, n - first, ch, vol);
    g_music.ring_rd += n;
    SDL_CondSignal(g_music.space);
  }
  SDL_UnlockMutex(g_music.lock);
  // Frames beyond n are left untouched: if the producer fell behind the game's
  // own audio simply plays alone instead of glitching.
}

// ------------------------------------------------------- bucket / takeover --

bool MusicPlayer_TakeOverTrack(uint8_t track) {
  if (!g_music.enabled || g_music.quit || g_music.total_tracks == 0 || !g_music.lock)
    return false;
  if (track == 0 || track >= 0xf0)
    return false;              // 0 = no change, f0..f3 = pause / volume fades
  int bucket = (track < sizeof(kMusicBuckets)) ? kMusicBuckets[track] : -1;
  if (bucket < 0)
    return false;              // fanfares and anything we do not map
  int pool = MusicResolvePool(bucket);
  if (pool < 0)
    return false;              // no bucket folder AND no root pool: SNES plays
  SDL_LockMutex(g_music.lock);
  if (g_music.cur_pool != pool) {
    g_music.cur_pool = pool;
    g_music.gen++;             // producer drops the old file + ring
  }
  SDL_UnlockMutex(g_music.lock);
  return true;
}

bool MusicPlayer_TakeResumeFlag(void) {
  if (!g_music.lock)
    return false;
  SDL_LockMutex(g_music.lock);
  bool v = g_music.resume_flag;
  g_music.resume_flag = false;
  SDL_UnlockMutex(g_music.lock);
  return v;
}

// Stop taking over and ask nmi.c to hand the SPC its track back.
static void MusicReleaseToSnes(void) {
  SDL_LockMutex(g_music.lock);
  if (g_music.cur_pool >= 0) {
    g_music.cur_pool = -1;
    g_music.gen++;
    g_music.resume_flag = true;
  }
  SDL_UnlockMutex(g_music.lock);
}

// ------------------------------------------------------------ live controls --

int MusicPlayer_GetEnabled(void) { return g_music.enabled ? 1 : 0; }
int MusicPlayer_GetVolume(void) { return g_music.volume; }
int MusicPlayer_GetShuffle(void) { return g_music.shuffle ? 1 : 0; }
int MusicPlayer_TrackCount(void) { return g_music.total_tracks; }

void MusicPlayer_SetEnabled(int on) {
  bool want = on != 0;
  if (want == g_music.enabled)
    return;
  g_music.enabled = want;
  if (!want)
    MusicReleaseToSnes();      // unpause the SPC on the next NMI
  // Turning it ON does nothing until the game's next music request reaches
  // MusicPlayer_TakeOverTrack (i.e. the next screen/area change).
  MusicPlayer_SaveConfig();
}

void MusicPlayer_SetVolume(int v) {
  g_music.volume = v < 0 ? 0 : v > SDL_MIX_MAXVOLUME ? SDL_MIX_MAXVOLUME : v;
  MusicPlayer_SaveConfig();
}

void MusicPlayer_SetShuffle(int on) {
  g_music.shuffle = on != 0;
  MusicPlayer_SaveConfig();
}

void MusicPlayer_Skip(void) {
  if (!g_music.lock)
    return;
  SDL_LockMutex(g_music.lock);
  if (g_music.cur_pool >= 0)
    g_music.gen++;             // same pool, next file
  SDL_UnlockMutex(g_music.lock);
}

bool MusicPlayer_SaveConfig(void) {
  FILE *f = fopen("music.ini", "w");
  if (!f)
    return false;
  fprintf(f,
    "# Written by the game: OPTIONS > AUDIO on the player select.\n"
    "# folder/ holds the pools: title overworld darkworld dungeon boss town\n"
    "# cave ending. Loose files in folder/ are the fallback pool.\n"
    "folder=%s\n"
    "enabled=%d\n"
    "volume=%d\n"
    "shuffle=%d\n",
    g_music.folder, g_music.enabled ? 1 : 0, g_music.volume, g_music.shuffle ? 1 : 0);
  fclose(f);
  return true;
}

bool MusicPlayer_TakeNowPlaying(char *buf, int buflen) {
  if (!g_music.lock || !buf || buflen <= 0)
    return false;
  bool v;
  SDL_LockMutex(g_music.lock);
  v = g_music.now_playing_new && g_music.now_playing[0] != 0;
  if (v)
    snprintf(buf, (size_t)buflen, "%s", g_music.now_playing);
  g_music.now_playing_new = false;
  SDL_UnlockMutex(g_music.lock);
  return v;
}

bool MusicPlayer_NowPlaying(char *buf, int buflen) {
  if (!g_music.lock || !buf || buflen <= 0)
    return false;
  SDL_LockMutex(g_music.lock);
  bool v = g_music.now_playing[0] != 0;
  if (v)
    snprintf(buf, (size_t)buflen, "%s", g_music.now_playing);
  SDL_UnlockMutex(g_music.lock);
  return v;
}

void MusicPlayer_Shutdown(void) {
  if (g_music.lock) {
    SDL_LockMutex(g_music.lock);
    g_music.quit = true;
    SDL_CondSignal(g_music.space);
    SDL_UnlockMutex(g_music.lock);
  }
  if (g_music.thread) {
    SDL_WaitThread(g_music.thread, NULL);
    g_music.thread = NULL;
  }
  if (g_music.lock) {
    SDL_DestroyMutex(g_music.lock);
    g_music.lock = NULL;
  }
  if (g_music.space) {
    SDL_DestroyCond(g_music.space);
    g_music.space = NULL;
  }
  free(g_music.ring);
  g_music.ring = NULL;
  for (int p = 0; p < kMusicPool_Count; p++) {
    for (int i = 0; i < g_music.pools[p].count; i++)
      free(g_music.pools[p].tracks[i].path);
    free(g_music.pools[p].tracks);
    g_music.pools[p].tracks = NULL;
    g_music.pools[p].count = 0;
  }
  g_music.total_tracks = 0;
}

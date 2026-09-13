// music_player.h - options-driven background music for zelda3.
//
// Prototype for "lane's music feature": an options-set custom folder path; the
// game just plays whatever audio files are present in that folder. No jukebox
// UI, no in-game menu - config driven only.
//
// music.ini (read from the working directory, same style as twitch_config.txt):
//   folder=music      ; folder scanned non-recursively for .wav/.ogg/.mp3
//   enabled=1         ; 0/1 master switch (music.ini present implies enabled=1)
//   volume=64         ; 0-128, default SDL_MIX_MAXVOLUME/2
//   shuffle=0         ; 0 = sequential (sorted by filename), 1 = random
//
// FOLDER LAYOUT (the "sounds area linked" model, same idea as the ZALiA/Z2
// pools). Inside `folder` these OPTIONAL subfolders are each their own pool:
//
//   music/title      music/overworld   music/darkworld   music/dungeon
//   music/boss       music/town        music/cave        music/ending
//   music/*.ogg      <- the loose files in the root are the FALLBACK pool
//
// The player watches the vanilla music request the game sends to the APU
// (music_control in src/nmi.c, i.e. the MSU-1 track numbers 1..32) and maps it
// to one of those buckets (kMusicBuckets in music_player.c). When the bucket
// changes it starts a new file from that bucket's pool; while the game keeps
// asking for tracks in the SAME bucket the current file keeps playing across
// screens. A bucket with no files falls back to the root pool; if that is
// empty too the request is NOT taken over and the SNES track plays as normal.
//
// Threading model: a single decode thread decodes/converts/resamples tracks
// into a bounded ring buffer; MusicPlayer_Mix only memcpys out of that ring
// from the SDL audio callback. No file I/O or decoding ever happens on the
// callback thread. See tools/MUSIC_INTEGRATION.md for the full rationale.

#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads music.ini from the working directory, scans the folder, takes a
// snapshot of the track list. Never fails hard: with no music.ini or an
// empty/bad folder the player is simply idle. The decode thread is started
// lazily on the first MusicPlayer_Mix call (so it always inherits the real
// output format set via MusicPlayer_SetOutputFormat).
void MusicPlayer_Init(void);

// Optional. Tells the player the engine's actual output format (the SDL
// "have" spec in main.c after SDL_OpenAudioDevice). Defaults to 44100 Hz
// stereo if never called. Call between MusicPlayer_Init and the point where
// the audio device is unpaused.
void MusicPlayer_SetOutputFormat(int freq, int channels);

// Mix music additively (with saturation) into the engine's audio stream.
// Intended to be called from the engine's audio path (main.c AudioCallback,
// same place ZeldaRenderAudio/MsuPlayer_Mix run). `samples` is FRAMES, i.e.
// interleaved sample-pairs at the output format set above; for the main.c
// callback that is: len / (g_audio_channels * sizeof(int16)).
// Never blocks: if the ring buffer runs dry the remaining frames are left
// untouched (game audio only).
void MusicPlayer_Mix(int16_t *stream, int samples);

// Stops the decode thread and frees everything. Call only after the SDL audio
// device has been closed (see tools/MUSIC_INTEGRATION.md for the ordering).
void MusicPlayer_Shutdown(void);

// --------------------------------------------------------------------------
// Bucket takeover (src/nmi.c Interrupt_NMI_AudioParts_Locked).
// --------------------------------------------------------------------------

// The game is about to send vanilla music track `track` (1..0xef) to the APU.
// Returns true when the folder player has a pool for it and has taken it over
// - the caller must then substitute the engine's "pause spc player" byte 0xf0
// (exactly what the MUSIC=OFF row does) so the SNES tune does not play on top.
// Returns false when the player is off, has nothing for that bucket, or the
// track is a fanfare we deliberately never replace: play the SNES track then.
bool MusicPlayer_TakeOverTrack(uint8_t track);

// True exactly once after the folder player stops taking over (it was turned
// off, or its folder went empty). The caller should re-issue the last real
// music request so the paused SPC starts singing again.
bool MusicPlayer_TakeResumeFlag(void);

// --------------------------------------------------------------------------
// Live controls (OPTIONS > AUDIO rows, src/options.c; !skipsong in twitch.c).
// Enabled / volume / shuffle all apply immediately; the folder SCAN is taken
// once at startup, so adding files to music/ still needs a restart.
// --------------------------------------------------------------------------
int  MusicPlayer_GetEnabled(void);
void MusicPlayer_SetEnabled(int on);
int  MusicPlayer_GetVolume(void);          // 0..128
void MusicPlayer_SetVolume(int v);
int  MusicPlayer_GetShuffle(void);
void MusicPlayer_SetShuffle(int on);
int  MusicPlayer_TrackCount(void);         // total files found across all pools
void MusicPlayer_Skip(void);               // next file in the current pool

// Rewrites music.ini next to the exe from the live values. Called by the
// options setters so a menu change survives a restart.
bool MusicPlayer_SaveConfig(void);

// Poll from the main thread (QuickChat_Tick). Returns true ONCE per track
// start and copies the filename without extension into buf (uppercased,
// A-Z 0-9 and space only, which is all the file-select font can draw).
bool MusicPlayer_TakeNowPlaying(char *buf, int buflen);

// Same text, but without consuming the flag - for !nowplaying. False when
// nothing from the folder is playing.
bool MusicPlayer_NowPlaying(char *buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif  // MUSIC_PLAYER_H

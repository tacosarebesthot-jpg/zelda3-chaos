#ifndef ZELDA3_AUDIO_H_
#define ZELDA3_AUDIO_H_

#include "types.h"

// Things for msu
bool ZeldaIsPlayingMusicTrack(uint8 track);
bool ZeldaIsPlayingMusicTrackWithBug(uint8 track);
void ZeldaPlayMsuAudioTrack(uint8 track);
bool ZeldaIsMusicPlaying();

void ZeldaEnableMsu(uint8 enable);

void ZeldaRenderAudio(int16 *audio_buffer, int samples, int channels);
void ZeldaDiscardUnusedAudioFrames();
void ZeldaRestoreMusicAfterLoad_Locked(bool is_reset);
void ZeldaSaveMusicStateToRam_Locked();
void ZeldaPushApuState();

// SFX SHUFFLE (the "shuffle_sfx" row): a per-file seeded permutation of the
// low 6 bits (the actual sound-effect id - the top two bits are pan/priority)
// of a sound_effect_1 / sound_effect_2 byte, applied ONLY at the funnel where
// these are turned into APU commands (nmi.c's Interrupt_NMI_AudioParts_Locked)
// - see the big comment above AudioSfx_Remap1 in audio.c.
uint8 AudioSfx_Remap1(uint8 v);
uint8 AudioSfx_Remap2(uint8 v);

#endif  // ZELDA3_AUDIO_H_

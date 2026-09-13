#!/usr/bin/env python3
"""Generate OGG/MP3 test fixtures + music.ini for the zelda3 music_player.

Companion to tools/music_test.py (which covers the WAV paths). This one
creates, by default under <repo root>/music_test/:

  mp3_tone.mp3      - 4 s, 554 Hz sine, stereo 44100 Hz (real MP3)
  ogg_tone.ogg      - 4 s, 440 Hz sine, stereo 44100 Hz (real OGG/Vorbis)
  wav_tone.wav      - 4 s, 659 Hz sine, stereo 44100 Hz (real WAV)
  fake_track.ogg    - 0-byte corrupt file: with MUSIC_PLAYER_HAVE_VORBIS it is
                      now ADDED as a track and must fail at OPEN time with
                      "[music] unable to open ..." on stderr (fallback skip),
                      never at scan time.

and writes music.ini in the repo root (folder=music_test, enabled=1).

Encoding strategy:
  1. ffmpeg, if on PATH (wav -> -c:a libvorbis / libmp3lame)
  2. python soundfile (bundled libsndfile >= 1.1.0 encodes Vorbis AND MP3)

Usage:  python tools/music_ogg_mp3_test.py [--root DIR] [--seconds S]
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import wave

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = "music_test"
SECONDS = 4.0
RATE = 44100


def sine_frames(freq, seconds, rate=RATE):
    import numpy as np
    t = np.arange(int(seconds * rate)) / rate
    env = np.minimum(1.0, np.minimum(t, np.maximum(0.0, seconds - t)))  # 1 s fades
    return (0.5 * 32767 * env * np.sin(2 * np.pi * freq * t)).astype(np.int16)


def write_wav(path, samples, rate=RATE):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        inter = bytearray()
        for s in samples:
            inter += struct.pack("<hh", s, s)
        w.writeframes(bytes(inter))


def encoders_available():
    has_ffmpeg = shutil.which("ffmpeg") is not None
    try:
        import soundfile  # noqa: F401
        has_sf = True
    except ImportError:
        has_sf = False
    return has_ffmpeg, has_sf


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", default=REPO_ROOT)
    p.add_argument("--seconds", type=float, default=SECONDS)
    args = p.parse_args()

    has_ffmpeg, has_sf = encoders_available()
    if not has_ffmpeg and not has_sf:
        sys.exit("need ffmpeg on PATH or 'pip install soundfile numpy'")

    music_dir = os.path.join(args.root, FOLDER)
    os.makedirs(music_dir, exist_ok=True)

    # Intermediates: one WAV per tone, encoded to the target formats.
    tmp_wav = os.path.join(music_dir, "_tmp_tone.wav")
    write_wav(tmp_wav, sine_frames(440.0, args.seconds))

    targets = [
        ("ogg_tone.ogg", 440.0),
        ("mp3_tone.mp3", 554.0),
        ("wav_tone.wav", 659.0),
    ]
    for name, freq in targets:
        out = os.path.join(music_dir, name)
        if has_ffmpeg:
            if name.endswith(".ogg"):
                cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", tmp_wav, "-c:a", "libvorbis", out]
            elif name.endswith(".mp3"):
                cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", tmp_wav, "-c:a", "libmp3lame", out]
            else:
                shutil.copyfile(tmp_wav, out)
        else:
            import numpy as np
            import soundfile as sf
            data = np.column_stack([sine_frames(freq, args.seconds)] * 2).astype(np.int16)
            if name.endswith(".ogg"):
                sf.write(out, data, RATE, format="OGG", subtype="VORBIS")
            elif name.endswith(".mp3"):
                sf.write(out, data, RATE, format="MP3", subtype="MPEG_LAYER_III")
            else:
                sf.write(out, data, RATE, format="WAV", subtype="PCM_16")
        print("wrote %s (%d bytes)" % (out, os.path.getsize(out)))

    os.remove(tmp_wav)

    fake = os.path.join(music_dir, "fake_track.ogg")
    with open(fake, "wb") as f:
        pass
    print("wrote %s (0-byte corrupt file - must fail at OPEN time, not scan time)" % fake)

    ini = os.path.join(args.root, "music.ini")
    with open(ini, "w") as f:
        f.write("folder=%s\nenabled=1\nvolume=64\nshuffle=0\n" % FOLDER)
    print("wrote %s" % ini)
    print("\nRun zelda3.exe from the repo root for ~15 s; expect '[music] playing' for")
    print("ogg_tone.ogg / mp3_tone.mp3 / wav_tone.wav and ONE 'unable to open' for")
    print("fake_track.ogg (fallback skip).")


if __name__ == "__main__":
    main()

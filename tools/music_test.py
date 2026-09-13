#!/usr/bin/env python3
"""Generate a test music folder + music.ini for the zelda3 music_player prototype.

Creates (by default, relative to the repo root = parent of tools/):
  music_test/sine_440_hz.wav   - 5 s, stereo, 44100 Hz, 16-bit
  music_test/sine_554_hz.wav   - 5 s, MONO,   22050 Hz, 16-bit  (exercises the
                                  nearest-neighbor resampler + mono->stereo)
  music_test/sine_659_hz.wav   - 5 s, stereo, 44100 Hz, 16-bit
  music_test/fake_track.ogg    - 0-byte placeholder (shows the ogg_skipped
                                  counter, since stb_vorbis is not vendored)
  music.ini                    - config read by the game from its working dir

Usage:
  python tools/music_test.py            # writes music.ini + music_test/ in repo root
  python tools/music_test.py --ini ..   # custom music.ini directory
  python tools/music_test.py --help
"""

import argparse
import math
import os
import struct
import wave

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FOLDER = "music_test"
DEFAULT_SECONDS = 5.0


def write_sine_wav(path, freq, seconds, rate=44100, channels=2, amplitude=0.5):
    nframes = int(seconds * rate)
    peak = int(32767 * amplitude)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)  # 16-bit
        w.setframerate(rate)
        frames = bytearray()
        two_pi_f = 2.0 * math.pi * freq
        for i in range(nframes):
            # 1 s fade-in / fade-out so track changes are audible, not clicky
            t = i / rate
            env = min(1.0, t / 1.0, max(0.0, (seconds - t) / 1.0))
            s = int(peak * env * math.sin(two_pi_f * t))
            for _ in range(channels):
                frames += struct.pack("<h", s)
        w.writeframes(bytes(frames))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--root", default=REPO_ROOT,
                   help="directory that receives music.ini and the test folder (default: repo root)")
    p.add_argument("--folder", default=DEFAULT_FOLDER,
                   help="name of the test music folder (default: %(default)s)")
    p.add_argument("--seconds", type=float, default=DEFAULT_SECONDS)
    p.add_argument("--volume", type=int, default=64,
                   help="volume= line for music.ini, 0-128 (default: %(default)s)")
    p.add_argument("--shuffle", type=int, default=0, choices=(0, 1))
    p.add_argument("--ini-name", default="music.ini")
    args = p.parse_args()

    music_dir = os.path.join(args.root, args.folder)
    os.makedirs(music_dir, exist_ok=True)

    specs = [
        ("sine_440_hz.wav", dict(freq=440.0, rate=44100, channels=2)),
        ("sine_554_hz.wav", dict(freq=554.0, rate=22050, channels=1)),  # resample + mono path
        ("sine_659_hz.wav", dict(freq=659.0, rate=44100, channels=2)),
    ]
    for name, kw in specs:
        out = os.path.join(music_dir, name)
        write_sine_wav(out, kw["freq"], args.seconds, rate=kw["rate"], channels=kw["channels"])
        print("wrote %s (%s Hz, %s Hz, %s ch)" % (out, kw["freq"], kw["rate"], kw["channels"]))

    # Placeholder so the init banner demonstrates the ogg-skipped report path.
    fake = os.path.join(music_dir, "fake_track.ogg")
    with open(fake, "wb") as f:
        pass
    print("wrote %s (0-byte ogg placeholder - should be reported as skipped)" % fake)

    ini = os.path.join(args.root, args.ini_name)
    with open(ini, "w") as f:
        f.write(
            "# music player config for zelda3 (read from the working directory,\n"
            "# same style as twitch_config.txt)\n"
            "# folder: scanned NON-recursively for .wav/.ogg/.mp3\n"
            "# volume: 0-128 (default 64 = SDL_MIX_MAXVOLUME/2)\n"
            "# shuffle: 0 = sequential (sorted by name), 1 = random order\n"
            "folder=%s\n"
            "enabled=1\n"
            "volume=%d\n"
            "shuffle=%d\n" % (args.folder, args.volume, args.shuffle)
        )
    print("wrote %s" % ini)
    print("\nDone. Run zelda3.exe with the repo root as working directory and listen")
    print("for the three tones cycling every ~%g s over the game audio." % args.seconds)


if __name__ == "__main__":
    main()

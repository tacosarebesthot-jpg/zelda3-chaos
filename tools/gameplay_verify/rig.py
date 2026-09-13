#!/usr/bin/env python3
"""win32 rig for in-gameplay verification of zelda3 twitch verbs.

Library only - see run_verify.py for the scenario driver.

What this does:
  * launches zelda3.exe with a CUSTOM ini (passed via the engine's own
    --config flag, so the repo's zelda3.ini is never touched)
  * temporarily swaps twitch_config.txt (the engine reads it relative to
    cwd at boot, there is no flag for it); the original bytes are restored
    in Game.stop() no matter what
  * drives the game with real keyboard input (ctypes SendInput, scancodes)
    aimed at the game window only; every held key is released in finally
  * captures the game window to PNG via PrintWindow(PW_RENDERFULLCONTENT)
  * tails the game's stdout for "RESULT ..." lines emitted by twitch.c

Keyboard mapping (defaults from zelda3.ini [KeyMap] Controls):
  Up/Down/Left/Right = arrow keys, Select = Right Shift, Start = Return,
  A = x, B = z, X = s, Y = a, L = c, R = v
"""
import ctypes
import json
import os
import re
import subprocess
import sys
import time

from collections import deque
from ctypes import (wintypes, byref, c_size_t, c_ushort, c_ulong, c_long,
                    Structure, Union, POINTER)

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# ------------------------------------------------------- rig failure types --

class WalkError(RuntimeError):
    """Position-aware walk never reached its target inside the tolerance
    before the hard timeout (or telemetry died mid-walk)."""


class PositionUnknown(RuntimeError):
    """No probe telemetry available - the state-aware walker refuses to act
    blind (old rig behaviour was exactly this silent fumbling)."""


# parse "key=value" pairs of the game's telemetry lines (rig hardening)
_KV_RX = re.compile(r"(\w+)=(\-?\d+)")
# parse "slot:t=type,g=graphics,s=state,x=..,y=.." entries of RESULT sprites
_SLOT_RX = re.compile(r"(\d+):t=(\d+),g=(\d+),s=(\d+)(?:,x=(\d+),y=(\d+))?")

# ------------------------------------------------- crash-safe config swap --

SIDECAR_SUFFIX = ".rigbak"


def sidecar_path(path):
    return path + SIDECAR_SUFFIX


def config_recover(paths):
    """Idempotent crash recovery, to be called at the very start of any
    harness/preflight BEFORE the originals are read: if a .rigbak sidecar
    still exists for a config, a previous run died between swap and restore
    - put the original bytes back and only then remove the sidecar.
    Returns the list of recovered paths."""
    recovered = []
    for p in paths:
        sc = sidecar_path(p)
        if os.path.exists(sc):
            with open(sc, "rb") as fh:
                data = fh.read()
            with open(p, "wb") as fh:
                fh.write(data)
            try:
                os.remove(sc)
            except OSError:
                pass
            recovered.append(p)
    return recovered


def config_backup(paths, originals=None):
    """Write byte-copy sidecars for every config about to be rewritten.
    originals: optional {path: bytes} (defaults to the current bytes on
    disk).  Returns the list of sidecar paths written."""
    written = []
    for p in paths:
        if originals is not None and p in originals:
            data = originals[p]
            if data is None:
                continue
        elif os.path.exists(p):
            with open(p, "rb") as fh:
                data = fh.read()
        else:
            continue
        with open(sidecar_path(p), "wb") as fh:
            fh.write(data)
        written.append(sidecar_path(p))
    return written


def config_restore(paths):
    """Restore each path that still has a sidecar and remove the sidecar
    only after the clean (byte-for-byte) restore.  Returns {path: status}."""
    status = {}
    for p in paths:
        sc = sidecar_path(p)
        if not os.path.exists(sc):
            status[os.path.basename(p)] = "no sidecar (not swapped)"
            continue
        with open(sc, "rb") as fh:
            data = fh.read()
        with open(p, "wb") as fh:
            fh.write(data)
        os.remove(sc)
        status[os.path.basename(p)] = "restored from sidecar"
    return status

# ------------------------------------------------------------------ input --

# set-1 scancodes for the keys we need (SDL reads scancodes from WM_KEYDOWN,
# and zelda3 maps them through the [KeyMap] Controls table)
SC = {
    "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06,
    "6": 0x07, "7": 0x08, "8": 0x09, "9": 0x0A, "0": 0x0B,
    "-": 0x0C, "=": 0x0D, "backspace": 0x0E,
    "a": 0x1E, "s": 0x1F, "x": 0x2D, "z": 0x2C, "c": 0x2E, "v": 0x2F,
    "m": 0x32,             # KeyMap ModsPage (stream build)
    "up": 0xC8, "down": 0xD0, "left": 0xCB, "right": 0xCD,
    "return": 0x1C, "rshift": 0x36, "esc": 0x01, "space": 0x39,
    "f11": 0x57,           # sprite selector: F11 next / Shift+F11 previous
    "f10": 0x44,           # settings menu toggle (src/settings_menu.c)
    "f12": 0x58,           # settings menu toggle via GetAsyncKeyState (randomizer.c)
}
EXTENDED = {"up", "down", "left", "right"}

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008
INPUT_KEYBOARD = 1

ULONG_PTR = c_size_t


class KEYBDINPUT(Structure):
    _fields_ = [("wVk", c_ushort), ("wScan", c_ushort),
                ("dwFlags", c_ulong), ("time", c_ulong),
                ("dwExtraInfo", ULONG_PTR)]


class MOUSEINPUT(Structure):
    _fields_ = [("dx", c_long), ("dy", c_long), ("mouseData", c_ulong),
                ("dwFlags", c_ulong), ("time", c_ulong),
                ("dwExtraInfo", ULONG_PTR)]


class HARDWAREINPUT(Structure):
    _fields_ = [("uMsg", c_ulong), ("wParamL", c_ushort),
                ("wParamH", c_ushort)]


class _INPUTU(Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("hi", HARDWAREINPUT)]


class INPUT(Structure):
    _fields_ = [("type", c_ulong), ("u", _INPUTU)]


_held = set()  # scancodes currently held - always released by release_all()


def _make_input(scan, up, ext):
    flags = KEYEVENTF_SCANCODE
    if up:
        flags |= KEYEVENTF_KEYUP
    if ext:
        flags |= KEYEVENTF_EXTENDEDKEY
    ki = KEYBDINPUT(0, scan, flags, 0, ULONG_PTR(0))
    inp = INPUT(INPUT_KEYBOARD)
    inp.u.ki = ki
    return inp


def key_down(name):
    scan = SC[name]
    if scan in _held:
        return
    _held.add(scan)
    arr = (INPUT * 1)(_make_input(scan, False, name in EXTENDED))
    user32.SendInput(1, arr, ctypes.sizeof(INPUT))


def key_up(name):
    scan = SC[name]
    if scan not in _held:
        return
    _held.discard(scan)
    arr = (INPUT * 1)(_make_input(scan, True, name in EXTENDED))
    user32.SendInput(1, arr, ctypes.sizeof(INPUT))


def release_all():
    """Release every key this rig is holding. Safe to call repeatedly."""
    for scan in list(_held):
        ext = False
        for name, s in SC.items():
            if s == scan and name in EXTENDED:
                ext = True
        arr = (INPUT * 1)(_make_input(scan, True, ext))
        user32.SendInput(1, arr, ctypes.sizeof(INPUT))
        _held.discard(scan)


def tap(name, hold_s=0.06):
    key_down(name)
    time.sleep(hold_s)
    key_up(name)


# ----------------------------------------------------------------- window --

WINDOW_TITLE = "The Legend of Zelda: A Link to the Past"
PW_RENDERFULLCONTENT = 0x2


def find_window_for_pid(pid, timeout=30.0):
    """Find a visible top-level window belonging to pid whose title contains
    the zelda3 title (DisplayPerfInTitle may append ' | FPS: n')."""
    deadline = time.time() + timeout
    found = []
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid_out = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, byref(pid_out))
        if pid_out.value != pid:
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        found.append((hwnd, buf.value))
        return True

    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(EnumProc(cb), 0)
        for hwnd, title in found:
            if WINDOW_TITLE.lower() in title.lower():
                return hwnd
        time.sleep(0.5)
    return None


def is_foreground(hwnd):
    return user32.GetForegroundWindow() == hwnd


def focus_window(hwnd, attempts=8):
    """Bring hwnd to the foreground; returns True when focused."""
    for i in range(attempts):
        if is_foreground(hwnd):
            return True
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)  # SW_RESTORE
            time.sleep(0.4)
        # brief ALT tap unlocks SetForegroundWindow for our process
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 2, 0)
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.35 + 0.15 * i)
    return is_foreground(hwnd)


# ------------------------------------------------------------- screenshot --

class BITMAPINFOHEADER(Structure):
    _fields_ = [("biSize", c_ulong), ("biWidth", c_long), ("biHeight", c_long),
                ("biPlanes", c_ushort), ("biBitCount", c_ushort),
                ("biCompression", c_ulong), ("biSizeImage", c_ulong),
                ("biXPelsPerMeter", c_long), ("biYPelsPerMeter", c_long),
                ("biClrUsed", c_ulong), ("biClrImportant", c_ulong)]


class BITMAPINFO(Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", c_ulong * 3)]


DIB_RGB_COLORS = 0


def _window_client_box(hwnd):
    """(x, y, w, h) of the client area relative to the window's top-left."""
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    pt = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, byref(pt))
    wrect = wintypes.RECT()
    user32.GetWindowRect(hwnd, byref(wrect))
    return pt.x - wrect.left, pt.y - wrect.top, w, h


def capture_window(hwnd, out_png):
    """Capture the window's client area to a PNG. Returns (path, mean_rgb)
    or raises on failure. Uses PrintWindow(PW_RENDERFULLCONTENT) which works
    for GPU-rendered windows on Win8.1+."""
    from PIL import Image  # local import: only needed for capture/report

    cx, cy, w, h = _window_client_box(hwnd)
    if w <= 0 or h <= 0:
        raise RuntimeError("window has no client area")

    hdc_win = user32.GetWindowDC(hwnd)
    if not hdc_win:
        raise RuntimeError("GetWindowDC failed")
    memdc = gdi32.CreateCompatibleDC(hdc_win)
    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = w
    bmi.bmiHeader.biHeight = -h          # top-down
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = DIB_RGB_COLORS
    bits = POINTER(c_ulong)()
    bmp = gdi32.CreateDIBSection(memdc, byref(bmi), DIB_RGB_COLORS,
                                 byref(bits), None, 0)
    if not bmp or not bits:
        gdi32.DeleteDC(memdc)
        user32.ReleaseDC(hwnd, hdc_win)
        raise RuntimeError("CreateDIBSection failed")
    old = gdi32.SelectObject(memdc, bmp)
    ok = user32.PrintWindow(hwnd, memdc, PW_RENDERFULLCONTENT)
    # read the pixels BEFORE deleting the DIB - the pointer dies with it
    stride = w * 4
    raw = ctypes.string_at(bits, stride * h) if ok else None
    gdi32.SelectObject(memdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(hwnd, hdc_win)
    if not ok:
        raise RuntimeError("PrintWindow failed")
    img = Image.frombuffer("RGB", (w, h), raw, "raw", "BGRX", 0, 1)
    img = img.crop((cx, cy, cx + w, cy + h)) if (cx or cy) else img
    img.save(out_png)
    stat = img.convert("L")
    hist = stat.histogram()
    total = sum(hist)
    mean = sum(i * v for i, v in enumerate(hist)) / max(total, 1)
    return out_png, mean


# ------------------------------------------------------------------ game --

TEST_TWITCH_CFG = """\
# Written by tools/gameplay_verify - the original twitch_config.txt is
# restored by Game.stop(). Enabled=0 means IRC never connects; drop=1 reads
# twitch_drop/*.txt; debug=1 prints the RESULT lines the rig asserts on.
token=
user=
channel=
enabled=0
drop=1
debug=1
test=0
cooldown=600
effect_secs=5
vs_mode=0
"""


class Game:
    """Owns the zelda3.exe process, its stdout log, and the twitch_config.txt
    swap. stop() always restores the original config and kills the game.

    The original bytes are ALSO mirrored to twitch_config.txt.rigbak for the
    duration of the run: if a run dies hard between start() and stop(), the
    next start() picks the real bytes up from the sidecar instead of
    silently treating the test config as the original."""

    SIDECAR_SUFFIX = ".rigbak"

    def __init__(self, base, log_path, ini_relpath):
        self.base = base
        self.exe = os.path.join(base, "zelda3.exe")
        self.cfg_path = os.path.join(base, "twitch_config.txt")
        self.drop_dir = os.path.join(base, "twitch_drop")
        self.log_path = log_path
        self.ini_relpath = ini_relpath
        self.proc = None
        self.log_fh = None
        self.log_pos = 0
        self._saved_cfg = None
        self.hwnd = None
        # telemetry state, stashed by _read_new() for every RESULT probe /
        # RESULT sprites line that flows through (rig hardening 2026-09-10)
        self.last_probe = None        # dict of the newest probe line
        self.last_sprites = None      # dict {f, n, slots:[(i,t,g,s,x,y)]}
        self.probe_track = deque(maxlen=8192)   # rolling probe history
        self.sprite_track = deque(maxlen=512)   # rolling sprites history
        self.probe_wall = None        # wall time of the newest probe
        self.gameplay_gap_log = []    # detected gameplay-left/returned events
        self.last_fire_gate = None    # state gate snapshot of the last
                                      # fire_verb() (grace demo evidence)

    # -- lifecycle ----------------------------------------------------------
    def start(self):
        if not os.path.exists(self.exe):
            raise RuntimeError("zelda3.exe missing - run build_msvc.cmd first")
        os.makedirs(self.drop_dir, exist_ok=True)
        self._clear_drop()

        sidecar = self.cfg_path + self.SIDECAR_SUFFIX
        cur = None
        if os.path.exists(self.cfg_path):
            with open(self.cfg_path, "rb") as fh:
                cur = fh.read()
        if os.path.exists(sidecar):
            with open(sidecar, "rb") as fh:
                self._saved_cfg = fh.read()   # crashed run: real bytes here
        else:
            self._saved_cfg = cur
        with open(self.cfg_path, "w", encoding="utf-8") as fh:
            fh.write(TEST_TWITCH_CFG)
        with open(self.cfg_path, "rb") as fh:
            test_bytes = fh.read()
        if self._saved_cfg is not None and self._saved_cfg != test_bytes:
            with open(sidecar, "wb") as fh:
                fh.write(self._saved_cfg)

        self.log_fh = open(self.log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [self.exe, "--config", self.ini_relpath],
            cwd=self.base, stdout=self.log_fh, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        self.hwnd = find_window_for_pid(self.proc.pid, timeout=30)
        if not self.hwnd:
            raise RuntimeError("game window not found within 30s")
        if not focus_window(self.hwnd):
            raise RuntimeError("could not bring game window to foreground - "
                               "NOT sending any keys")
        return self.hwnd

    def stop(self):
        try:
            release_all()
        except Exception:
            pass
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(5)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
            self.proc = None
        if self.log_fh:
            try:
                self.log_fh.close()
            except Exception:
                pass
            self.log_fh = None
        try:
            self._clear_drop()
        except Exception:
            pass
        if self._saved_cfg is not None:
            with open(self.cfg_path, "wb") as fh:
                fh.write(self._saved_cfg)
            self._saved_cfg = None
            sidecar = self.cfg_path + self.SIDECAR_SUFFIX
            if os.path.exists(sidecar):
                try:
                    os.remove(sidecar)
                except OSError:
                    pass
            print("  [original twitch_config.txt restored]")

    # -- helpers ------------------------------------------------------------
    def _clear_drop(self):
        for f in os.listdir(self.drop_dir):
            if f.lower().endswith(".txt"):
                try:
                    os.remove(os.path.join(self.drop_dir, f))
                except OSError:
                    pass

    def drop(self, line):
        name = os.path.join(self.drop_dir,
                            "rig_%d_%d.txt" % (time.time_ns(), len(line)))
        with open(name, "w", encoding="utf-8") as fh:
            fh.write(line + "\n")

    def log_read_all(self):
        with open(self.log_path, "r", errors="replace") as fh:
            return fh.read()

    def _read_new(self):
        with open(self.log_path, "r", errors="replace") as fh:
            fh.seek(self.log_pos)
            new = fh.read()
            self.log_pos = fh.tell()
        lines = [l for l in new.splitlines() if l.startswith("RESULT")]
        self._stash_telemetry(lines)
        return lines

    # -- telemetry (state-aware rig) ----------------------------------------
    # The game (twitch.c, debug=1) prints every 10th gameplay frame:
    #   RESULT probe x=.. y=.. st=.. ice=.. vx=.. f=.. mod=.. sub=..
    #   RESULT sprites f=.. n=.. 0:t=..,g=..,s=.. ...
    # f = frame_ctr_dbg (monotonic frame id), st = link_player_handler_state,
    # mod/sub = main_module_index/submodule_index.  Every reader below drains
    # through _read_new(), so nothing is parsed twice and wait_for() users
    # keep working unchanged (probe lines never match verb= regexes).

    def _stash_telemetry(self, lines):
        for l in lines:
            if l.startswith("RESULT probe"):
                kv = dict((m.group(1), int(m.group(2)))
                          for m in _KV_RX.finditer(l))
                now = time.time()
                if self.probe_wall is not None and self.probe_track:
                    dt = now - self.probe_wall
                    if dt > 2.0:
                        # gameplay LEFT and came back: a transition, death,
                        # respawn or cutscene happened between the probes
                        self.gameplay_gap_log.append(
                            {"after_f": self.probe_track[-1].get("f", -1),
                             "resumed_f": kv.get("f", -1),
                             "gap_s": round(dt, 2)})
                self.probe_wall = now
                self.last_probe = kv
                self.probe_track.append(kv)
            elif l.startswith("RESULT sprites"):
                kv = dict((m.group(1), int(m.group(2)))
                          for m in _KV_RX.finditer(l))
                slots = []
                for m in _SLOT_RX.finditer(l):
                    slot = [int(m.group(i)) for i in (1, 2, 3, 4)]
                    slot += [int(m.group(i)) if m.group(i) else -1
                             for i in (5, 6)]
                    slots.append(tuple(slot))
                self.last_sprites = {"f": kv.get("f", -1),
                                     "n": kv.get("n", len(slots)),
                                     "slots": slots}
                self.sprite_track.append(self.last_sprites)

    def _drain(self):
        """Consume any new RESULT lines (keeps last_probe fresh)."""
        self._read_new()

    def position(self, max_age_s=None, timeout=10.0):
        """Latest (x, y, probe_dict). Waits up to `timeout` for the first
        probe. Raises PositionUnknown when no telemetry is available at all
        (never silently guess: the state-aware walker refuses to fumble)."""
        deadline = time.time() + timeout
        while self.last_probe is None:
            self._drain()
            if self.last_probe is not None:
                break
            if time.time() >= deadline:
                raise PositionUnknown(
                    "no RESULT probe telemetry seen in %s within %.1fs - "
                    "is the game running with twitch debug=1 in gameplay?"
                    % (self.log_path, timeout))
            time.sleep(0.1)
        p = self.last_probe
        if max_age_s is not None:
            # probes carry only the frame id, not wall time; freshness is
            # judged by waiting for a strictly-newer frame below
            pass
        return p.get("x", -1), p.get("y", -1), p

    def wait_probe_after(self, frame_id, timeout=6.0):
        """Block until a probe with f > frame_id arrives (frame-advance ack).
        Returns the probe dict or None on timeout."""
        deadline = time.time() + timeout
        last_f = frame_id
        while time.time() < deadline:
            self._drain()
            p = self.last_probe
            if p is not None and p.get("f", -1) > last_f:
                return p
            time.sleep(0.03)
        return None

    def state(self):
        """Latest live state snapshot (the RESULT probe dict): frame_id,
        x/y, room/screen, module/sub, velocities, direction, animation,
        incapacitated_timer (grace), handler_state, cape, flags."""
        self._drain()
        return dict(self.last_probe) if self.last_probe else None

    def require_state(self, rooms=None, module=None, sub=0, max_inv=0,
                      handler=(0,), timeout=10.0, what="pre-action"):
        """LIVE pre-action gate: block until the state snapshot satisfies
        every given condition (rooms: dungeon_room_index value(s);
        module: main_module_index value(s) - the engine's gameplay check
        is module 7 (dungeon) or 9 (overworld); max_inv:
        link_incapacitated_timer must be <= this so verbs never fire into
        a hit/respawn; handler: allowed link_player_handler_state values).
        Raises (loud) with the actual state if the gate never opens."""
        mods = None if module is None else (
            (module,) if isinstance(module, int) else tuple(module))
        room_ids = None if rooms is None else (
            (rooms,) if isinstance(rooms, int) else tuple(rooms))
        handlers = None if handler is None else (
            (handler,) if isinstance(handler, int) else tuple(handler))
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            self._drain()
            last = self.last_probe
            if last:
                if room_ids is not None and last.get("r") not in room_ids:
                    ok = False
                elif mods is not None and last.get("mod") not in mods:
                    ok = False
                elif sub is not None and last.get("sub", 0) != sub:
                    ok = False
                elif last.get("inv", 0) > max_inv:
                    ok = False
                elif handlers is not None and last.get("st") not in handlers:
                    ok = False
                else:
                    return dict(last)
            time.sleep(0.05)
        raise PositionUnknown(
            "%s: state gate not satisfied within %.1fs (rooms=%s module=%s "
            "sub=%s max_inv=%s handler=%s); last probe=%s"
            % (what, timeout, room_ids, mods, sub, max_inv, handlers, last))

    def fire_verb(self, drop_line, expect_rx=None, timeout=8.0, grace=True):
        """State-aware verb fire: FIRST waits for the state gate (in
        gameplay: module 7/9, sub 0, incapacitated_timer 0 when
        grace=True - never fire into a respawn), THEN drops the verb and
        waits for its RESULT.  Returns (matched_line_or_None, gate); gate
        is the state snapshot the gate opened on (grace evidence)."""
        gate = self.require_state(module=(7, 9), sub=0,
                                  max_inv=0 if grace else 999,
                                  timeout=10.0,
                                  what="fire %r" % drop_line)
        self.last_fire_gate = gate
        self.drop(drop_line)
        if not expect_rx:
            return None, gate
        line, seen = self.wait_for(
            "%s|verb=\\w+ DROPPED" % expect_rx, timeout)
        if line and "DROPPED" in line:
            return None, gate       # loud enough: caller records the miss
        return line, gate

    def gameplay_gaps(self):
        """Transitions/death/respawn events detected LIVE from the probe
        stream: every >2s hole in gameplay telemetry (gameplay left and
        came back).  [{'after_f', 'resumed_f', 'gap_s'}, ...]"""
        return list(self.gameplay_gap_log)

    def measure_pace(self, seconds=2.0):
        """Measured emulation rate: (f2-f1)/wall between two probes ~seconds
        apart. ~60.0 means the frame-delay pacing is holding; hundreds means
        the run is unlocked (host-load-dependent run lengths)."""
        self._drain()
        p1 = self.last_probe
        if p1 is None:
            p1 = self.position()[2]
        t1 = time.time()
        f1, t1 = p1.get("f", 0), t1
        deadline = time.time() + max(seconds * 4, 6.0)
        while time.time() < deadline:
            time.sleep(0.1)
            self._drain()
            p2 = self.last_probe
            if p2 and p2.get("f", 0) > f1 and time.time() - t1 >= seconds:
                dt = time.time() - t1
                return (p2["f"] - f1) / dt if dt > 0 else 0.0
        return 0.0

    # -- state-aware walking (item 1) ---------------------------------------
    def walk_to(self, x, y, tol=2, timeout=25.0, coarse_px=9,
                pulse_s=0.045, poll_s=0.02, verbose=False):
        """Walk Link to world coords (x, y) within +-tol pixels, state-aware.

        Unlike the old fixed-frame-count holds, this loop re-reads Link's
        position from the game's probe telemetry after EVERY probe frame
        (~10 emu frames) and steers toward the target: holds a direction for
        long distances, then switches to short input pulses evaluated one
        probe-frame apart so the final approach converges inside the
        tolerance.  If the dominant axis is wall-blocked (no position change
        while pushing for ~50 frames), the walker swaps to the cross axis so
        L-shaped routes still resolve; only a genuinely unreachable target
        runs out the clock.

        Inputs are injected live and acknowledged by the NEXT
        probe's frame id - never by sleep-based open-loop timing (item 4).
        Fails LOUDLY (WalkError naming target and last known position)
        rather than silently fumbling when the target is unreachable or the
        telemetry dies.  Returns a dict with the final state.
        """
        self.ensure_focus()
        px, py, p = self.position(timeout=10.0)
        start_f = p.get("f", 0)
        deadline = time.time() + timeout
        held = None
        last_pulse_f = -1
        cur_pulse_s = pulse_s
        samples = 0
        pulses = 0
        stall = 0
        last_pos = (px, py)
        force_other_axis = 0
        force_away = False
        detour_px = 0
        prev_cross = None
        detour_cross_is_y = False
        prev_f = -1
        stale_iters = 0
        no_progress = 0
        while True:
            self._drain()
            p = self.last_probe
            now = time.time()
            if p is None or now >= deadline:
                release_all()
                raise WalkError(
                    "never reached target (x=%d, y=%d): got (x=%d, y=%d) "
                    "after %.1fs, %d probe samples, %d pulses%s"
                    % (x, y, px, py, timeout, samples, pulses,
                       "" if p is not None else " [telemetry died: no probe "
                       "frames - game stalled, paused or left gameplay]"))
            px, py, f = p.get("x", -1), p.get("y", -1), p.get("f", -1)
            samples += 1
            if f != prev_f:
                prev_f, stale_iters = f, 0
            else:
                stale_iters += 1
                if stale_iters > 100:
                    # no new probe frame for ~3s: the game left gameplay
                    # (door, cutscene, death) or stalled - never walk blind
                    release_all()
                    raise WalkError(
                        "telemetry stalled: no probe frame newer than f=%d "
                        "while walking to (x=%d, y=%d); last known position "
                        "(x=%d, y=%d) - game likely left gameplay (door/"
                        "cutscene/death) or stopped"
                        % (f, x, y, px, py))
            # account for movement made during a detour (away) hold
            if prev_cross is not None:
                detour_px += abs((py if detour_cross_is_y else px)
                                 - prev_cross)
                prev_cross = None
            dx, dy = x - px, y - py
            if verbose:
                print("    walk: at (%d,%d) f=%d -> target (%d,%d) d=(%d,%d)"
                      % (px, py, f, x, y, dx, dy))
            if abs(dx) <= tol and abs(dy) <= tol:
                release_all()
                return {"target": (x, y), "reached": (px, py),
                        "err": (abs(dx), abs(dy)), "frames": f - start_f,
                        "samples": samples, "pulses": pulses}
            # dominant axis first (ties -> y, matches the Sanctuary route);
            # a wall-blocked dominant axis yields to the cross axis
            prefer_y = abs(dy) >= abs(dx)
            if force_other_axis > 0:
                prefer_y = not prefer_y
                force_other_axis -= 1
            if prefer_y:
                key, far, cross = ("down" if dy > 0 else "up"), abs(dy), abs(dx)
            else:
                key, far, cross = ("right" if dx > 0 else "left"), abs(dx), abs(dy)
            moved = (px, py) != last_pos
            if moved:
                stall = 0
                no_progress = 0
            else:
                # probes arrive every ~2 frames; with input held and zero
                # movement this is real-time stuck detection (owner scope:
                # "walk input held + delta==0 = BLOCKED"), not a timeout
                if far > coarse_px:
                    stall += 1
                no_progress += 1
                if no_progress >= 30:
                    release_all()
                    raise WalkError(
                        "BLOCKED at (x=%d, y=%d) after ~%d frames of held "
                        "input walking toward (x=%d, y=%d): position never "
                        "changed (wall/NPC/object%s). state at block: %r"
                        % (px, py, no_progress * 2, x, y,
                           ", mid-grace/invulnerable" if p.get("inv", 0) > 0
                           else "", p))
            last_pos = (px, py)
            if stall >= 5:
                # pushed for ~50 frames with zero movement: a wall (or an
                # NPC standing in the way).
                stall = 0
                if force_other_axis > 0:
                    # the forced cross push is blocked too: detour AWAY
                    # from the target on the cross axis to round it
                    force_other_axis = 4
                    force_away = True
                elif cross > tol:
                    force_other_axis = 4       # route along the cross axis
                elif detour_px < 80:
                    force_other_axis = 4       # detour AWAY from the target
                    force_away = True          # to get around the obstacle
            if far > coarse_px:
                # coarse phase: hold the direction while the game runs; the
                # next probe (~10 frames) re-evaluates.  Input injected now
                # is consumed by the game at its next frame boundary.
                hold = key
                if force_away:
                    # detour: move the CROSS axis away from the target so a
                    # wall on the dominant axis can be rounded
                    force_away = False
                    detour_cross_is_y = not prefer_y
                    prev_cross = py if detour_cross_is_y else px
                    if prefer_y:   # dominant y, cross x
                        hold = "left" if dx > 0 else "right"
                    else:          # dominant x, cross y
                        hold = "up" if dy > 0 else "down"
                if held != hold:
                    release_all()
                    key_down(hold)
                    held = hold
                time.sleep(max(poll_s, 0.03))
            else:
                # fine phase: pulse, then WAIT for the probe that proves the
                # pulse's effect before the next pulse (frame-acked stepping)
                if held is not None:
                    release_all()
                    held = None
                if f == last_pulse_f:
                    time.sleep(poll_s)
                    continue
                if now >= deadline:
                    continue
                tap(key, hold_s=cur_pulse_s)
                pulses += 1
                last_pulse_f = p.get("f", -1)
                # a pulse that stopped converging is halved (min 1 frame)
                if pulses > 4 and cur_pulse_s > 0.02:
                    cur_pulse_s = max(0.018, cur_pulse_s * 0.6)

    def ensure_focus(self):
        """Focus guard: never send keys while the game window is not
        foreground. Raises (loud) instead of typing into someone else's app."""
        if self.hwnd and not is_foreground(self.hwnd):
            if not focus_window(self.hwnd):
                release_all()
                raise RuntimeError("lost window focus; refusing key input")

    def walk_via(self, waypoints, tol=2, timeout_per_leg=20.0, **kw):
        """Walk through a chain of waypoints (the closed-loop replacement for
        recorded fixed-frame routes): every leg is a walk_to with its own
        tolerance and loud timeout.  Returns the list of per-leg results."""
        out = []
        for (wx, wy) in waypoints:
            out.append(self.walk_to(wx, wy, tol=tol,
                                    timeout=timeout_per_leg, **kw))
        return out

    # -- sprite slot diff (item 3) ------------------------------------------
    def sprite_slots(self, after_frame=None, timeout=4.0):
        """Latest RESULT sprites dump: {f, n, slots:[(slot,type,graf,state)]}.
        With after_frame, waits for a dump strictly newer than that frame id
        (i.e. sampled AFTER the spawn batch landed)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._drain()
            s = self.last_sprites
            if s is not None and (after_frame is None or s["f"] > after_frame):
                return s
            time.sleep(0.05)
        return None

    def spawn_diff(self, expected_type, before=None, after=None):
        """Diff sprite_type[]/sprite_graphics[] for slots that went live
        between `before` and `after` dumps against the enemy type the verb
        intended.  Slots are (slot, type, graphics, state, x, y).  Returns
        (matches, mismatches) where each entry is (slot, type_seen,
        graphics_seen) / (slot, expected, type_seen, graphics_seen)."""
        b = {(s[0]) for s in before["slots"]} if before else set()
        matches, mismatches = [], []
        for s in after["slots"]:
            slot, typ, graf, state = s[0], s[1], s[2], s[3]
            if slot in b and state != 0:
                continue  # was already live before the batch
            if typ == expected_type:
                matches.append((slot, typ, graf))
            else:
                mismatches.append((slot, expected_type, typ, graf))
        return matches, mismatches

    # -- state dump with monotonic frame id (item 4) ------------------------
    def state_dump(self, path, note=""):
        """JSON snapshot of game state stamped with the game's own monotonic
        frame counter (probe f) so frame drift between runs is detectable -
        wall-clock timestamps alone cannot see emulator-speed drift."""
        self._drain()
        p = self.last_probe or {}
        dump = {
            "iso": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "wall_time": time.time(),
            "frame_id": p.get("f"),           # monotonic, from the game
            "link": {"x": p.get("x"), "y": p.get("y"),
                     "handler_state": p.get("st")},
            "module": p.get("mod"), "submodule": p.get("sub"),
            "sprites": self.last_sprites,
            "note": note,
        }
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(dump, fh, indent=1)
        return dump

    # -- graceful shutdown (item 2: clean exit code) -------------------------
    def close(self, timeout=15.0):
        """Ask the game to quit gracefully (WM_CLOSE -> SDL_QUIT -> main
        returns 0) and return its exit code; None if it would not exit in
        time (caller then hard-kills via stop())."""
        if not self.proc:
            return None
        WM_CLOSE = 0x0010
        try:
            user32.PostMessageW(self.hwnd, WM_CLOSE, 0, 0)
        except Exception:
            pass
        try:
            code = self.proc.wait(timeout)
            self.proc = None
            return code
        except Exception:
            return None

    def results(self, timeout=8.0):
        """Wait until any RESULT line appears (up to timeout); return all new."""
        deadline = time.time() + timeout
        lines = self._read_new()
        while not lines and time.time() < deadline:
            time.sleep(0.15)
            lines = self._read_new()
        return lines

    def wait_for(self, pattern, timeout=15.0):
        """Keep reading until /pattern/ matches a RESULT line or timeout.
        Returns (matched_line_or_None, all_new_lines)."""
        import re
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        seen = []
        while time.time() < deadline:
            seen += self._read_new()
            for l in seen:
                if rx.search(l):
                    return l, seen
            time.sleep(0.15)
        return None, seen

    def wait_quiet(self, timeout=25.0):
        """Wait until all timed effects have reported effect=END."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            pending = 0
            for l in self._read_new():
                pass  # drain
            txt = self.log_read_all()
            import re
            starts = len(re.findall(r"effect=START", txt))
            ends = len(re.findall(r"effect=END", txt))
            pending = starts - ends
            if pending <= 0:
                return True
            time.sleep(0.3)
        return False

    def shot(self, out_dir, name):
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, name + ".png")
        mean = 0.0
        for attempt in range(3):      # PrintWindow can race the swapchain
            path, mean = capture_window(self.hwnd, path)
            if mean >= 2.0:
                break
            time.sleep(0.4)
        rel = os.path.relpath(path, os.path.dirname(out_dir))
        if mean < 4.0:
            print("    [warn] %s looks black (mean=%0.1f)" % (name, mean))
        return path

    def in_gameplay(self):
        """Drop a world-verb probe; it is only applied in real gameplay."""
        self.drop("spawn|keese 1|rig-probe|0")
        line, _ = self.wait_for(r"verb=spawn", 6.0)
        return line is not None and "DROPPED" not in line, line

    def in_gameplay_probe(self, timeout=20.0):
        """Gameplay check with zero side effects: RESULT probe telemetry only
        flows while the game is in real gameplay (and debug=1).  Returns the
        probe dict or None."""
        try:
            return self.position(timeout=timeout)[2]
        except PositionUnknown:
            return None

    def load_chapter(self, chapter, settle=3.0, timeout=30.0, attempts=4):
        """Load reference-save slot N and wait for proof of gameplay via the
        probe telemetry.  Retries the tap while the boot is still initialising
        (an early key press can be swallowed by the title screen).  Returns
        the first gameplay probe dict; raises RuntimeError when the load
        never reached gameplay (loud)."""
        key = loadref_key(chapter)
        marker = "*** Loading slot %d" % (256 + chapter - 1)
        for attempt in range(attempts):
            before = os.path.getsize(self.log_path) \
                if os.path.exists(self.log_path) else 0
            self.ensure_focus()
            tap(key)
            deadline = time.time() + 10.0
            while time.time() < deadline:
                with open(self.log_path, "r", errors="replace") as fh:
                    fh.seek(max(0, before - 64))
                    if marker in fh.read():
                        break
                time.sleep(0.2)
            else:
                continue                     # swallowed - retry the tap
            time.sleep(settle)
            p = self.in_gameplay_probe(timeout=timeout)
            if p is None:
                raise RuntimeError("LoadRef slot %d: no gameplay telemetry "
                                   "within %.0fs" % (chapter, timeout))
            return p
        raise RuntimeError("LoadRef slot %d: no '%s' marker after %d taps"
                           % (chapter, marker, attempts))


def loadref_key(chapter):
    """Keyboard key that loads reference-save slot N (kReferenceSaves order).

    The custom ini maps:  1,2,...,9,0,-,=,Backspace  ->  chapters 1..13
    (mirrors the stock commented-out LoadRef line in zelda3.ini).
    """
    keys = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
            "-", "=", "backspace"]
    if not 1 <= chapter <= len(keys):
        raise ValueError("chapter must be 1..13")
    return keys[chapter - 1]


# enemy ids the twitch spawn verbs can request (twitch.c kSpawnTable +
# cucco 0x0B) - decodes sprite_type[] values in swarm reports.  The ch1
# Sanctuary story NPCs (seen in every ch1 slot dump) are decoded too so
# reports do not mistake them for corruption.
SPAWN_TABLE = {0: "raven", 8: "octorok", 11: "cucco", 111: "keese",
               156: "zoro", 167: "stalfos",
               115: "uncle/priest (room NPC)", 118: "zelda (room NPC)"}


def spawn_name(t):
    return "%s (0x%02X)" % (SPAWN_TABLE.get(t, "?ENEMY?"), t)


def make_verify_ini(repo_ini, out_path, load_ref=True, enable_audio=False,
                    display_perf=True):
    """Write tools/gameplay_verify/zelda3_verify.ini: a copy of the repo's
    zelda3.ini with LoadRef enabled (so key '1' loads saves/ref/Chapter 1)
    and audio off (no audio device on headless runs). display_perf=1 also
    forces DisplayPerfInTitle=1 so the rig can assert the ~60fps frame-delay
    pacing from the window title during smoke tests (item 4: host CPU load
    must not silently unlock emulation speed). The repo ini is never
    modified - the game is launched with --config pointing at this file."""
    with open(repo_ini, "r", encoding="utf-8") as fh:
        lines = fh.readlines()
    out = []
    for line in lines:
        s = line.strip()
        if load_ref and s.startswith("#LoadRef"):
            out.append(line.replace("#LoadRef", "LoadRef", 1))
        elif display_perf and s.startswith("DisplayPerfInTitle"):
            out.append("DisplayPerfInTitle = 1\n")
        elif not enable_audio and s.startswith("EnableAudio"):
            out.append("EnableAudio = 0\n")
        else:
            out.append(line)
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.writelines(out)
    return out_path

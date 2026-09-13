"""Stub for the third-party `bps` pip package (bps-patch format library).

ALttPDoorRandomizer's Rom.py does `import bps.apply` / `import bps.io` at module
import time and raises if missing. Since dump_logic.py never patches a ROM
(--suppress_rom), an inert stub is enough. This keeps the ALttPDoorRandomizer
clone untouched (the real package would have to be pip-installed there).

Only the symbols referenced by the codebase are declared; if any were ever
called they would raise, which is the desired behavior for a logic-only dump.
"""

__all__ = []

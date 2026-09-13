"""Stub of bps.io -- never called in logic-only (suppress_rom) mode."""


def read(*args, **kwargs):
    raise NotImplementedError(
        "bps.io is a stub in randomizer_ref/stubs; ROM patching is disabled "
        "when dumping logic data (use --suppress_rom)."
    )


def write(*args, **kwargs):
    raise NotImplementedError(
        "bps.io is a stub in randomizer_ref/stubs; ROM patching is disabled "
        "when dumping logic data (use --suppress_rom)."
    )

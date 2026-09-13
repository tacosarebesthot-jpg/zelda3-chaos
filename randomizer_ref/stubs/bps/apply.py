"""Stub of bps.apply -- never called in logic-only (suppress_rom) mode."""


def apply_patch(*args, **kwargs):
    raise NotImplementedError(
        "bps.apply is a stub in randomizer_ref/stubs; ROM patching is disabled "
        "when dumping logic data (use --suppress_rom)."
    )


apply = apply_patch

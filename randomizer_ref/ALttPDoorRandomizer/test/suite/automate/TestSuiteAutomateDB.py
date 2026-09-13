"""
Schema and helpers for automated seed generation (TestSuiteAutomate.pyw).

SQLite on Windows is case-insensitive for unquoted identifiers):
  suite_settings  - dimension table keyed by (settings_code, settings_ver)
  suite_runs      - one row per generation attempt

When adding a new generation option that belongs in settings codes:
  1. Extend SETTINGS_COLUMN_SPECS below (column name, SQL type, World attr, bool?, global?).
  2. Ensure BaseClasses.Settings.make_code / adjust_args_from_code encode it.
  3. Run:  python test/suite/automate/TestSuiteAutomate.pyw migrate
     (or `init` on a fresh DB — both call ensure_settings_columns)

See TestSuiteAutomate.pyw: generate by default; init | migrate as subcommands.
"""

from __future__ import annotations

import os
import sqlite3
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

# ---------------------------------------------------------------------------
# Defaults (override via CLI args on the harness / init scripts)
# ---------------------------------------------------------------------------

# This file lives at: <repo>/test/suite/automate/TestSuiteAutomateDB.py
AUTOMATE_DIR = Path(__file__).resolve().parent
REPO_ROOT = AUTOMATE_DIR.parents[2]  # automate -> suite -> test -> repo

# Runtime output/DB location (machine-local; override via --outputpath / --db).
DEFAULT_AUTOMATE_DIR = Path(r"./output")
DEFAULT_DB_PATH = DEFAULT_AUTOMATE_DIR / "log.db"
# Sibling of this module (do not rebuild path from REPO_ROOT + "test/...")
DEFAULT_CUSTOMIZER = AUTOMATE_DIR / "automate_customizer.yaml"

# ---------------------------------------------------------------------------
# settings dimension columns
# (everything Settings.make_code / adjust_args_from_code knows about)
# booleans stored as INTEGER 0/1 for easy AVG()
# ---------------------------------------------------------------------------

# (column_name, sql_type, world_attr_or_None, is_bool, is_global_algo)
# world_attr: attribute name on World; None means special-cased in row builder
SETTINGS_COLUMN_SPECS: List[Tuple[str, str, Optional[str], bool, bool]] = [
    ("settings_code", "TEXT NOT NULL", None, False, False),
    ("settings_ver", "INTEGER NOT NULL", None, False, False),
    # ER / DR
    ("shuffle", "TEXT", "shuffle", False, False),
    ("door_shuffle", "TEXT", "doorShuffle", False, False),
    ("logic", "TEXT", "logic", False, False),
    ("mode", "TEXT", "mode", False, False),
    ("swords", "TEXT", "swords", False, False),
    ("bombbag", "INTEGER", "bombbag", True, False),
    ("goal", "TEXT", "goal", False, False),
    ("difficulty", "TEXT", "difficulty", False, False),
    ("item_functionality", "TEXT", "difficulty_adjustments", False, False),
    ("hints", "INTEGER", "hints", True, False),
    ("shopsanity", "INTEGER", "shopsanity", True, False),
    ("decoupledoors", "INTEGER", "decoupledoors", True, False),
    ("mixed_travel", "TEXT", "mixed_travel", False, False),
    ("standardize_palettes", "TEXT", "standardize_palettes", False, False),
    ("intensity", "TEXT", "intensity", False, False),
    ("shuffletavern", "INTEGER", "shuffletavern", True, False),
    ("dropshuffle", "TEXT", "dropshuffle", False, False),
    ("pottery", "TEXT", "pottery", False, False),
    ("door_self_loops", "INTEGER", "door_self_loops", True, False),
    ("crystals_gt", "TEXT", "crystals_gt_orig", False, False),
    ("dungeon_counters", "TEXT", "dungeon_counters", False, False),
    ("experimental", "INTEGER", "experimental", True, False),
    ("shufflelinks", "INTEGER", "shufflelinks", True, False),
    ("crystals_ganon", "TEXT", "crystals_ganon_orig", False, False),
    ("openpyramid", "TEXT", "open_pyramid", False, False),
    ("accessibility", "TEXT", "accessibility", False, False),
    ("mapshuffle", "TEXT", "mapshuffle", False, False),
    ("compassshuffle", "TEXT", "compassshuffle", False, False),
    ("keyshuffle", "TEXT", "keyshuffle", False, False),
    ("bigkeyshuffle", "TEXT", "bigkeyshuffle", False, False),
    ("enemy_health", "TEXT", "enemy_health", False, False),
    ("enemy_damage", "TEXT", "enemy_damage", False, False),
    ("potshuffle", "INTEGER", "potshuffle", True, False),
    ("enemy_shuffle", "TEXT", "enemy_shuffle", False, False),
    ("restrict_boss_items", "TEXT", "restrict_boss_items", False, False),
    ("algorithm", "TEXT", "algorithm", False, True),
    ("boss_shuffle", "TEXT", "boss_shuffle", False, False),
    ("ow_parallel", "INTEGER", "owParallel", True, False),
    ("ow_layout", "TEXT", "owLayout", False, False),
    ("ow_terrain", "INTEGER", "owTerrain", True, False),
    ("ow_whirlpool", "INTEGER", "owWhirlpoolShuffle", True, False),
    ("ow_crossed", "TEXT", "owCrossed", False, False),
    ("ow_keepsimilar", "INTEGER", "owKeepSimilar", True, False),
    ("ow_mixed", "INTEGER", "owMixed", True, False),
    ("bonk_drops", "INTEGER", "shuffle_bonk_drops", True, False),
    ("shuffle_followers", "INTEGER", "shuffle_followers", True, False),
    ("ow_fluteshuffle", "TEXT", "owFluteShuffle", False, False),
    ("ow_fog", "INTEGER", "owFog", True, False),
    ("flute_mode", "TEXT", "flute_mode", False, False),
    ("bow_mode", "TEXT", "bow_mode", False, False),
    ("take_any", "TEXT", "take_any", False, False),
    ("prizeshuffle", "TEXT", "prizeshuffle", False, False),
    ("pseudoboots", "INTEGER", "pseudoboots", True, False),
    ("overworld_map", "TEXT", "overworld_map", False, False),
    ("trap_door_mode", "TEXT", "trap_door_mode", False, False),
    ("key_logic_algorithm", "TEXT", "key_logic_algorithm", False, False),
    ("skullwoods", "TEXT", "skullwoods", False, False),
    ("linked_drops", "TEXT", "linked_drops", False, False),
    ("mirrorscroll", "INTEGER", "mirrorscroll", True, False),
    ("door_type_mode", "TEXT", "door_type_mode", False, False),
]

SETTINGS_VALUE_COLUMNS = [
    name for name, _t, attr, _b, _g in SETTINGS_COLUMN_SPECS
    if name not in ("settings_code", "settings_ver")
]


SETTINGS_TABLE = "suite_settings"
RUNS_TABLE = "suite_runs"


def _settings_table_sql() -> str:
    cols = []
    for name, sql_type, _attr, _is_bool, _is_global in SETTINGS_COLUMN_SPECS:
        cols.append(f"    {name} {sql_type}")
    cols_sql = ",\n".join(cols)
    return f"""
CREATE TABLE IF NOT EXISTS {SETTINGS_TABLE} (
{cols_sql},
    PRIMARY KEY (settings_code, settings_ver)
);
""".strip()


RUNS_TABLE_SQL = f"""
CREATE TABLE IF NOT EXISTS {RUNS_TABLE} (
    id              INTEGER PRIMARY KEY,
    seed            INTEGER,
    mystery_name    TEXT,
    suite           TEXT,
    settings_code   TEXT NOT NULL,
    settings_ver    INTEGER NOT NULL,
    app_version     TEXT NOT NULL,
    timestamp       INTEGER NOT NULL,
    success         INTEGER NOT NULL,
    duration_ms     INTEGER,
    stage           TEXT,
    error_type      TEXT,
    log             TEXT,
    FOREIGN KEY (settings_code, settings_ver)
        REFERENCES {SETTINGS_TABLE}(settings_code, settings_ver)
);
""".strip()

INDEX_SQL = [
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_success ON {RUNS_TABLE}(success);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_time ON {RUNS_TABLE}(timestamp);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_code ON {RUNS_TABLE}(settings_code, settings_ver);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_settings_door ON {SETTINGS_TABLE}(door_shuffle, intensity);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_settings_ow ON {SETTINGS_TABLE}(ow_layout, ow_crossed, ow_mixed);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_settings_er ON {SETTINGS_TABLE}(shuffle);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_settings_mode ON {SETTINGS_TABLE}(mode, logic);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_suite ON {RUNS_TABLE}(suite);",
    # Explorer filters: suite + time / outcome / classification
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_suite_time ON {RUNS_TABLE}(suite, timestamp);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_suite_success ON {RUNS_TABLE}(suite, success);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_suite_error ON {RUNS_TABLE}(suite, success, error_type);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_error_time ON {RUNS_TABLE}(error_type, timestamp);",
    f"CREATE INDEX IF NOT EXISTS idx_suite_runs_stage ON {RUNS_TABLE}(stage);",
]


def connect(db_path: os.PathLike | str) -> sqlite3.Connection:
    path = Path(db_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path))
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON;")
    # WAL lets the harness keep writing while the explorer reads.
    conn.execute("PRAGMA journal_mode=WAL;")
    return conn


def init_schema(conn: sqlite3.Connection) -> None:
    """Create tables and indexes if they do not already exist."""
    conn.execute(_settings_table_sql())
    conn.execute(RUNS_TABLE_SQL)
    ensure_indexes(conn)
    conn.commit()


def ensure_indexes(conn: sqlite3.Connection) -> List[str]:
    """Create any missing indexes from INDEX_SQL. Safe to re-run."""
    before = {
        r["name"] if isinstance(r, sqlite3.Row) else r[0]
        for r in conn.execute(
            "SELECT name FROM sqlite_master WHERE type = 'index' AND name IS NOT NULL"
        )
    }
    for stmt in INDEX_SQL:
        conn.execute(stmt)
    conn.commit()
    after = {
        r["name"] if isinstance(r, sqlite3.Row) else r[0]
        for r in conn.execute(
            "SELECT name FROM sqlite_master WHERE type = 'index' AND name IS NOT NULL"
        )
    }
    return [f"Created index: {name}" for name in sorted(after - before)]


def table_columns(conn: sqlite3.Connection, table: str) -> List[str]:
    rows = conn.execute(f"PRAGMA table_info({table})").fetchall()
    return [r["name"] if isinstance(r, sqlite3.Row) else r[1] for r in rows]


def add_settings_column(
    conn: sqlite3.Connection,
    column_name: str,
    sql_type: str = "TEXT",
    dry_run: bool = False,
) -> str:
    """
    Add a new column to the suite_settings dimension table (for future options).

    Example:
        add_settings_column(conn, "some_new_option", "TEXT")
    """
    existing = table_columns(conn, SETTINGS_TABLE)
    if column_name in existing:
        return f"Column already exists: {SETTINGS_TABLE}.{column_name}"

    stmt = f"ALTER TABLE {SETTINGS_TABLE} ADD COLUMN {column_name} {sql_type};"
    if dry_run:
        return f"DRY RUN: {stmt}"
    conn.execute(stmt)
    conn.commit()
    return f"Added column: {SETTINGS_TABLE}.{column_name} {sql_type}"


def ensure_runs_columns(conn: sqlite3.Connection) -> List[str]:
    """Add suite_runs columns introduced after the original schema; backfill suite."""
    messages = []
    existing = set(table_columns(conn, RUNS_TABLE))
    if "suite" not in existing:
        conn.execute(f"ALTER TABLE {RUNS_TABLE} ADD COLUMN suite TEXT")
        messages.append(f"Added column: {RUNS_TABLE}.suite TEXT")
    conn.execute(
        f"UPDATE {RUNS_TABLE} SET suite = 'mystery' "
        f"WHERE suite IS NULL OR TRIM(suite) = ''"
    )
    conn.execute(f"CREATE INDEX IF NOT EXISTS idx_suite_runs_suite ON {RUNS_TABLE}(suite)")
    conn.commit()
    return messages


def ensure_settings_columns(conn: sqlite3.Connection) -> List[str]:
    """
    Ensure every column in SETTINGS_COLUMN_SPECS exists on suite_settings.
    Safe to re-run after code updates that add new specs.
    """
    messages = []
    existing = set(table_columns(conn, SETTINGS_TABLE))
    for name, sql_type, _attr, _is_bool, _is_global in SETTINGS_COLUMN_SPECS:
        if name in existing:
            continue
        # strip NOT NULL for ALTER ADD (existing rows get NULL)
        alter_type = sql_type.replace(" NOT NULL", "")
        msg = add_settings_column(conn, name, alter_type)
        messages.append(msg)
    return messages


def _as_sql_bool(value: Any) -> Optional[int]:
    if value is None:
        return None
    return 1 if bool(value) else 0


def _as_sql_text(value: Any) -> Optional[str]:
    if value is None:
        return None
    return str(value)


def settings_row_from_world(world, player: int = 1) -> Dict[str, Any]:
    """Decode a World into a settings dimension row (includes code + version)."""
    from BaseClasses import Settings, settings_version

    code = Settings.make_code(world, player)
    row: Dict[str, Any] = {
        "settings_code": code,
        "settings_ver": int(settings_version),
    }

    for name, _sql_type, attr, is_bool, is_global in SETTINGS_COLUMN_SPECS:
        if name in ("settings_code", "settings_ver"):
            continue
        if attr is None:
            continue
        if is_global:
            raw = getattr(world, attr)
        else:
            mapping = getattr(world, attr)
            raw = mapping[player] if isinstance(mapping, dict) else mapping
        if is_bool:
            row[name] = _as_sql_bool(raw)
        else:
            row[name] = _as_sql_text(raw)
    return row


# args attribute name -> World attribute name used by Settings.make_code / SETTINGS_COLUMN_SPECS
_ARGS_TO_WORLD_ATTR = (
    ("shuffle", "shuffle"),
    ("door_shuffle", "doorShuffle"),
    ("logic", "logic"),
    ("mode", "mode"),
    ("swords", "swords"),
    ("bombbag", "bombbag"),
    ("goal", "goal"),
    ("difficulty", "difficulty"),
    ("item_functionality", "difficulty_adjustments"),
    ("hints", "hints"),
    ("shopsanity", "shopsanity"),
    ("decoupledoors", "decoupledoors"),
    ("mixed_travel", "mixed_travel"),
    ("standardize_palettes", "standardize_palettes"),
    ("intensity", "intensity"),
    ("shuffletavern", "shuffletavern"),
    ("dropshuffle", "dropshuffle"),
    ("pottery", "pottery"),
    ("door_self_loops", "door_self_loops"),
    ("crystals_gt", "crystals_gt_orig"),
    ("dungeon_counters", "dungeon_counters"),
    ("experimental", "experimental"),
    ("shufflelinks", "shufflelinks"),
    ("crystals_ganon", "crystals_ganon_orig"),
    ("openpyramid", "open_pyramid"),
    ("accessibility", "accessibility"),
    ("mapshuffle", "mapshuffle"),
    ("compassshuffle", "compassshuffle"),
    ("keyshuffle", "keyshuffle"),
    ("bigkeyshuffle", "bigkeyshuffle"),
    ("enemy_health", "enemy_health"),
    ("enemy_damage", "enemy_damage"),
    ("shufflepots", "potshuffle"),
    ("shuffleenemies", "enemy_shuffle"),
    ("restrict_boss_items", "restrict_boss_items"),
    ("shufflebosses", "boss_shuffle"),
    ("ow_parallel", "owParallel"),
    ("ow_layout", "owLayout"),
    ("ow_terrain", "owTerrain"),
    ("ow_whirlpool", "owWhirlpoolShuffle"),
    ("ow_crossed", "owCrossed"),
    ("ow_keepsimilar", "owKeepSimilar"),
    ("ow_mixed", "owMixed"),
    ("bonk_drops", "shuffle_bonk_drops"),
    ("shuffle_followers", "shuffle_followers"),
    ("ow_fluteshuffle", "owFluteShuffle"),
    ("ow_fog", "owFog"),
    ("flute_mode", "flute_mode"),
    ("bow_mode", "bow_mode"),
    ("take_any", "take_any"),
    ("prizeshuffle", "prizeshuffle"),
    ("pseudoboots", "pseudoboots"),
    ("overworld_map", "overworld_map"),
    ("trap_door_mode", "trap_door_mode"),
    ("key_logic_algorithm", "key_logic_algorithm"),
    ("skullwoods", "skullwoods"),
    ("linked_drops", "linked_drops"),
    ("mirrorscroll", "mirrorscroll"),
    ("door_type_mode", "door_type_mode"),
)


def _player_map(value: Any, player: int = 1) -> Dict[int, Any]:
    if isinstance(value, dict):
        return value
    return {player: value}


def world_shim_from_args(args, player: int = 1):
    """
    Build a minimal World-like object for Settings.make_code without importing Main/Rom.

    The harness freeze path must not import Rom (requires the bps package) just to
    compute a settings code; generation still needs bps via DungeonRandomizer.
    """
    class _WorldShim:
        def is_pyramid_open(self, p):
            # Mirror BaseClasses.World.is_pyramid_open
            if self.open_pyramid[p] == "yes":
                return True
            if self.open_pyramid[p] == "no":
                return False
            if self.shuffle[p] not in ["vanilla", "dungeonssimple", "dungeonsfull", "district"]:
                return False
            return self.goal[p] in ["crystals", "trinity", "ganonhunt"]

    w = _WorldShim()
    for args_attr, world_attr in _ARGS_TO_WORLD_ATTR:
        raw = getattr(args, args_attr, None)
        setattr(w, world_attr, _player_map(raw, player))

    # algorithm is multiworld-wide (not per-player)
    w.algorithm = getattr(args, "algorithm", "balanced")

    # make_code expects numeric intensity unless "random"
    intensity = w.intensity.get(player)
    if intensity is not None and intensity != "random":
        try:
            w.intensity[player] = int(intensity)
        except (TypeError, ValueError):
            pass

    return w


def settings_row_from_args(args, player: int = 1) -> Dict[str, Any]:
    """Build a settings dimension row from CLI args after customizer apply (no Main/Rom)."""
    return settings_row_from_world(world_shim_from_args(args, player), player)


def upsert_settings(conn: sqlite3.Connection, row: Dict[str, Any]) -> None:
    cols = [c for c, *_ in SETTINGS_COLUMN_SPECS]
    placeholders = ", ".join("?" for _ in cols)
    col_list = ", ".join(cols)
    values = [row.get(c) for c in cols]
    conn.execute(
        f"INSERT OR IGNORE INTO {SETTINGS_TABLE} ({col_list}) VALUES ({placeholders})",
        values,
    )


def insert_seedgen(
    conn: sqlite3.Connection,
    *,
    seed: Optional[int],
    mystery_name: Optional[str],
    suite: Optional[str] = None,
    settings_code: str,
    settings_ver: int,
    app_version: str,
    timestamp: int,
    success: bool,
    duration_ms: Optional[int] = None,
    stage: Optional[str] = None,
    error_type: Optional[str] = None,
    log: Optional[str] = None,
) -> int:
    cur = conn.execute(
        f"""
        INSERT INTO {RUNS_TABLE} (
            seed, mystery_name, suite, settings_code, settings_ver, app_version,
            timestamp, success, duration_ms, stage, error_type, log
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (
            seed,
            mystery_name,
            suite,
            settings_code,
            settings_ver,
            app_version,
            timestamp,
            1 if success else 0,
            duration_ms,
            stage,
            error_type,
            log,
        ),
    )
    return int(cur.lastrowid)


def record_attempt(
    conn: sqlite3.Connection,
    settings_row: Dict[str, Any],
    *,
    seed: Optional[int],
    mystery_name: Optional[str],
    suite: Optional[str] = None,
    app_version: str,
    timestamp: int,
    success: bool,
    duration_ms: Optional[int] = None,
    stage: Optional[str] = None,
    error_type: Optional[str] = None,
    log: Optional[str] = None,
) -> int:
    upsert_settings(conn, settings_row)
    row_id = insert_seedgen(
        conn,
        seed=seed,
        mystery_name=mystery_name,
        suite=suite,
        settings_code=settings_row["settings_code"],
        settings_ver=settings_row["settings_ver"],
        app_version=app_version,
        timestamp=timestamp,
        success=success,
        duration_ms=duration_ms,
        stage=stage,
        error_type=error_type,
        log=log,
    )
    conn.commit()
    return row_id


# ---------------------------------------------------------------------------
# Args / customizer freeze helpers (used by the generation harness)
# ---------------------------------------------------------------------------

# (yaml_key under settings.1, args attribute name)
_FIXED_SETTING_FIELDS: Sequence[Tuple[str, str]] = (
    ("ow_layout", "ow_layout"),
    ("ow_parallel", "ow_parallel"),
    ("ow_terrain", "ow_terrain"),
    ("ow_crossed", "ow_crossed"),
    ("ow_keepsimilar", "ow_keepsimilar"),
    ("ow_mixed", "ow_mixed"),
    ("ow_whirlpool", "ow_whirlpool"),
    ("ow_fluteshuffle", "ow_fluteshuffle"),
    ("ow_fog", "ow_fog"),
    ("shuffle_followers", "shuffle_followers"),
    ("bonk_drops", "bonk_drops"),
    ("shuffle", "shuffle"),
    ("door_shuffle", "door_shuffle"),
    ("logic", "logic"),
    ("mode", "mode"),
    ("boots_hint", "boots_hint"),
    ("swords", "swords"),
    ("flute_mode", "flute_mode"),
    ("bow_mode", "bow_mode"),
    ("item_functionality", "item_functionality"),
    ("goal", "goal"),
    ("difficulty", "difficulty"),
    ("accessibility", "accessibility"),
    ("retro", "retro"),
    ("take_any", "take_any"),
    ("hints", "hints"),
    ("shopsanity", "shopsanity"),
    ("dropshuffle", "dropshuffle"),
    ("pottery", "pottery"),
    ("mixed_travel", "mixed_travel"),
    ("standardize_palettes", "standardize_palettes"),
    ("intensity", "intensity"),
    ("door_type_mode", "door_type_mode"),
    ("trap_door_mode", "trap_door_mode"),
    ("key_logic_algorithm", "key_logic_algorithm"),
    ("decoupledoors", "decoupledoors"),
    ("door_self_loops", "door_self_loops"),
    ("dungeon_counters", "dungeon_counters"),
    ("crystals_gt", "crystals_gt"),
    ("crystals_ganon", "crystals_ganon"),
    ("experimental", "experimental"),
    ("collection_rate", "collection_rate"),
    ("openpyramid", "openpyramid"),
    ("prizeshuffle", "prizeshuffle"),
    ("bigkeyshuffle", "bigkeyshuffle"),
    ("keyshuffle", "keyshuffle"),
    ("mapshuffle", "mapshuffle"),
    ("compassshuffle", "compassshuffle"),
    ("boss_shuffle", "shufflebosses"),
    ("enemy_shuffle", "shuffleenemies"),
    ("enemy_health", "enemy_health"),
    ("enemy_damage", "enemy_damage"),
    ("any_enemy_logic", "any_enemy_logic"),
    ("shufflepots", "shufflepots"),
    ("bombbag", "bombbag"),
    ("shufflelinks", "shufflelinks"),
    ("shuffletavern", "shuffletavern"),
    ("shuffleganon", "shuffleganon"),
    ("skullwoods", "skullwoods"),
    ("linked_drops", "linked_drops"),
    ("restrict_boss_items", "restrict_boss_items"),
    ("overworld_map", "overworld_map"),
    ("pseudoboots", "pseudoboots"),
    ("mirrorscroll", "mirrorscroll"),
    ("triforce_goal", "triforce_goal"),
    ("triforce_pool", "triforce_pool"),
    ("triforce_goal_min", "triforce_goal_min"),
    ("triforce_goal_max", "triforce_goal_max"),
    ("triforce_pool_min", "triforce_pool_min"),
    ("triforce_pool_max", "triforce_pool_max"),
    ("triforce_min_difference", "triforce_min_difference"),
    ("triforce_max_difference", "triforce_max_difference"),
)


def _player_value(args, attr: str, player: int = 1) -> Any:
    val = getattr(args, attr, None)
    if isinstance(val, dict):
        return val.get(player)
    return val


def fixed_player_settings_from_args(args, player: int = 1) -> Dict[str, Any]:
    """Snapshot rolled args into a plain dict suitable for a fixed customizer yaml."""
    out: Dict[str, Any] = {}
    for yaml_key, attr in _FIXED_SETTING_FIELDS:
        val = _player_value(args, attr, player)
        if val is None:
            continue
        out[yaml_key] = val
    return out


def write_fixed_customizer(
    path: os.PathLike | str,
    player_settings: Dict[str, Any],
    *,
    seed: Optional[int] = None,
    algorithm: Optional[str] = None,
) -> None:
    import yaml

    meta: Dict[str, Any] = {"players": 1}
    if seed is not None:
        meta["seed"] = int(seed)
    if algorithm is not None:
        meta["algorithm"] = algorithm

    doc = {
        "meta": meta,
        "settings": {1: player_settings},
    }
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        yaml.safe_dump(doc, f, default_flow_style=False, sort_keys=False)


def make_temp_fixed_customizer(
    player_settings: Dict[str, Any],
    *,
    seed: Optional[int] = None,
    algorithm: Optional[str] = None,
    directory: Optional[os.PathLike | str] = None,
) -> str:
    fd, name = tempfile.mkstemp(prefix="automate_fixed_", suffix=".yaml", dir=directory)
    os.close(fd)
    write_fixed_customizer(name, player_settings, seed=seed, algorithm=algorithm)
    return name


def get_app_version() -> str:
    try:
        from OverworldShuffle import __version__ as or_version
    except Exception:
        or_version = "unknown"
    try:
        from Main import __version__ as dr_version
    except Exception:
        dr_version = "unknown"
    return f"DR{dr_version}/OR{or_version}"

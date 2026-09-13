#!/usr/bin/env python3
"""
Local browser dashboard for TestSuiteAutomate results.

Reads suite_runs / suite_settings (read-only) and serves an explorer at
http://127.0.0.1:<port>/

  python test/suite/automate/TestSuiteAutomateExplore.pyw
  python test/suite/automate/TestSuiteAutomateExplore.pyw --db <path/to/log.db>
  python test/suite/automate/TestSuiteAutomateExplore.pyw --port 8765 --no-browser
  pythonw test/suite/automate/TestSuiteAutomateExplore.pyw

Views:
  Overview     — volume, fail rate over time, error/stage mix, duration
  By setting   — pick a setting, see stage / error_type mix per value
  Error cause  — pick an error_type, rank setting values by lift vs baseline
  Crosstab     — two settings as a fail/error-rate heatmap

View-only error hides (after a fix lands): hide an error_type as of a timestamp
so older failures leave charts; new post-fix occurrences still show. Stored in
a sidecar JSON next to the DB (see queries.hides_path). Overview has
“Show hidden errors” and per-error Hide/Unhide.
"""

from __future__ import annotations

import argparse
import socket
import sys
import traceback
import webbrowser
from pathlib import Path

# This file lives at <repo>/test/suite/automate/
_HERE = Path(__file__).resolve().parent
REPO_ROOT = _HERE.parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from test.suite.automate.TestSuiteAutomateDB import DEFAULT_DB_PATH  # noqa: E402
from test.suite.automate.explore.server import make_server  # noqa: E402


def _free_port(host: str, preferred: int) -> int:
    for port in range(preferred, preferred + 20):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                s.bind((host, port))
            except OSError:
                continue
            return port
    raise RuntimeError(f"No free port in {preferred}–{preferred + 19} on {host}")


def _log(msg: str) -> None:
    try:
        print(msg, flush=True)
    except Exception:
        pass


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="TestSuiteAutomateExplore",
        description="Local browser explorer for TestSuiteAutomate SQLite results.",
    )
    p.add_argument(
        "--db",
        default=str(DEFAULT_DB_PATH),
        help=f"SQLite database path (default: {DEFAULT_DB_PATH})",
    )
    p.add_argument("--host", default="127.0.0.1", help="Bind address (default: 127.0.0.1)")
    p.add_argument("--port", type=int, default=8765, help="Preferred port (default: 8765)")
    p.add_argument("--no-browser", action="store_true", help="Do not open a browser window")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    db_path = str(Path(args.db))
    host = args.host
    try:
        port = _free_port(host, int(args.port))
    except Exception as e:
        _log(str(e))
        return 2

    try:
        httpd = make_server(db_path, host=host, port=port)
    except Exception:
        _log(traceback.format_exc())
        return 2

    url = f"http://{host}:{port}/"
    _log(f"TestSuite Explorer  db={db_path}")
    _log(f"Listening on {url}")
    if not args.no_browser:
        try:
            webbrowser.open(url)
        except Exception as e:
            _log(f"Could not open browser: {e}")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        _log("Stopped.")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""
Localhost HTTP server for the TestSuiteAutomate explorer.

Binds 127.0.0.1 only. Opens the SQLite DB read-only per request so the
generation harness can keep writing.
"""

from __future__ import annotations

import json
import mimetypes
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Optional, Tuple
from urllib.parse import parse_qs, unquote, urlparse

from test.suite.automate.explore import queries as Q

STATIC_DIR = Path(__file__).resolve().parent / "static"


def _q_int(params: Dict[str, list], key: str, default: Optional[int] = None) -> Optional[int]:
    raw = params.get(key, [None])[0]
    if raw is None or raw == "":
        return default
    return int(raw)


def _q_str(params: Dict[str, list], key: str, default: Optional[str] = None) -> Optional[str]:
    raw = params.get(key, [None])[0]
    if raw is None:
        return default
    raw = raw.strip()
    return raw if raw != "" else default


def _q_bool(params: Dict[str, list], key: str) -> bool:
    raw = (params.get(key, [""])[0] or "").strip().lower()
    return raw in ("1", "true", "yes", "on")


def _setting_value(params: Dict[str, list], key: str = "value") -> Any:
    if _q_bool(params, "unset") or _q_str(params, key) == Q.UNSET:
        return Q.UNSET
    return _q_str(params, key)


class ExploreHandler(BaseHTTPRequestHandler):
    db_path: str = ""
    server_version = "TestSuiteExplore/1.0"

    def log_message(self, fmt: str, *args) -> None:
        try:
            BaseHTTPRequestHandler.log_message(self, fmt, *args)
        except Exception:
            pass

    def _send(self, code: int, body: bytes, content_type: str, extra: Optional[Dict[str, str]] = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, payload: Any) -> None:
        body = json.dumps(payload, default=_json_default).encode("utf-8")
        self._send(code, body, "application/json; charset=utf-8")

    def _error(self, code: int, message: str) -> None:
        self._json(code, {"error": message})

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        params = parse_qs(parsed.query, keep_blank_values=True)
        try:
            if path == "/" or path == "/index.html":
                self._serve_static("index.html")
                return
            if path.startswith("/static/"):
                self._serve_static(path[len("/static/"):])
                return
            if path.startswith("/api/"):
                self._api_get(path, params)
                return
            self._error(404, f"Not found: {path}")
        except FileNotFoundError as e:
            self._error(404, str(e))
        except ValueError as e:
            self._error(400, str(e))
        except Exception:
            self._error(500, traceback.format_exc())

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        try:
            if path == "/api/hides":
                body = self._read_json_body()
                et = (body.get("error_type") or "").strip()
                if not et:
                    self._error(400, "error_type is required")
                    return
                as_of = body.get("hidden_as_of")
                if as_of is not None and as_of != "":
                    as_of = int(as_of)
                else:
                    as_of = None
                note = body.get("note") or ""
                hides = Q.set_hide(self.db_path, et, hidden_as_of=as_of, note=note)
                self._json(200, {"hides": hides, "path": str(Q.hides_path(self.db_path))})
                return
            self._error(404, f"Not found: {path}")
        except ValueError as e:
            self._error(400, str(e))
        except Exception:
            self._error(500, traceback.format_exc())

    def do_DELETE(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        params = parse_qs(parsed.query, keep_blank_values=True)
        try:
            if path == "/api/hides":
                et = _q_str(params, "error_type")
                if not et:
                    self._error(400, "error_type is required")
                    return
                hides = Q.clear_hide(self.db_path, et)
                self._json(200, {"hides": hides, "path": str(Q.hides_path(self.db_path))})
                return
            self._error(404, f"Not found: {path}")
        except ValueError as e:
            self._error(400, str(e))
        except Exception:
            self._error(500, traceback.format_exc())

    def _read_json_body(self) -> Dict[str, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length > 0 else b"{}"
        if not raw:
            return {}
        data = json.loads(raw.decode("utf-8"))
        if not isinstance(data, dict):
            raise ValueError("JSON body must be an object")
        return data

    def _serve_static(self, rel: str) -> None:
        rel = rel.replace("\\", "/").lstrip("/")
        if ".." in rel.split("/"):
            self._error(400, "Invalid path")
            return
        target = (STATIC_DIR / rel).resolve()
        if STATIC_DIR.resolve() not in target.parents and target != STATIC_DIR.resolve():
            self._error(400, "Invalid path")
            return
        if not target.is_file():
            self._error(404, f"Missing static file: {rel}")
            return
        forced = {
            ".html": "text/html",
            ".css": "text/css",
            ".js": "application/javascript",
            ".json": "application/json",
            ".svg": "image/svg+xml",
        }
        ctype = forced.get(target.suffix.lower()) or mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json", "image/svg+xml"):
            ctype = f"{ctype}; charset=utf-8"
        self._send(200, target.read_bytes(), ctype)

    def _hide_ctx(self, params: Dict[str, list]) -> Tuple[bool, Dict[str, int], list]:
        show_hidden = _q_bool(params, "show_hidden")
        hides = Q.load_hides(self.db_path)
        hide_map = {h["error_type"]: int(h["hidden_as_of"]) for h in hides}
        return show_hidden, hide_map, hides

    def _api_get(self, path: str, params: Dict[str, list]) -> None:
        start = _q_int(params, "from")
        end = _q_int(params, "to")
        suite = _q_str(params, "suite")
        show_hidden, hide_map, hides = self._hide_ctx(params)
        routes: Dict[str, Callable] = {
            "/api/health": lambda: self._health(),
            "/api/hides": lambda: self._json(200, {
                "hides": hides,
                "path": str(Q.hides_path(self.db_path)),
            }),
            "/api/meta": lambda: self._with_db(
                lambda c: Q.meta(
                    c, start, end,
                    hide_map=hide_map, show_hidden=show_hidden, hides=hides,
                    suite=suite,
                )
            ),
            "/api/overview": lambda: self._with_db(
                lambda c: Q.overview(
                    c, start, end,
                    hide_map=hide_map, show_hidden=show_hidden, hides=hides,
                    suite=suite,
                )
            ),
            "/api/setting": lambda: self._setting(
                params, start, end, hide_map, show_hidden, suite
            ),
            "/api/lift": lambda: self._lift(
                params, start, end, hide_map, show_hidden, suite
            ),
            "/api/crosstab": lambda: self._crosstab(
                params, start, end, hide_map, show_hidden, suite
            ),
            "/api/runs": lambda: self._runs(
                params, start, end, hide_map, show_hidden, suite
            ),
        }
        if path.startswith("/api/run/"):
            rest = path[len("/api/run/"):]
            if not rest.isdigit():
                self._error(400, "Run id must be an integer")
                return
            self._with_db(lambda c: Q.run_detail(c, int(rest)), not_found="Run not found")
            return
        handler = routes.get(path)
        if handler is None:
            self._error(404, f"Unknown API: {path}")
            return
        handler()

    def _with_db(self, fn: Callable, not_found: Optional[str] = None) -> None:
        conn = Q.connect_ro(self.db_path)
        try:
            payload = fn(conn)
        finally:
            conn.close()
        if payload is None and not_found:
            self._error(404, not_found)
            return
        self._json(200, payload)

    def _health(self) -> None:
        info: Dict[str, Any] = {"ok": True, "db": self.db_path}
        try:
            conn = Q.connect_ro(self.db_path)
            try:
                info["n"] = conn.execute("SELECT COUNT(*) FROM suite_runs").fetchone()[0]
                info["suites"] = Q.list_suites(conn)
            finally:
                conn.close()
            info["hides_path"] = str(Q.hides_path(self.db_path))
        except Exception as e:
            info["ok"] = False
            info["error"] = str(e)
        self._json(200 if info["ok"] else 503, info)

    def _setting(
        self,
        params: Dict[str, list],
        start: Optional[int],
        end: Optional[int],
        hide_map: Dict[str, int],
        show_hidden: bool,
        suite: Optional[str],
    ) -> None:
        setting = _q_str(params, "setting")
        if not setting:
            self._error(400, "setting is required")
            return
        self._with_db(lambda c: Q.setting_breakdown(
            c, setting, start, end,
            hide_map=hide_map, show_hidden=show_hidden, suite=suite,
        ))

    def _lift(
        self,
        params: Dict[str, list],
        start: Optional[int],
        end: Optional[int],
        hide_map: Dict[str, int],
        show_hidden: bool,
        suite: Optional[str],
    ) -> None:
        self._with_db(lambda c: Q.error_lift(
            c,
            error_type=_q_str(params, "error_type", Q.ANY_FAIL),
            start=start,
            end=end,
            min_n=_q_int(params, "min_n", 10) or 10,
            stage=_q_str(params, "stage"),
            hide_constant=not _q_bool(params, "show_constant"),
            hide_map=hide_map,
            show_hidden=show_hidden,
            suite=suite,
        ))

    def _crosstab(
        self,
        params: Dict[str, list],
        start: Optional[int],
        end: Optional[int],
        hide_map: Dict[str, int],
        show_hidden: bool,
        suite: Optional[str],
    ) -> None:
        row = _q_str(params, "row")
        col = _q_str(params, "col")
        if not row or not col:
            self._error(400, "row and col are required")
            return
        self._with_db(lambda c: Q.crosstab(
            c, row, col, start, end,
            error_type=_q_str(params, "error_type", Q.ANY_FAIL),
            hide_map=hide_map,
            show_hidden=show_hidden,
            suite=suite,
        ))

    def _runs(
        self,
        params: Dict[str, list],
        start: Optional[int],
        end: Optional[int],
        hide_map: Dict[str, int],
        show_hidden: bool,
        suite: Optional[str],
    ) -> None:
        val = _setting_value(params, "value")
        val2 = _setting_value(params, "value2") if _q_str(params, "setting2") else None
        success_raw = _q_str(params, "success")
        success = int(success_raw) if success_raw in ("0", "1") else None
        self._with_db(lambda c: Q.list_runs(
            c,
            start=start,
            end=end,
            setting=_q_str(params, "setting"),
            value=None if val == Q.UNSET else val,
            unset=val == Q.UNSET or _q_bool(params, "unset"),
            setting2=_q_str(params, "setting2"),
            value2=None if val2 == Q.UNSET else val2,
            unset2=(val2 == Q.UNSET) or _q_bool(params, "unset2"),
            error_type=_q_str(params, "error_type"),
            stage=_q_str(params, "stage"),
            success=success,
            limit=_q_int(params, "limit", 100) or 100,
            offset=_q_int(params, "offset", 0) or 0,
            hide_map=hide_map,
            show_hidden=show_hidden,
            suite=suite,
        ))


def _json_default(obj: Any) -> Any:
    if isinstance(obj, Path):
        return str(obj)
    raise TypeError(f"Not JSON serializable: {type(obj)!r}")


def make_server(db_path: str, host: str = "127.0.0.1", port: int = 8765) -> ThreadingHTTPServer:
    # Ensure explorer indexes exist (brief writable open; requests stay read-only).
    try:
        from test.suite.automate.TestSuiteAutomateDB import connect, ensure_indexes

        conn = connect(db_path)
        try:
            created = ensure_indexes(conn)
            for msg in created:
                print(msg, flush=True)
        finally:
            conn.close()
    except Exception as e:
        print(f"Warning: could not ensure indexes on {db_path}: {e}", flush=True)

    class BoundHandler(ExploreHandler):
        pass

    BoundHandler.db_path = db_path
    httpd = ThreadingHTTPServer((host, port), BoundHandler)
    return httpd

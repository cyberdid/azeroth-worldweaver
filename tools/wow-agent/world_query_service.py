"""Serve bounded world-data requests from agents that cannot read the server tree."""

from __future__ import annotations

import importlib.util
import json
import os
import re
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable

_SOURCE = Path(__file__).with_name("world_query.py")
_SPEC = importlib.util.spec_from_file_location("wow_agent_world_query", _SOURCE)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError("world_query.py is unavailable")
_QUERY = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_QUERY)

_REQUEST_NAME = re.compile(r"^([0-9a-f]{32})\.request\.json$")
_OPERATIONS = {"creature", "gameobject", "spawns", "quest", "fishing", "loot", "item", "spell"}
_ARGUMENTS = {
    "creature": {"entry", "name", "limit"},
    "gameobject": {"entry", "name", "limit"},
    "spawns": {"kind", "entry", "map_id", "zone_id", "x", "y", "z", "radius", "limit"},
    "quest": {"id", "name", "limit"},
    "fishing": {"area_id", "limit"},
    "loot": {"kind", "entry", "limit"},
    "item": {"entry", "name", "limit"},
    "spell": {"id", "name", "limit"},
}
_MAX_REQUEST_BYTES = 4096
_MAX_AGE_SECONDS = 300
_MAX_QUEUED_REQUESTS = 16


def _write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(encoded) > 36 * 1024:
        encoded = b'{"error":"response_too_large"}'
    descriptor, temp_path = tempfile.mkstemp(prefix=".world-query-", suffix=".tmp", dir=path.parent)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = -1
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
    except Exception:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(temp_path)
        except OSError:
            pass
        raise


def _error_result(operation: str, code: str) -> dict[str, Any]:
    provenance = "static_spawn" if operation == "spawns" else "mixed_static" if operation == "fishing" else "static_template"
    return {"operation": operation, "provenance": provenance, "results": [], "truncated": False, "error": code}


def _process_request(request_path: Path, request_dir: Path, config_path: str, log: Callable[[str], None]) -> None:
    match = _REQUEST_NAME.fullmatch(request_path.name)
    if not match:
        return
    request_id = match.group(1)
    working_path = request_dir / f"{request_id}.working"
    response_path = request_dir / f"{request_id}.response.json"
    try:
        os.replace(request_path, working_path)
    except FileNotFoundError:
        return
    operation = "unknown"
    try:
        stat = working_path.stat()
        if stat.st_size > _MAX_REQUEST_BYTES:
            result = _error_result(operation, "request_too_large")
        elif time.time() - stat.st_mtime > _MAX_AGE_SECONDS:
            result = _error_result(operation, "request_expired")
        else:
            try:
                request = json.loads(working_path.read_text(encoding="utf-8"))
            except (OSError, UnicodeError, json.JSONDecodeError):
                request = None
            if (not isinstance(request, dict) or set(request) != {"id", "operation", "args"}
                    or request.get("id") != request_id):
                result = _error_result(operation, "invalid_request")
            else:
                operation = request.get("operation")
                args = request.get("args")
                if (not isinstance(operation, str) or operation not in _OPERATIONS
                        or not isinstance(args, dict) or not set(args).issubset(_ARGUMENTS.get(operation, set()))
                        or any(not isinstance(value, (str, int, float)) or isinstance(value, bool)
                               for value in args.values())):
                    result = _error_result(operation if isinstance(operation, str) else "unknown", "invalid_request")
                else:
                    result = _QUERY.run_query(operation, args, config_path)
        _write_json_atomic(response_path, {"id": request_id, "result": result})
        log(f"world query served operation={operation}")
    except Exception as error:
        log(f"world query service error={type(error).__name__}")
        try:
            _write_json_atomic(response_path, {"id": request_id, "result": _error_result(operation, "service_error")})
        except OSError:
            pass
    finally:
        try:
            working_path.unlink()
        except OSError:
            pass


def serve_requests(
    request_dir: str,
    config_path: str = _QUERY.DEFAULT_CONFIG,
    log: Callable[[str], None] = lambda _: None,
    stop_event: threading.Event | None = None,
) -> None:
    """Continuously serve UUID-named JSON requests from a private shared directory."""
    root = Path(request_dir)
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        root.chmod(0o700)
    except OSError:
        pass
    stop_event = stop_event or threading.Event()
    while not stop_event.is_set():
        try:
            candidates = [path for path in root.iterdir() if _REQUEST_NAME.fullmatch(path.name)]
            candidates.sort(key=lambda path: path.stat().st_mtime)
            for path in candidates[:_MAX_QUEUED_REQUESTS]:
                _process_request(path, root, config_path, log)
            for path in candidates[_MAX_QUEUED_REQUESTS:]:
                match = _REQUEST_NAME.fullmatch(path.name)
                if match:
                    request_id = match.group(1)
                    try:
                        os.replace(path, root / f"{request_id}.working")
                    except FileNotFoundError:
                        continue
                    try:
                        _write_json_atomic(root / f"{request_id}.response.json",
                                           {"id": request_id, "result": _error_result("unknown", "request_queue_full")})
                    finally:
                        try:
                            (root / f"{request_id}.working").unlink()
                        except OSError:
                            pass
            cutoff = time.time() - _MAX_AGE_SECONDS
            for path in root.iterdir():
                if path.name.endswith((".response.json", ".working")):
                    try:
                        if path.stat().st_mtime < cutoff:
                            path.unlink()
                    except OSError:
                        pass
        except OSError as error:
            log(f"world query directory error={type(error).__name__}")
        stop_event.wait(0.25)


def start_service(request_dir: str, config_path: str, log: Callable[[str], None]) -> threading.Thread:
    thread = threading.Thread(target=serve_requests, args=(request_dir, config_path, log),
                              name="world-query-service", daemon=True)
    thread.start()
    return thread

#!/usr/bin/env python3
"""Run a typed read-only AzerothCore lookup or filter the latest live nearby snapshot."""

from __future__ import annotations

import argparse
import json
import math
import os
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

DEFAULT_TELEMETRY = Path(os.environ.get(
    "WOW_AGENT_TELEMETRY_DIR",
    Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "azeroth-worldweaver",
))
OPERATIONS = ("creature", "gameobject", "spawns", "quest", "fishing", "loot", "item", "spell", "vendors", "nearby")
MAX_LIVE_SNAPSHOT_BYTES = 16 * 1024 * 1024
MAX_LIVE_QUERY_ROWS = 50
MAX_LIVE_QUERY_RESPONSE_BYTES = 32 * 1024
MAX_LIVE_SNAPSHOT_AGE_SECONDS = 5.0
MAX_LIVE_CACHED_SNAPSHOTS = 8
MAX_LIVE_SNAPSHOT_CACHE_AGE_SECONDS = 30.0
MAX_LIVE_SNAPSHOT_CACHE_BYTES = 64 * 1024 * 1024


def submit(operation: str, args: dict[str, Any], telemetry_dir: Path = DEFAULT_TELEMETRY,
           timeout_seconds: float = 60.0) -> dict[str, Any]:
    request_dir = telemetry_dir / ".world-query"
    request_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        request_dir.chmod(0o700)
    except OSError:
        pass

    request_id = uuid.uuid4().hex
    request_path = request_dir / f"{request_id}.request.json"
    response_path = request_dir / f"{request_id}.response.json"
    payload = json.dumps({"id": request_id, "operation": operation, "args": args},
                         ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(payload) > 4096:
        raise ValueError("request_too_large")
    descriptor = os.open(request_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(payload)
        stream.flush()
        os.fsync(stream.fileno())

    deadline = time.monotonic() + timeout_seconds
    try:
        while time.monotonic() < deadline:
            try:
                response = json.loads(response_path.read_text(encoding="utf-8"))
            except (FileNotFoundError, OSError, UnicodeError, json.JSONDecodeError):
                time.sleep(0.1)
                continue
            if not isinstance(response, dict) or response.get("id") != request_id:
                raise RuntimeError("invalid_response")
            result = response.get("result")
            if not isinstance(result, dict):
                raise RuntimeError("invalid_response")
            return result
        return {"operation": operation, "provenance": "none", "results": [], "truncated": False,
                "error": "query_service_timeout"}
    finally:
        for path in (request_path, response_path):
            try:
                path.unlink()
            except OSError:
                pass


def _read_latest_live_snapshot(
    telemetry_dir: Path, group_id: str | None
) -> tuple[dict[str, Any] | None, float | None]:
    if group_id is not None and (not group_id.isascii() or not group_id.isdigit()):
        return None, None
    try:
        root = telemetry_dir.resolve()
    except (OSError, RuntimeError):
        return None, None
    paths = ([telemetry_dir / f"world-state-{group_id}.json"] if group_id is not None
             else list(telemetry_dir.glob("world-state-*.json")))
    candidates: list[tuple[int, dict[str, Any]]] = []
    for path in paths:
        try:
            if path.is_symlink() or path.parent.resolve() != root:
                continue
            if path.stat().st_size > MAX_LIVE_SNAPSHOT_BYTES:
                continue
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, RuntimeError, UnicodeError, json.JSONDecodeError):
            continue
        if not isinstance(value, dict) or value.get("schema_version") != 1:
            continue
        current_group = value.get("group_id")
        session_id = value.get("session_id")
        snapshot_seq = value.get("snapshot_seq")
        captured = value.get("captured_at_unix_ms")
        if (not isinstance(current_group, str) or not current_group.isascii() or not current_group.isdigit()
                or (group_id is not None and current_group != group_id)
                or not isinstance(session_id, str) or not session_id
                or not isinstance(snapshot_seq, int) or isinstance(snapshot_seq, bool)
                or not isinstance(captured, int) or isinstance(captured, bool)):
            continue
        candidates.append((captured, value))
    if not candidates:
        return None, None
    captured, snapshot = max(candidates, key=lambda item: item[0])
    age_seconds = (time.time() * 1000 - captured) / 1000
    return snapshot, round(age_seconds, 3)


def _live_snapshot_cache_dir(telemetry_dir: Path) -> Path | None:
    try:
        telemetry_root = telemetry_dir.resolve()
        query_dir = telemetry_dir / ".world-query"
        cache_dir = query_dir / "snapshots"
        if query_dir.is_symlink() or cache_dir.is_symlink():
            return None
        query_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        query_dir.chmod(0o700)
        cache_dir.mkdir(mode=0o700, exist_ok=True)
        cache_dir.chmod(0o700)
        resolved = cache_dir.resolve()
        if telemetry_root not in resolved.parents:
            return None
        return cache_dir
    except (OSError, RuntimeError):
        return None


def _snapshot_cache_path(
    cache_dir: Path, group_id: str, session_id: str, snapshot_seq: int
) -> Path | None:
    if (not group_id.isascii() or not group_id.isdigit()
            or not session_id or len(session_id) > 64
            or not all(char.isdigit() or char == "-" for char in session_id)):
        return None
    path = cache_dir / f"snapshot-{group_id}-{session_id}-{snapshot_seq}.json"
    try:
        if path.is_symlink() or path.parent.resolve() != cache_dir.resolve():
            return None
    except (OSError, RuntimeError):
        return None
    return path


def _read_cached_live_snapshot(
    telemetry_dir: Path, group_id: str, session_id: str, snapshot_seq: int
) -> dict[str, Any] | None:
    cache_dir = _live_snapshot_cache_dir(telemetry_dir)
    path = _snapshot_cache_path(cache_dir, group_id, session_id, snapshot_seq) if cache_dir else None
    if path is None:
        return None
    try:
        stat = path.stat()
        if time.time() - stat.st_mtime > MAX_LIVE_SNAPSHOT_CACHE_AGE_SECONDS:
            path.unlink(missing_ok=True)
            return None
        if stat.st_size > MAX_LIVE_SNAPSHOT_BYTES:
            return None
        snapshot = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, RuntimeError, UnicodeError, json.JSONDecodeError):
        return None
    if (not isinstance(snapshot, dict) or snapshot.get("schema_version") != 1
            or snapshot.get("group_id") != group_id
            or snapshot.get("session_id") != session_id
            or snapshot.get("snapshot_seq") != snapshot_seq):
        return None
    return snapshot


def _prune_live_snapshot_cache(cache_dir: Path) -> None:
    now = time.time()
    cached: list[tuple[float, int, Path]] = []
    for path in cache_dir.glob("snapshot-*.json"):
        try:
            if path.is_symlink() or path.parent.resolve() != cache_dir.resolve():
                continue
            stat = path.stat()
            if now - stat.st_mtime > MAX_LIVE_SNAPSHOT_CACHE_AGE_SECONDS:
                path.unlink(missing_ok=True)
                continue
            cached.append((stat.st_mtime, stat.st_size, path))
        except (OSError, RuntimeError):
            continue

    cached.sort(key=lambda item: (item[0], item[1]), reverse=True)
    retained = 0
    for index, (_mtime, size, path) in enumerate(cached):
        if index >= MAX_LIVE_CACHED_SNAPSHOTS or retained + size > MAX_LIVE_SNAPSHOT_CACHE_BYTES:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        else:
            retained += size


def _cache_live_snapshot(telemetry_dir: Path, snapshot: dict[str, Any]) -> bool:
    group_id = snapshot.get("group_id")
    session_id = snapshot.get("session_id")
    snapshot_seq = snapshot.get("snapshot_seq")
    if (not isinstance(group_id, str) or not isinstance(session_id, str)
            or not isinstance(snapshot_seq, int) or isinstance(snapshot_seq, bool)):
        return False
    cache_dir = _live_snapshot_cache_dir(telemetry_dir)
    path = _snapshot_cache_path(cache_dir, group_id, session_id, snapshot_seq) if cache_dir else None
    if path is None:
        return False
    payload = json.dumps(snapshot, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(payload) > MAX_LIVE_SNAPSHOT_BYTES:
        return False

    temp_path: Path | None = None
    try:
        descriptor, temp_name = tempfile.mkstemp(prefix=".snapshot-", suffix=".tmp", dir=cache_dir)
        temp_path = Path(temp_name)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        temp_path.chmod(0o600)
        os.replace(temp_path, path)
        _prune_live_snapshot_cache(cache_dir)
        return True
    except OSError:
        if temp_path is not None:
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                pass
        return False


def _snapshot_age_seconds(snapshot: dict[str, Any]) -> float | None:
    captured = snapshot.get("captured_at_unix_ms")
    if not isinstance(captured, int) or isinstance(captured, bool):
        return None
    return round((time.time() * 1000 - captured) / 1000, 3)


def query_live_nearby(args: dict[str, Any], telemetry_dir: Path) -> dict[str, Any]:
    """Filter and page the latest server-observed nearby objects without database access."""
    operation = "nearby"
    supported_args = {
        "group_id", "kind", "entry", "name", "map_id", "radius", "limit", "offset",
        "line_of_sight", "attackable_only", "fishing_only", "session_id", "snapshot_seq",
    }

    def error(code: str, **details: Any) -> dict[str, Any]:
        result = {"operation": operation, "provenance": "none", "results": [],
                  "truncated": False, "error": code}
        result.update(details)
        return result

    if set(args) - supported_args:
        return error("unsupported_filter")

    configured_root = os.environ.get("WOW_AGENT_TELEMETRY_DIR")
    if configured_root:
        try:
            if telemetry_dir.resolve() != Path(configured_root).resolve():
                return error("telemetry_dir_not_allowed")
        except (OSError, RuntimeError):
            return error("telemetry_dir_not_allowed")

    group_value = args.get("group_id")
    group_id = str(group_value) if group_value is not None else None
    if group_id is not None and (not group_id.isascii() or not group_id.isdigit()):
        return error("invalid_group_id")

    kind = args.get("kind")
    if kind is not None and kind not in {"creature", "gameobject"}:
        return error("invalid_kind")
    name = args.get("name")
    if name is not None and (not isinstance(name, str) or len(name) > 64):
        return error("invalid_name")
    name_match = name.casefold() if isinstance(name, str) else None

    def integer_arg(value: Any, minimum: int, maximum: int) -> int | None:
        try:
            parsed = int(value)
        except (TypeError, ValueError, OverflowError):
            return None
        return parsed if minimum <= parsed <= maximum else None

    expected_session = args.get("session_id")
    expected_seq_raw = args.get("snapshot_seq")
    if (expected_session is None) != (expected_seq_raw is None):
        return error("invalid_snapshot_pin")
    expected_seq = None
    if expected_session is not None:
        expected_seq = integer_arg(expected_seq_raw, 1, 0xFFFFFFFFFFFFFFFF)
        if (group_id is None or not isinstance(expected_session, str) or not expected_session
                or len(expected_session) > 64
                or not all(char.isdigit() or char == "-" for char in expected_session)
                or expected_seq is None):
            return error("invalid_snapshot_pin")

    entry_filter = None
    if args.get("entry") is not None:
        entry_filter = integer_arg(args["entry"], 1, 0xFFFFFFFF)
        if entry_filter is None:
            return error("invalid_entry")
    map_filter = None
    if args.get("map_id") is not None:
        map_filter = integer_arg(args["map_id"], 0, 0xFFFFFFFF)
        if map_filter is None:
            return error("invalid_map_id")
    limit = integer_arg(args.get("limit", 20), 1, MAX_LIVE_QUERY_ROWS)
    offset = integer_arg(args.get("offset", 0), 0, 100000)
    if limit is None or offset is None:
        return error("invalid_pagination")

    radius: float | None = None
    if args.get("radius") is not None:
        try:
            radius = float(args["radius"])
        except (TypeError, ValueError, OverflowError):
            return error("invalid_radius")
        if not math.isfinite(radius) or not 0 <= radius <= 200:
            return error("invalid_radius")

    los_filter = args.get("line_of_sight")
    if los_filter not in (None, "yes", "no"):
        return error("invalid_line_of_sight")
    attackable_only = args.get("attackable_only", False)
    fishing_only = args.get("fishing_only", False)
    if not isinstance(attackable_only, bool) or not isinstance(fishing_only, bool):
        return error("invalid_filter")

    cache_hit = False
    if expected_session is not None and expected_seq is not None and group_id is not None:
        snapshot = _read_cached_live_snapshot(telemetry_dir, group_id, expected_session, expected_seq)
        if snapshot is not None:
            cache_hit = True
            age_seconds = _snapshot_age_seconds(snapshot)
        else:
            current, current_age = _read_latest_live_snapshot(telemetry_dir, group_id)
            if current is None or current_age is None:
                return error("live_snapshot_unavailable")
            current_identity = {"session_id": current.get("session_id"),
                                "snapshot_seq": current.get("snapshot_seq")}
            if current_identity != {"session_id": expected_session, "snapshot_seq": expected_seq}:
                return error("snapshot_changed", requested_snapshot={"session_id": expected_session,
                             "snapshot_seq": expected_seq}, current_snapshot=current_identity)
            snapshot, age_seconds = current, current_age
            if not _cache_live_snapshot(telemetry_dir, snapshot):
                return error("snapshot_cache_unavailable")
    else:
        snapshot, age_seconds = _read_latest_live_snapshot(telemetry_dir, group_id)
        if snapshot is None or age_seconds is None:
            return error("live_snapshot_unavailable")
        if not _cache_live_snapshot(telemetry_dir, snapshot):
            return error("snapshot_cache_unavailable")
    if snapshot is None or age_seconds is None:
        return error("live_snapshot_unavailable")
    nearby = snapshot.get("nearby")
    if not isinstance(nearby, list):
        return error("nearby_list_unavailable")

    matches: list[dict[str, Any]] = []
    for record in nearby:
        if not isinstance(record, dict):
            continue
        if kind and record.get("type") != kind:
            continue
        if entry_filter is not None and record.get("entry") != entry_filter:
            continue
        if map_filter is not None and record.get("map_id") != map_filter:
            continue
        if name_match and name_match not in str(record.get("name", "")).casefold():
            continue
        try:
            distance = float(record.get("distance"))
        except (TypeError, ValueError, OverflowError):
            continue
        if not math.isfinite(distance) or (radius is not None and distance > radius):
            continue
        if los_filter is not None and record.get("line_of_sight") != (los_filter == "yes"):
            continue
        if attackable_only and (record.get("type") != "creature"
                                or record.get("attackable_by_nearest_member") is not True):
            continue
        if fishing_only and (record.get("type") != "gameobject"
                             or not (record.get("fishing_node") is True
                                     or record.get("fishing_hole") is True)):
            continue
        matches.append(record)

    matches.sort(key=lambda record: float(record["distance"]))
    page = matches[offset:offset + limit]
    clock_skew = age_seconds < -MAX_LIVE_SNAPSHOT_AGE_SECONDS
    result: dict[str, Any] = {
        "operation": operation,
        "provenance": "live_snapshot",
        "available": True,
        "stale": clock_skew or age_seconds > MAX_LIVE_SNAPSHOT_AGE_SECONDS,
        "clock_skew": clock_skew,
        "snapshot": {
            "group_id": snapshot.get("group_id"),
            "session_id": snapshot.get("session_id"),
            "snapshot_seq": snapshot.get("snapshot_seq"),
            "event_seq": snapshot.get("event_seq"),
            "captured_at_unix_ms": snapshot.get("captured_at_unix_ms"),
            "age_seconds": age_seconds,
        },
        "snapshot_pinned": True,
        "snapshot_cache_hit": cache_hit,
        "nearby_server_limit": snapshot.get("nearby_limit"),
        "nearby_server_count": len(nearby),
        "nearby_server_truncated": snapshot.get("nearby_truncated"),
        "matching_count": len(matches),
        "offset": offset,
        "returned_count": len(page),
        "truncated": offset + len(page) < len(matches),
        "next_offset": offset + len(page) if offset + len(page) < len(matches) else None,
        "results": page,
    }
    while len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) > MAX_LIVE_QUERY_RESPONSE_BYTES:
        if len(page) <= 1:
            return error("response_too_large")
        page = page[:len(page) // 2]
        result["results"] = page
        result["returned_count"] = len(page)
        result["truncated"] = True
        result["next_offset"] = offset + len(page)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=OPERATIONS)
    parser.add_argument("--entry")
    parser.add_argument("--id")
    parser.add_argument("--item-id")
    parser.add_argument("--name")
    parser.add_argument("--kind", choices=("creature", "gameobject"))
    parser.add_argument("--map-id")
    parser.add_argument("--zone-id")
    parser.add_argument("--area-id")
    parser.add_argument("--x")
    parser.add_argument("--y")
    parser.add_argument("--z")
    parser.add_argument("--radius")
    parser.add_argument("--limit", default="20")
    parser.add_argument("--group-id")
    parser.add_argument("--session-id")
    parser.add_argument("--snapshot-seq")
    parser.add_argument("--offset")
    parser.add_argument("--line-of-sight", choices=("yes", "no"))
    parser.add_argument("--attackable-only", action="store_true")
    parser.add_argument("--fishing-only", action="store_true")
    parser.add_argument("--telemetry-dir", type=Path, default=DEFAULT_TELEMETRY)
    parsed = parser.parse_args()
    if parsed.operation == "nearby":
        args = {key: value for key, value in vars(parsed).items()
                if key not in {"operation", "telemetry_dir"} and value is not None}
        args["attackable_only"] = parsed.attackable_only
        args["fishing_only"] = parsed.fishing_only
        result = query_live_nearby(args, parsed.telemetry_dir)
    else:
        if (parsed.group_id is not None or parsed.session_id is not None or parsed.snapshot_seq is not None
                or (parsed.offset is not None and parsed.operation != "vendors") or parsed.line_of_sight is not None
                or parsed.attackable_only or parsed.fishing_only):
            parser.error("live nearby filters are only valid with the nearby operation")
        args = {key: value for key, value in vars(parsed).items()
                if key not in {"operation", "telemetry_dir", "group_id", "session_id", "snapshot_seq",
                               "line_of_sight",
                               "attackable_only", "fishing_only"} and value is not None}
        result = submit(parsed.operation, args, parsed.telemetry_dir)
    print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    return 1 if "error" in result else 0


if __name__ == "__main__":
    raise SystemExit(main())

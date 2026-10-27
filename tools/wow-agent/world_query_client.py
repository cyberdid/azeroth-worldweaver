#!/usr/bin/env python3
"""Submit a named read-only AzerothCore world-data lookup to the local bridge."""

from __future__ import annotations

import argparse
import json
import os
import time
import uuid
from pathlib import Path
from typing import Any

DEFAULT_TELEMETRY = Path(os.environ.get(
    "WOW_AGENT_TELEMETRY_DIR",
    Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "azeroth-worldweaver",
))
OPERATIONS = ("creature", "gameobject", "spawns", "quest", "fishing", "loot", "item", "spell")


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=OPERATIONS)
    parser.add_argument("--entry")
    parser.add_argument("--id")
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
    parser.add_argument("--telemetry-dir", type=Path, default=DEFAULT_TELEMETRY)
    parsed = parser.parse_args()
    args = {key: value for key, value in vars(parsed).items()
            if key not in {"operation", "telemetry_dir"} and value is not None}
    result = submit(parsed.operation, args, parsed.telemetry_dir)
    print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    return 1 if "error" in result else 0


if __name__ == "__main__":
    raise SystemExit(main())

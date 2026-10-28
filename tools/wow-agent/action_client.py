#!/usr/bin/env python3
"""Validate and send the bridge's small typed in-game action protocol."""

from __future__ import annotations

import json
import re
import subprocess
import uuid
from pathlib import Path
from typing import Any, Callable


ALLOWED_BOTS = frozenset({"Claude", "Codex"})
ALLOWED_ACTIONS = frozenset({
    "follow",
    "stay",
    "attack_master_target",
    "attack_nearby",
    "loot_nearby",
    "fish_here",
    "use_fishing_bobber",
    "stop_fishing",
    "ensure_online",
    "cast_spell",
    "interact_npc",
    "gossip_select",
})
ACTION_REQUIRED_FIELDS = {
    "follow": frozenset(),
    "stay": frozenset(),
    "attack_master_target": frozenset(),
    "attack_nearby": frozenset(),
    "loot_nearby": frozenset(),
    "fish_here": frozenset(),
    "use_fishing_bobber": frozenset(),
    "stop_fishing": frozenset(),
    "ensure_online": frozenset(),
    "cast_spell": frozenset({"spell_id"}),
    "interact_npc": frozenset({"target_guid"}),
    "gossip_select": frozenset({"target_guid", "option_index"}),
}
ACTION_OPTIONAL_FIELDS = {
    "cast_spell": frozenset({"target_guid"}),
}
MAX_ACTION_LINE_CHARS = 4096
ACTION_TIMEOUT_SECONDS = 10
_ACTION_LINE = re.compile(r"^\s*ACTION:\s*(.*?)\s*$")
_SAFE_CHARACTER_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9]{0,11}$")
_ACTION_ID = re.compile(r"^[0-9a-f]{32}$")


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate_key")
        result[key] = value
    return result


def _valid_guid(value: Any) -> bool:
    return (isinstance(value, str) and value.isascii() and value.isdigit()
            and 0 < len(value) <= 20 and 0 < int(value) <= 0xFFFFFFFFFFFFFFFF)


def parse_action_response(
    response: str,
    *,
    bot_name: str,
    allow_action: bool = True,
) -> tuple[str, dict[str, Any] | None, str | None]:
    """Remove protocol lines and accept one bounded, action-specific JSON shape."""
    if not isinstance(response, str):
        return "", None, "invalid_response"

    lines = response.splitlines()
    action_lines: list[str] = []
    reply_lines: list[str] = []
    for line in lines:
        if re.match(r"^\s*ACTION\s*:", line):
            action_lines.append(line)
        else:
            reply_lines.append(line)
    reply = "\n".join(reply_lines).strip()

    if not action_lines or not allow_action:
        return reply, None, None
    if len(action_lines) != 1:
        return reply, None, "multiple_actions"

    line = action_lines[0]
    if len(line) > MAX_ACTION_LINE_CHARS:
        return reply, None, "action_too_large"
    match = _ACTION_LINE.fullmatch(line)
    if not match:
        return reply, None, "invalid_action_line"
    try:
        value = json.loads(match.group(1), object_pairs_hook=_strict_object)
    except (json.JSONDecodeError, ValueError):
        return reply, None, "invalid_action_json"
    if not isinstance(value, dict):
        return reply, None, "invalid_action_shape"
    if not all(isinstance(value.get(key), str) for key in ("actor", "type")):
        return reply, None, "invalid_action_shape"
    if value["actor"] != bot_name or bot_name not in ALLOWED_BOTS:
        return reply, None, "actor_mismatch"
    if value["type"] not in ALLOWED_ACTIONS:
        return reply, None, "unsupported_action"
    required = {"actor", "type"} | set(ACTION_REQUIRED_FIELDS[value["type"]])
    allowed = required | set(ACTION_OPTIONAL_FIELDS.get(value["type"], ()))
    if set(value) - allowed:
        return reply, None, "extra_fields"
    if not required.issubset(value):
        return reply, None, "invalid_action_shape"
    if "spell_id" in value and (not isinstance(value["spell_id"], int) or isinstance(value["spell_id"], bool)
                                or not 0 < value["spell_id"] <= 0xFFFFFFFF):
        return reply, None, "invalid_spell_id"
    if "target_guid" in value and not _valid_guid(value["target_guid"]):
        return reply, None, "invalid_target_guid"
    if "option_index" in value and (not isinstance(value["option_index"], int)
                                    or isinstance(value["option_index"], bool)
                                    or not 0 <= value["option_index"] <= 255):
        return reply, None, "invalid_option_index"
    return reply, value, None


def build_action_command(
    action: dict[str, Any],
    *,
    sender: str,
    group_id: str,
    action_id: str,
) -> str:
    """Build one console command from exact enums and validated scalar identifiers."""
    if not isinstance(action, dict) or action.get("type") not in ACTION_REQUIRED_FIELDS:
        raise ValueError("invalid_action_shape")
    actor = action.get("actor")
    action_type = action.get("type")
    required = {"actor", "type"} | set(ACTION_REQUIRED_FIELDS[action_type])
    allowed = required | set(ACTION_OPTIONAL_FIELDS.get(action_type, ()))
    if not required.issubset(action) or set(action) - allowed:
        raise ValueError("invalid_action_shape")
    if actor not in ALLOWED_BOTS:
        raise ValueError("invalid_actor")
    if action_type not in ALLOWED_ACTIONS:
        raise ValueError("unsupported_action")
    if not isinstance(sender, str) or not _SAFE_CHARACTER_NAME.fullmatch(sender):
        raise ValueError("invalid_sender")
    if not isinstance(group_id, str) or not group_id.isascii() or not group_id.isdigit():
        raise ValueError("invalid_group_id")
    if len(group_id) > 20 or int(group_id) > 0xFFFFFFFFFFFFFFFF:
        raise ValueError("invalid_group_id")
    if not isinstance(action_id, str) or not _ACTION_ID.fullmatch(action_id):
        raise ValueError("invalid_action_id")
    spell_id = action.get("spell_id", 0)
    target_guid = action.get("target_guid", "0")
    option_index = action.get("option_index", -1)
    if "spell_id" in action and (not isinstance(spell_id, int) or isinstance(spell_id, bool)
                                  or not 0 < spell_id <= 0xFFFFFFFF):
        raise ValueError("invalid_spell_id")
    if "target_guid" in action and not _valid_guid(target_guid):
        raise ValueError("invalid_target_guid")
    if "option_index" in action and (not isinstance(option_index, int)
                                     or isinstance(option_index, bool)
                                     or not 0 <= option_index <= 255):
        raise ValueError("invalid_option_index")
    return (f"claude action {actor} {sender} {group_id} {action_id} {action_type} "
            f"{spell_id} {target_guid} {option_index}")


def parse_action_result(output: str, expected_action_id: str) -> dict[str, str] | None:
    """Extract a single server acknowledgement and bind it to this request ID."""
    if not isinstance(output, str) or not _ACTION_ID.fullmatch(expected_action_id):
        return None
    results: list[dict[str, Any]] = []
    for line in output.splitlines():
        marker = line.find("ACTION_RESULT:")
        if marker < 0:
            continue
        try:
            value = json.loads(line[marker + len("ACTION_RESULT:"):].strip(), object_pairs_hook=_strict_object)
        except (json.JSONDecodeError, ValueError):
            continue
        if isinstance(value, dict):
            results.append(value)
    if len(results) != 1:
        return None
    value = results[0]
    required = {"action_id", "status", "reason_code", "actor", "type"}
    if (not required.issubset(value)
            or value.get("action_id") != expected_action_id
            or value.get("status") not in {"accepted", "rejected"}
            or not isinstance(value.get("reason_code"), str)
            or value.get("actor") not in ALLOWED_BOTS
            or value.get("type") not in ALLOWED_ACTIONS):
        return None
    return {key: str(value[key]) for key in required}


def dispatch_action(
    action: dict[str, Any],
    *,
    sender: str,
    group_id: str,
    authorized_players: set[str] | frozenset[str],
    server_dir: str | Path,
    runner: Callable[..., Any] = subprocess.run,
    action_id: str | None = None,
) -> dict[str, str]:
    """Submit a validated action through the fixed local SOAP wrapper, never a shell."""
    request_id = action_id if action_id is not None else uuid.uuid4().hex
    if not isinstance(sender, str) or sender.casefold() not in {name.casefold() for name in authorized_players}:
        return {"action_id": request_id, "status": "rejected", "reason_code": "unauthorized_sender",
                "actor": action.get("actor", "Codex"), "type": action.get("type", "follow")}
    try:
        command = build_action_command(action, sender=sender, group_id=group_id, action_id=request_id)
    except (TypeError, ValueError) as error:
        reason = str(error) if str(error) in {
            "invalid_action_shape", "invalid_actor", "unsupported_action", "invalid_sender",
            "invalid_group_id", "invalid_action_id", "invalid_spell_id", "invalid_target_guid",
            "invalid_option_index",
        } else "invalid_request"
        return {"action_id": request_id, "status": "rejected", "reason_code": reason,
                "actor": action.get("actor", "Codex") if isinstance(action, dict) else "Codex",
                "type": action.get("type", "follow") if isinstance(action, dict) else "follow"}

    command_path = str(Path(server_dir) / "claude-cmd.sh")
    try:
        completed = runner(
            [command_path, command],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=ACTION_TIMEOUT_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"action_id": request_id, "status": "unknown", "reason_code": "server_timeout",
                "actor": action["actor"], "type": action["type"]}
    except OSError:
        return {"action_id": request_id, "status": "unknown", "reason_code": "transport_unavailable",
                "actor": action["actor"], "type": action["type"]}

    if completed.returncode != 0:
        return {"action_id": request_id, "status": "unknown", "reason_code": "transport_failed",
                "actor": action["actor"], "type": action["type"]}
    result = parse_action_result(completed.stdout, request_id)
    if result is None:
        return {"action_id": request_id, "status": "unknown", "reason_code": "missing_server_ack",
                "actor": action["actor"], "type": action["type"]}
    if result["actor"] != action["actor"] or result["type"] != action["type"]:
        return {"action_id": request_id, "status": "unknown", "reason_code": "mismatched_server_ack",
                "actor": action["actor"], "type": action["type"]}
    return result


def format_action_receipt(result: dict[str, str]) -> str:
    """Return a brief Ukrainian receipt that does not confuse acceptance with completion."""
    status = result.get("status")
    reason = result.get("reason_code")
    if status == "accepted":
        return "Дію прийнято сервером; результат ще перевіряю за телеметрією."
    if status == "rejected":
        messages = {
            "unauthorized_sender": "сервер не підтвердив відправника",
            "actor_mismatch": "дію адресовано іншому боту",
            "actor_unavailable": "бот недоступний у світі",
            "actor_not_in_group": "бот не в групі з вами",
            "invalid_target": "поточна ціль непридатна для цієї дії",
            "action_unavailable": "зараз дія неможлива",
            "unsupported_action": "така дія не дозволена",
        }
        return f"Сервер відхилив дію: {messages.get(reason, 'перевірте поточний стан гри')}."
    return "Не отримав підтвердження сервера, тому не вважаю дію виконаною."

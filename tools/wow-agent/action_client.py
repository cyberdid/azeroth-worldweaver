#!/usr/bin/env python3
"""Validate and send the bridge's small typed in-game action protocol."""

from __future__ import annotations

import json
import math
import re
import subprocess
import time
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
    "use_item",
    "equip_item",
    "vendor_travel",
    "vendor_travel_cancel",
    "vendor_inspect",
    "vendor_buy",
    "vendor_task_start",
    "vendor_task_cancel",
    "interact_npc",
    "gossip_select",
    "offer_items",
    "hunt_start",
    "hunt_cancel",
    "hunt_loot",
    "guide_start",
    "guide_cancel",
    "playerbot_action",
})
ACTION_REQUIRED_FIELDS = {
    "vendor_travel": frozenset({"map_id", "position", "purchase_reserve_copper"}),
    "vendor_travel_cancel": frozenset(),
    "vendor_inspect": frozenset({"target_guid"}),
    "vendor_buy": frozenset({"target_guid", "item_entry", "quantity", "max_total_copper"}),
    "vendor_task_start": frozenset({"item_entry", "quantity", "max_total_copper"}),
    "vendor_task_cancel": frozenset(),
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
    "use_item": frozenset({"item_entry", "bag", "slot"}),
    "equip_item": frozenset({"item_entry", "bag", "slot"}),
    "interact_npc": frozenset({"target_guid"}),
    "gossip_select": frozenset({"target_guid", "option_index"}),
    "offer_items": frozenset({"items"}),
    "hunt_start": frozenset({"map_id", "zone_id", "kill_count", "deliver_loot"}),
    "hunt_cancel": frozenset(),
    "hunt_loot": frozenset(),
    "guide_start": frozenset({"map_id", "zone_id"}),
    "guide_cancel": frozenset(),
    "playerbot_action": frozenset({"action_name"}),
}
ACTION_OPTIONAL_FIELDS = {
    "vendor_buy": frozenset({"allow_extended_cost"}),
    "vendor_task_start": frozenset({"allow_extended_cost", "return_to_user"}),
    "cast_spell": frozenset({"target_guid"}),
    "hunt_start": frozenset({"creature_entry", "creature_entries", "area_id", "collect_loot"}),
    "guide_start": frozenset({"creature_entry", "creature_entries", "area_id"}),
    "playerbot_action": frozenset({"target_guid", "qualifier"}),
}
MAX_ACTION_LINE_CHARS = 4096
ACTION_TIMEOUT_SECONDS = 10
_ACTION_LINE = re.compile(r"^\s*ACTION:\s*(.*?)\s*$")
_SAFE_CHARACTER_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9]{0,11}$")
_ACTION_ID = re.compile(r"^[0-9a-f]{32}$")
_ITEM_HANDOFF_INTENT = re.compile(
    r"(?:риб\w*|fish\w*|переда\w*|обмін\w*|торг\w*|стек\w*|stack\w*|предмет\w*|item\w*)",
    re.IGNORECASE,
)
_ITEM_HANDOFF_CLAIM = re.compile(
    r"(?:передаю|передав|передала|відправляю|відправив|відправила|кладу|поклав|поклала|"
    r"ставлю|поставив|поставила|transferring|sending|offering|placing)",
    re.IGNORECASE,
)
def has_item_handoff_intent(text: str) -> bool:
    """Recognize direct transfer requests that may need a one-turn clarification."""
    return isinstance(text, str) and _ITEM_HANDOFF_INTENT.search(text) is not None


def contains_item_handoff_claim(text: str) -> bool:
    """Detect wording that could falsely claim a transfer was started or completed."""
    return isinstance(text, str) and _ITEM_HANDOFF_CLAIM.search(text) is not None


def is_fresh_snapshot_for_cursor(
    snapshot: dict[str, Any] | None,
    cursor: dict[str, Any],
    *,
    max_age_seconds: float = 5.0,
) -> bool:
    """Accept a newer live snapshot from the same stream as the prompt cursor."""
    if not isinstance(snapshot, dict) or not isinstance(cursor, dict):
        return False
    if (snapshot.get("_stale") is not False
            or not isinstance(snapshot.get("_age_seconds"), (int, float))
            or snapshot["_age_seconds"] > max_age_seconds
            or snapshot.get("session_id") != cursor.get("session_id")
            or snapshot.get("group_id") != cursor.get("group_id")):
        return False
    current_seq = snapshot.get("snapshot_seq")
    cursor_seq = cursor.get("snapshot_seq")
    return (isinstance(current_seq, int) and not isinstance(current_seq, bool)
            and isinstance(cursor_seq, int) and not isinstance(cursor_seq, bool)
            and current_seq >= cursor_seq)


class PendingItemHandoffClarifications:
    """Allow one short follow-up to a bot's directly requested item-transfer clarification."""

    def __init__(self, timeout_seconds: float = 120.0):
        self.timeout_seconds = timeout_seconds
        self._pending: dict[str, float] = {}

    def remember(self, sender: str, *, channel: str, addressed: bool,
                 other_bot_named: bool, user_text: str, reply_text: str) -> bool:
        key = sender.casefold()
        self._pending.pop(key, None)
        if (channel != "PARTY" or not addressed or other_bot_named
                or not has_item_handoff_intent(user_text)
                or "?" not in reply_text):
            return False
        self._pending[key] = time.monotonic()
        return True

    def consume(self, sender: str, *, channel: str, addressed: bool,
                other_bot_named: bool) -> bool:
        created_at = self._pending.pop(sender.casefold(), None)
        return bool(
            created_at is not None
            and time.monotonic() - created_at <= self.timeout_seconds
            and channel == "PARTY"
            and not addressed
            and not other_bot_named
        )


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


def _encode_item_offers(value: Any) -> str:
    if not isinstance(value, list) or not 1 <= len(value) <= 6:
        raise ValueError("invalid_item_offers")
    encoded: list[str] = []
    seen: set[int] = set()
    total_stacks = 0
    for offer in value:
        if not isinstance(offer, dict) or set(offer) != {"item_id", "stack_count"}:
            raise ValueError("invalid_item_offers")
        item_id = offer["item_id"]
        stack_count = offer["stack_count"]
        if (not isinstance(item_id, int) or isinstance(item_id, bool)
                or not 0 < item_id <= 0xFFFFFFFF):
            raise ValueError("invalid_item_id")
        if (not isinstance(stack_count, int) or isinstance(stack_count, bool)
                or not 1 <= stack_count <= 6):
            raise ValueError("invalid_stack_count")
        if item_id in seen:
            raise ValueError("duplicate_item_id")
        seen.add(item_id)
        total_stacks += stack_count
        if total_stacks > 6:
            raise ValueError("too_many_item_stacks")
        encoded.append(f"{item_id}:{stack_count}")
    return ",".join(encoded)



def _validate_offer_selectors(value: Any) -> None:
    if not isinstance(value, list) or not 1 <= len(value) <= 6:
        raise ValueError("invalid_item_offers")
    validation_offers = []
    for offer in value:
        if isinstance(offer, dict) and set(offer) == {"item_id", "all"}:
            if offer["all"] is not True:
                raise ValueError("invalid_item_offers")
            validation_offers.append({"item_id": offer["item_id"], "stack_count": 1})
        else:
            validation_offers.append(offer)
    _encode_item_offers(validation_offers)


def resolve_inventory_offers(action: dict[str, Any], snapshot: dict[str, Any] | None,
                             group_id: str) -> dict[str, Any]:
    """Resolve 'all' in the actor's current inventory; keep the native trade protocol in stacks."""
    if action.get("type") != "offer_items":
        return action
    offers = action.get("items")
    _validate_offer_selectors(offers)
    if not any("all" in offer for offer in offers):
        return action
    age = snapshot.get("_age_seconds") if isinstance(snapshot, dict) else None
    if (not isinstance(snapshot, dict) or snapshot.get("_stale") is not False
            or not isinstance(age, (int, float)) or isinstance(age, bool) or not 0 <= age <= 5
            or str(snapshot.get("group_id")) != group_id):
        raise ValueError("fresh_inventory_required")
    member = next((m for m in snapshot.get("group", [])
                   if isinstance(m, dict) and m.get("name") == action.get("actor")), None)
    if not member or member.get("inventory_truncated") is not False:
        raise ValueError("inventory_incomplete")
    inventory = member.get("inventory")
    if not isinstance(inventory, list) or member.get("inventory_stacks_total") != len(inventory):
        raise ValueError("inventory_incomplete")
    resolved = []
    for offer in offers:
        if "all" not in offer:
            resolved.append(dict(offer))
            continue
        count = sum(1 for item in inventory if isinstance(item, dict)
                    and item.get("entry") == offer["item_id"] and item.get("tradeable") is True
                    and isinstance(item.get("count"), int) and item["count"] > 0)
        if count == 0:
            raise ValueError("no_tradeable_stacks_in_inventory")
        if count > 6:
            raise ValueError("too_many_item_stacks")
        resolved.append({"item_id": offer["item_id"], "stack_count": count})
    _encode_item_offers(resolved)
    return dict(action, items=resolved)

def _encode_item_location(action: dict[str, Any]) -> str:
    item_entry = action.get("item_entry")
    bag = action.get("bag")
    slot = action.get("slot")
    if (not isinstance(item_entry, int) or isinstance(item_entry, bool)
            or not 0 < item_entry <= 0xFFFFFFFF):
        raise ValueError("invalid_item_entry")
    if (not isinstance(bag, int) or isinstance(bag, bool)
            or bag != 255 and not 19 <= bag < 23):
        raise ValueError("invalid_inventory_bag")
    if not isinstance(slot, int) or isinstance(slot, bool) or not 0 <= slot <= 255:
        raise ValueError("invalid_inventory_slot")
    return f"{item_entry}:{bag}:{slot}"


def _encode_hunt_request(action: dict[str, Any]) -> str:
    has_single = "creature_entry" in action
    has_multiple = "creature_entries" in action
    if has_single == has_multiple:
        raise ValueError("invalid_creature_entries")
    raw_entries = [action["creature_entry"]] if has_single else action["creature_entries"]
    if not isinstance(raw_entries, list) or not 1 <= len(raw_entries) <= 16:
        raise ValueError("invalid_creature_entries")
    entries = []
    for creature_entry in raw_entries:
        if (not isinstance(creature_entry, int) or isinstance(creature_entry, bool)
                or not 0 < creature_entry <= 0xFFFFFFFF):
            raise ValueError("invalid_creature_entry")
        if creature_entry in entries:
            raise ValueError("duplicate_creature_entry")
        entries.append(creature_entry)
    map_id = action.get("map_id")
    zone_id = action.get("zone_id")
    area_id = action.get("area_id", 0)
    kill_count = action.get("kill_count")
    deliver_loot = action.get("deliver_loot")
    if (not isinstance(map_id, int) or isinstance(map_id, bool) or not 0 <= map_id <= 0xFFFF):
        raise ValueError("invalid_map_id")
    if (not isinstance(zone_id, int) or isinstance(zone_id, bool) or not 0 <= zone_id <= 0xFFFF):
        raise ValueError("invalid_zone_id")
    if (not isinstance(area_id, int) or isinstance(area_id, bool) or not 0 <= area_id <= 0xFFFF):
        raise ValueError("invalid_area_id")
    if (not isinstance(kill_count, int) or isinstance(kill_count, bool) or not 1 <= kill_count <= 100):
        raise ValueError("invalid_kill_count")
    if not isinstance(deliver_loot, bool):
        raise ValueError("invalid_deliver_loot")
    collect_loot = action.get("collect_loot", deliver_loot)
    if not isinstance(collect_loot, bool):
        raise ValueError("invalid_collect_loot")
    return f"{','.join(str(entry) for entry in entries)}:{map_id}:{zone_id}:{area_id}:{kill_count}:{int(deliver_loot)}:{int(collect_loot or deliver_loot)}"


def _encode_guide_request(action: dict[str, Any]) -> str:
    has_single = "creature_entry" in action
    has_multiple = "creature_entries" in action
    if has_single and has_multiple:
        raise ValueError("invalid_creature_entries")
    if has_single:
        raw_entries = [action["creature_entry"]]
    elif has_multiple:
        raw_entries = action["creature_entries"]
    else:
        raw_entries = []
    if (not isinstance(raw_entries, list) or len(raw_entries) > 16
            or (has_single or has_multiple) and not raw_entries):
        raise ValueError("invalid_creature_entries")
    entries = []
    for creature_entry in raw_entries:
        if (not isinstance(creature_entry, int) or isinstance(creature_entry, bool)
                or not 0 < creature_entry <= 0xFFFFFFFF):
            raise ValueError("invalid_creature_entry")
        if creature_entry in entries:
            raise ValueError("duplicate_creature_entry")
        entries.append(creature_entry)
    map_id = action.get("map_id")
    zone_id = action.get("zone_id")
    area_id = action.get("area_id", 0)
    if (not isinstance(map_id, int) or isinstance(map_id, bool) or not 0 <= map_id <= 0xFFFF):
        raise ValueError("invalid_map_id")
    if (not isinstance(zone_id, int) or isinstance(zone_id, bool) or not 0 <= zone_id <= 0xFFFF):
        raise ValueError("invalid_zone_id")
    if (not isinstance(area_id, int) or isinstance(area_id, bool) or not 0 <= area_id <= 0xFFFF):
        raise ValueError("invalid_area_id")
    if not entries and zone_id == 0:
        raise ValueError("invalid_zone_id")
    return f"{','.join(str(entry) for entry in entries) or '-'}:{map_id}:{zone_id}:{area_id}"


def _encode_playerbot_action(action: dict[str, Any]) -> str:
    name = action.get("action_name")
    qualifier = action.get("qualifier", "")
    if (not isinstance(name, str) or not name.isascii() or not 1 <= len(name) <= 128
            or name != name.strip() or any(ord(char) < 0x20 or ord(char) > 0x7E for char in name)):
        raise ValueError("invalid_playerbot_action")
    if not isinstance(qualifier, str) or len(qualifier.encode("utf-8")) > 256:
        raise ValueError("invalid_playerbot_qualifier")
    if any(ord(char) < 0x20 or ord(char) == 0x7F for char in qualifier):
        raise ValueError("invalid_playerbot_qualifier")
    return "p" + name.encode("ascii").hex() + "." + qualifier.encode("utf-8").hex()


def _encode_vendor_travel(action: dict[str, Any]) -> str:
    for key, maximum in (("map_id",65535),("purchase_reserve_copper",0xFFFFFFFF)):
        value=action.get(key)
        if not isinstance(value,int) or isinstance(value,bool) or not 0<=value<=maximum:
            raise ValueError("invalid_vendor_"+key)
    position=action.get("position")
    if not isinstance(position,list) or len(position)!=3 or any(
            not isinstance(v,(int,float)) or isinstance(v,bool) or not math.isfinite(v) or abs(v)>100000 for v in position):
        raise ValueError("invalid_vendor_position")
    return f"r:{action['map_id']}:{':'.join(format(v,'.6f') for v in position)}:{action['purchase_reserve_copper']}"


def _encode_vendor_purchase(action: dict[str, Any]) -> str:
    for key, maximum in (("item_entry", 0xFFFFFFFF), ("quantity", 0xFFFF), ("max_total_copper", 0xFFFFFFFF)):
        value = action.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or not (0 if key == "max_total_copper" else 1) <= value <= maximum:
            raise ValueError("invalid_vendor_" + key)
    for key in ("allow_extended_cost", "return_to_user"):
        if key in action and not isinstance(action[key], bool):
            raise ValueError("invalid_vendor_" + key)
    return f"v:{action['item_entry']}:{action['quantity']}:{action['max_total_copper']}:{int(action.get('allow_extended_cost', False))}"


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

    if not action_lines:
        return reply, None, None
    if not allow_action:
        return reply, None, "action_not_authorized"
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
    if value["type"] == "offer_items":
        try:
            _validate_offer_selectors(value["items"])
        except (KeyError, TypeError, ValueError) as error:
            reason = str(error)
            return reply, None, reason if reason in {
                "invalid_item_offers", "invalid_item_id", "invalid_stack_count",
                "duplicate_item_id", "too_many_item_stacks",
            } else "invalid_item_offers"
    if value["type"] in {"use_item", "equip_item"}:
        try:
            _encode_item_location(value)
        except (KeyError, TypeError, ValueError) as error:
            reason = str(error)
            return reply, None, reason if reason in {
                "invalid_item_entry", "invalid_inventory_bag", "invalid_inventory_slot",
            } else "invalid_item_location"
    if value["type"] == "hunt_start":
        try:
            _encode_hunt_request(value)
        except (KeyError, TypeError, ValueError) as error:
            reason = str(error)
            return reply, None, reason if reason in {
                "invalid_creature_entry", "invalid_creature_entries", "duplicate_creature_entry",
                "invalid_map_id", "invalid_zone_id", "invalid_area_id",
                "invalid_kill_count", "invalid_deliver_loot", "invalid_collect_loot",
            } else "invalid_hunt_request"
    if value["type"] == "guide_start":
        try:
            _encode_guide_request(value)
        except (KeyError, TypeError, ValueError) as error:
            reason = str(error)
            return reply, None, reason if reason in {
                "invalid_creature_entry", "invalid_creature_entries", "duplicate_creature_entry",
                "invalid_map_id", "invalid_zone_id", "invalid_area_id",
            } else "invalid_guide_request"
    if value["type"] == "vendor_travel":
        try:
            _encode_vendor_travel(value)
        except ValueError as error:
            return reply, None, str(error)
    if value["type"] in {"vendor_buy", "vendor_task_start"}:
        try:
            _encode_vendor_purchase(value)
        except ValueError as error:
            return reply, None, str(error)
    if value["type"] == "playerbot_action":
        try:
            _encode_playerbot_action(value)
        except (KeyError, TypeError, ValueError) as error:
            reason = str(error)
            return reply, None, reason if reason in {
                "invalid_playerbot_action", "invalid_playerbot_qualifier",
            } else "invalid_playerbot_action"
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
    item_offer_token = "-"
    task_token = "-"
    if "spell_id" in action and (not isinstance(spell_id, int) or isinstance(spell_id, bool)
                                  or not 0 < spell_id <= 0xFFFFFFFF):
        raise ValueError("invalid_spell_id")
    if "target_guid" in action and not _valid_guid(target_guid):
        raise ValueError("invalid_target_guid")
    if "option_index" in action and (not isinstance(option_index, int)
                                     or isinstance(option_index, bool)
                                     or not 0 <= option_index <= 255):
        raise ValueError("invalid_option_index")
    if action_type in {"vendor_task_start", "vendor_task_cancel"}:
        raise ValueError("bridge_local_action")
    if action_type == "vendor_travel":
        task_token = _encode_vendor_travel(action)
    elif action_type == "vendor_buy":
        task_token = _encode_vendor_purchase(action)
    elif action_type == "offer_items":
        item_offer_token = _encode_item_offers(action["items"])
    elif action_type in {"use_item", "equip_item"}:
        item_offer_token = _encode_item_location(action)
    elif action_type == "hunt_start":
        task_token = _encode_hunt_request(action)
    elif action_type == "hunt_cancel":
        task_token = "cancel"
    elif action_type == "guide_start":
        task_token = _encode_guide_request(action)
    elif action_type == "guide_cancel":
        task_token = "cancel"
    elif action_type == "playerbot_action":
        task_token = _encode_playerbot_action(action)
    return (f"claude action {actor} {sender} {group_id} {action_id} {action_type} "
            f"{spell_id} {target_guid} {option_index} {item_offer_token} {task_token}")


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
    inventory_snapshot: dict[str, Any] | None = None,
) -> dict[str, str]:
    """Submit a validated action through the fixed local SOAP wrapper, never a shell."""
    request_id = action_id if action_id is not None else uuid.uuid4().hex
    if not isinstance(sender, str) or sender.casefold() not in {name.casefold() for name in authorized_players}:
        return {"action_id": request_id, "status": "rejected", "reason_code": "unauthorized_sender",
                "actor": action.get("actor", "Codex"), "type": action.get("type", "follow")}
    try:
        action = resolve_inventory_offers(action, inventory_snapshot, group_id)
        command = build_action_command(action, sender=sender, group_id=group_id, action_id=request_id)
    except (TypeError, ValueError) as error:
        reason = str(error) if str(error) in {
            "invalid_action_shape", "invalid_actor", "unsupported_action", "invalid_sender",
            "invalid_group_id", "invalid_action_id", "invalid_spell_id", "invalid_target_guid",
            "invalid_option_index", "invalid_item_offers", "invalid_item_id", "invalid_stack_count",
            "invalid_item_entry", "invalid_inventory_bag", "invalid_inventory_slot",
            "duplicate_item_id", "too_many_item_stacks", "invalid_creature_entry", "invalid_map_id",
            "invalid_creature_entries", "duplicate_creature_entry", "invalid_zone_id", "invalid_area_id",
            "invalid_kill_count", "invalid_deliver_loot", "invalid_collect_loot",
            "invalid_playerbot_action", "invalid_playerbot_qualifier",
            "fresh_inventory_required", "inventory_incomplete", "no_tradeable_stacks_in_inventory",
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

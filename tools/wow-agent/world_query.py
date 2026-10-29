#!/usr/bin/env python3
"""Bounded read-only lookups against the local AzerothCore world database."""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import re
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any

DEFAULT_CONFIG = os.environ.get("WOW_SERVER_CONFIG", "/etc/azerothcore/worldserver.conf")
MAX_ROWS = 100
MAX_OUTPUT_BYTES = 32 * 1024
MYSQL_TIMEOUT_SECONDS = 8
MAX_REFERENCE_DEPTH = 3
MAX_REFERENCE_IDS_PER_QUERY = 64
MAX_REFERENCE_ROWS_PER_QUERY = 100
MAX_EXPANDED_REFERENCE_ROWS = 40
MAX_VENDOR_REFERENCE_DEPTH = 8


class QueryInputError(ValueError):
    pass


class QueryUnavailable(RuntimeError):
    pass


def _load_world_connection(config_path: str) -> dict[str, str]:
    try:
        lines = Path(config_path).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise QueryUnavailable("connection_config_unavailable") from error

    raw_value = None
    for line in lines:
        match = re.match(r"^\s*WorldDatabaseInfo\s*=\s*(.*?)\s*$", line)
        if not match:
            continue
        try:
            tokens = shlex.split(match.group(1), comments=True, posix=True)
        except ValueError as error:
            raise QueryUnavailable("connection_config_invalid") from error
        if tokens:
            raw_value = tokens[0]
            break

    if raw_value is None:
        raise QueryUnavailable("connection_config_unavailable")
    fields = raw_value.split(";")
    if len(fields) not in (5, 6):
        raise QueryUnavailable("connection_config_invalid")
    host, port_or_socket, user, password, database = fields[:5]
    ssl_mode = fields[5] if len(fields) == 6 else ""
    if not host or not user or not database:
        raise QueryUnavailable("connection_config_invalid")
    if any(any(ord(char) < 0x20 for char in value) for value in fields):
        raise QueryUnavailable("connection_config_invalid")

    connection = {"host": host, "user": user, "password": password, "database": database}
    if host == ".":
        if not port_or_socket:
            raise QueryUnavailable("connection_config_invalid")
        connection["host"] = "localhost"
        connection["socket"] = port_or_socket
    else:
        try:
            port = int(port_or_socket)
        except ValueError as error:
            raise QueryUnavailable("connection_config_invalid") from error
        if not 1 <= port <= 65535:
            raise QueryUnavailable("connection_config_invalid")
        connection["port"] = str(port)
    if ssl_mode == "ssl":
        connection["ssl-mode"] = "REQUIRED"
    elif ssl_mode:
        connection["ssl-mode"] = "DISABLED"
    return connection


def _option_file_value(value: str) -> str:
    if any(char in value for char in ("\x00", "\n", "\r")):
        raise QueryUnavailable("connection_config_invalid")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _write_defaults_file(connection: dict[str, str]) -> str:
    descriptor, path = tempfile.mkstemp(prefix="wow-world-query-", suffix=".cnf")
    try:
        os.fchmod(descriptor, 0o600)
        lines = ["[client]"]
        lines.extend(f"{key}={_option_file_value(value)}" for key, value in connection.items())
        lines.append("default-character-set=utf8mb4")
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            stream.write("\n".join(lines) + "\n")
        return path
    except Exception:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            os.unlink(path)
        except OSError:
            pass
        raise


def _sql_value(value: Any) -> str:
    if isinstance(value, bool):
        raise QueryInputError("invalid_parameter")
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise QueryInputError("invalid_parameter")
        return repr(value)
    if isinstance(value, str):
        if not value:
            return "''"
        return "CONVERT(0x" + value.encode("utf-8").hex() + " USING utf8mb4)"
    raise QueryInputError("invalid_parameter")


def _prepared_script(statement: str, parameters: list[Any]) -> str:
    statements = ["START TRANSACTION READ ONLY"]
    variable_names = []
    for index, parameter in enumerate(parameters):
        variable = f"@wow_p{index}"
        variable_names.append(variable)
        statements.append(f"SET {variable} = {_sql_value(parameter)}")
    encoded_statement = statement.encode("utf-8").hex()
    statements.extend((
        f"SET @wow_sql = CONVERT(0x{encoded_statement} USING utf8mb4)",
        "PREPARE wow_query FROM @wow_sql",
        "EXECUTE wow_query USING " + ", ".join(variable_names),
        "DEALLOCATE PREPARE wow_query",
        "COMMIT",
    ))
    return ";\n".join(statements) + ";\n"


def _mysql_rows(
    statement: str,
    parameters: list[Any],
    config_path: str,
) -> list[dict[str, Any]]:
    mysql = shutil.which("mysql")
    if not mysql:
        raise QueryUnavailable("mysql_client_unavailable")
    connection = _load_world_connection(config_path)
    defaults_path = _write_defaults_file(connection)
    command = [
        mysql,
        f"--defaults-extra-file={defaults_path}",
        "--batch",
        "--raw",
        "--skip-column-names",
        "--connect-timeout=3",
        "--default-character-set=utf8mb4",
        "--execute",
        _prepared_script(statement, parameters),
    ]
    try:
        result = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=MYSQL_TIMEOUT_SECONDS,
            check=True,
            close_fds=True,
        )
    except subprocess.TimeoutExpired as error:
        raise QueryUnavailable("query_timeout") from error
    except (OSError, subprocess.CalledProcessError) as error:
        raise QueryUnavailable("database_unavailable") from error
    finally:
        try:
            os.unlink(defaults_path)
        except OSError:
            pass

    rows = []
    for line in result.stdout.splitlines():
        if not line:
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            raise QueryUnavailable("query_result_invalid") from error
        if not isinstance(row, dict):
            raise QueryUnavailable("query_result_invalid")
        rows.append(row)
    return rows


def _identifier(value: Any, name: str, maximum: int = 0xFFFFFFFF, allow_zero: bool = False) -> int:
    if isinstance(value, bool):
        raise QueryInputError("invalid_" + name)
    try:
        number = int(value)
    except (TypeError, ValueError, OverflowError) as error:
        raise QueryInputError("invalid_" + name) from error
    if str(number) != str(value).strip() or number < (0 if allow_zero else 1) or number > maximum:
        raise QueryInputError("invalid_" + name)
    return number


def _limit(args: dict[str, Any]) -> int:
    if isinstance(args.get("limit"), bool):
        raise QueryInputError("invalid_limit")
    try:
        value = int(args.get("limit", 20))
    except (TypeError, ValueError, OverflowError) as error:
        raise QueryInputError("invalid_limit") from error
    if not 1 <= value <= MAX_ROWS:
        raise QueryInputError("invalid_limit")
    return value


def _name(args: dict[str, Any]) -> str:
    value = args.get("name")
    if not isinstance(value, str):
        raise QueryInputError("invalid_name")
    value = value.strip()
    if not value or len(value.encode("utf-8")) > 320 or any(ord(char) < 0x20 for char in value):
        raise QueryInputError("invalid_name")
    return value


def _limit_sql(statement: str, parameters: list[Any], limit: int, config_path: str) -> list[dict[str, Any]]:
    return _mysql_rows(statement, [*parameters, limit + 1], config_path)


def _clean_quest_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    for row in rows:
        objectives = row.get("objective_text")
        if isinstance(objectives, list):
            row["objective_text"] = [text for text in objectives if isinstance(text, str) and text.strip()]
        targets = row.get("required_targets")
        if isinstance(targets, list):
            normalized = []
            for target in targets:
                if not isinstance(target, dict):
                    continue
                try:
                    raw_id = int(target.get("npc_or_gameobject", 0))
                    count = int(target.get("count", 0))
                except (TypeError, ValueError, OverflowError):
                    continue
                if raw_id:
                    normalized.append({"kind": "creature" if raw_id > 0 else "gameobject",
                                       "entry": abs(raw_id), "count": count})
            row["required_targets"] = normalized
        for key in ("required_items", "rewards"):
            values = row.get(key)
            if isinstance(values, list):
                row[key] = [value for value in values
                            if isinstance(value, dict) and value.get("entry") not in (None, 0, "0")]
        for key in ("required_min_reputation", "required_max_reputation"):
            value = row.get(key)
            if isinstance(value, dict) and not value.get("faction"):
                row[key] = None
    return rows


def _lookup_template(operation: str, args: dict[str, Any], limit: int, config_path: str) -> list[dict[str, Any]]:
    by_id = {
        "creature": """SELECT JSON_OBJECT(
            'entry', ct.entry, 'name', ct.name, 'subname', ct.subname,
            'level_min', ct.minlevel, 'level_max', ct.maxlevel, 'faction', ct.faction,
            'npc_flags', ct.npcflag, 'rank', ct.rank, 'family', ct.family, 'type', ct.type,
            'loot_id', ct.lootid, 'pickpocket_loot_id', ct.pickpocketloot, 'skin_loot_id', ct.skinloot,
            'flags_extra', ct.flags_extra,
            'quest_start_ids', (SELECT JSON_ARRAYAGG(qs.quest) FROM creature_queststarter qs WHERE qs.id = ct.entry),
            'quest_end_ids', (SELECT JSON_ARRAYAGG(qe.quest) FROM creature_questender qe WHERE qe.id = ct.entry)
        ) FROM creature_template ct WHERE ct.entry = ? ORDER BY ct.entry LIMIT ?""",
        "gameobject": """SELECT JSON_OBJECT(
            'entry', gt.entry, 'name', gt.name, 'type', gt.type, 'display_id', gt.displayId,
            'size', gt.size, 'data1', gt.Data1,
            'quest_start_ids', (SELECT JSON_ARRAYAGG(qs.quest) FROM gameobject_queststarter qs WHERE qs.id = gt.entry),
            'quest_end_ids', (SELECT JSON_ARRAYAGG(qe.quest) FROM gameobject_questender qe WHERE qe.id = gt.entry)
        ) FROM gameobject_template gt WHERE gt.entry = ? ORDER BY gt.entry LIMIT ?""",
        "item": """SELECT JSON_OBJECT(
            'entry', it.entry, 'name', it.name, 'class', it.class, 'subclass', it.subclass,
            'quality', it.Quality, 'item_level', it.ItemLevel, 'required_level', it.RequiredLevel,
            'required_skill', it.RequiredSkill, 'required_skill_rank', it.RequiredSkillRank,
            'inventory_type', it.InventoryType, 'stackable', it.stackable,
            'buy_price', it.BuyPrice, 'sell_price', it.SellPrice,
            'description', LEFT(it.description, 1000), 'start_quest', it.startquest
        ) FROM item_template it WHERE it.entry = ? ORDER BY it.entry LIMIT ?""",
        "quest": """SELECT JSON_OBJECT(
            'id', q.ID, 'title', LEFT(q.LogTitle, 400), 'level', q.QuestLevel, 'min_level', q.MinLevel,
            'sort_id', q.QuestSortID, 'info_id', q.QuestInfoID,
            'description', LEFT(q.QuestDescription, 3000), 'log_description', LEFT(q.LogDescription, 1500),
            'area_description', LEFT(q.AreaDescription, 1000), 'completion_log', LEFT(q.QuestCompletionLog, 1000),
            'objective_text', JSON_ARRAY(LEFT(q.ObjectiveText1, 500), LEFT(q.ObjectiveText2, 500),
                LEFT(q.ObjectiveText3, 500), LEFT(q.ObjectiveText4, 500)),
            'required_targets', JSON_ARRAY(
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo1, 'count', q.RequiredNpcOrGoCount1),
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo2, 'count', q.RequiredNpcOrGoCount2),
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo3, 'count', q.RequiredNpcOrGoCount3),
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo4, 'count', q.RequiredNpcOrGoCount4)),
            'required_items', JSON_ARRAY(
                JSON_OBJECT('entry', q.RequiredItemId1, 'count', q.RequiredItemCount1),
                JSON_OBJECT('entry', q.RequiredItemId2, 'count', q.RequiredItemCount2),
                JSON_OBJECT('entry', q.RequiredItemId3, 'count', q.RequiredItemCount3),
                JSON_OBJECT('entry', q.RequiredItemId4, 'count', q.RequiredItemCount4),
                JSON_OBJECT('entry', q.RequiredItemId5, 'count', q.RequiredItemCount5),
                JSON_OBJECT('entry', q.RequiredItemId6, 'count', q.RequiredItemCount6)),
            'rewards', JSON_ARRAY(
                JSON_OBJECT('entry', q.RewardItem1, 'count', q.RewardAmount1),
                JSON_OBJECT('entry', q.RewardItem2, 'count', q.RewardAmount2),
                JSON_OBJECT('entry', q.RewardItem3, 'count', q.RewardAmount3),
                JSON_OBJECT('entry', q.RewardItem4, 'count', q.RewardAmount4),
                JSON_OBJECT('entry', q.RewardChoiceItemID1, 'count', q.RewardChoiceItemQuantity1),
                JSON_OBJECT('entry', q.RewardChoiceItemID2, 'count', q.RewardChoiceItemQuantity2),
                JSON_OBJECT('entry', q.RewardChoiceItemID3, 'count', q.RewardChoiceItemQuantity3),
                JSON_OBJECT('entry', q.RewardChoiceItemID4, 'count', q.RewardChoiceItemQuantity4),
                JSON_OBJECT('entry', q.RewardChoiceItemID5, 'count', q.RewardChoiceItemQuantity5),
                JSON_OBJECT('entry', q.RewardChoiceItemID6, 'count', q.RewardChoiceItemQuantity6)),
            'next_quest', q.RewardNextQuest, 'previous_quest', qa.PrevQuestID,
            'exclusive_group', qa.ExclusiveGroup, 'required_skill', qa.RequiredSkillID,
            'required_skill_points', qa.RequiredSkillPoints,
            'required_min_reputation', JSON_OBJECT('faction', qa.RequiredMinRepFaction, 'value', qa.RequiredMinRepValue),
            'required_max_reputation', JSON_OBJECT('faction', qa.RequiredMaxRepFaction, 'value', qa.RequiredMaxRepValue)
        ) FROM quest_template q LEFT JOIN quest_template_addon qa ON qa.ID = q.ID
          WHERE q.ID = ? ORDER BY q.ID LIMIT ?""",
    }
    by_name = {
        "creature": """SELECT JSON_OBJECT(
            'entry', ct.entry, 'name', ct.name, 'subname', ct.subname,
            'level_min', ct.minlevel, 'level_max', ct.maxlevel, 'faction', ct.faction,
            'npc_flags', ct.npcflag, 'rank', ct.rank, 'family', ct.family, 'type', ct.type,
            'loot_id', ct.lootid, 'pickpocket_loot_id', ct.pickpocketloot, 'skin_loot_id', ct.skinloot,
            'flags_extra', ct.flags_extra,
            'quest_start_ids', (SELECT JSON_ARRAYAGG(qs.quest) FROM creature_queststarter qs WHERE qs.id = ct.entry),
            'quest_end_ids', (SELECT JSON_ARRAYAGG(qe.quest) FROM creature_questender qe WHERE qe.id = ct.entry)
        ) FROM creature_template ct WHERE LOCATE(?, ct.name) > 0 ORDER BY ct.name, ct.entry LIMIT ?""",
        "gameobject": """SELECT JSON_OBJECT(
            'entry', gt.entry, 'name', gt.name, 'type', gt.type, 'display_id', gt.displayId,
            'size', gt.size, 'data1', gt.Data1,
            'quest_start_ids', (SELECT JSON_ARRAYAGG(qs.quest) FROM gameobject_queststarter qs WHERE qs.id = gt.entry),
            'quest_end_ids', (SELECT JSON_ARRAYAGG(qe.quest) FROM gameobject_questender qe WHERE qe.id = gt.entry)
        ) FROM gameobject_template gt WHERE LOCATE(?, gt.name) > 0 ORDER BY gt.name, gt.entry LIMIT ?""",
        "item": """SELECT JSON_OBJECT(
            'entry', it.entry, 'name', it.name, 'class', it.class, 'subclass', it.subclass,
            'quality', it.Quality, 'item_level', it.ItemLevel, 'required_level', it.RequiredLevel,
            'required_skill', it.RequiredSkill, 'required_skill_rank', it.RequiredSkillRank,
            'inventory_type', it.InventoryType, 'stackable', it.stackable,
            'buy_price', it.BuyPrice, 'sell_price', it.SellPrice,
            'description', LEFT(it.description, 1000), 'start_quest', it.startquest
        ) FROM item_template it WHERE LOCATE(?, it.name) > 0 ORDER BY it.name, it.entry LIMIT ?""",
        "quest": """SELECT JSON_OBJECT(
            'id', q.ID, 'title', LEFT(q.LogTitle, 400), 'level', q.QuestLevel, 'min_level', q.MinLevel,
            'sort_id', q.QuestSortID, 'info_id', q.QuestInfoID,
            'description', LEFT(q.QuestDescription, 3000), 'log_description', LEFT(q.LogDescription, 1500),
            'area_description', LEFT(q.AreaDescription, 1000), 'completion_log', LEFT(q.QuestCompletionLog, 1000),
            'objective_text', JSON_ARRAY(LEFT(q.ObjectiveText1, 500), LEFT(q.ObjectiveText2, 500),
                LEFT(q.ObjectiveText3, 500), LEFT(q.ObjectiveText4, 500)),
            'required_targets', JSON_ARRAY(
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo1, 'count', q.RequiredNpcOrGoCount1),
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo2, 'count', q.RequiredNpcOrGoCount2),
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo3, 'count', q.RequiredNpcOrGoCount3),
                JSON_OBJECT('npc_or_gameobject', q.RequiredNpcOrGo4, 'count', q.RequiredNpcOrGoCount4)),
            'required_items', JSON_ARRAY(
                JSON_OBJECT('entry', q.RequiredItemId1, 'count', q.RequiredItemCount1),
                JSON_OBJECT('entry', q.RequiredItemId2, 'count', q.RequiredItemCount2),
                JSON_OBJECT('entry', q.RequiredItemId3, 'count', q.RequiredItemCount3),
                JSON_OBJECT('entry', q.RequiredItemId4, 'count', q.RequiredItemCount4),
                JSON_OBJECT('entry', q.RequiredItemId5, 'count', q.RequiredItemCount5),
                JSON_OBJECT('entry', q.RequiredItemId6, 'count', q.RequiredItemCount6)),
            'rewards', JSON_ARRAY(
                JSON_OBJECT('entry', q.RewardItem1, 'count', q.RewardAmount1),
                JSON_OBJECT('entry', q.RewardItem2, 'count', q.RewardAmount2),
                JSON_OBJECT('entry', q.RewardItem3, 'count', q.RewardAmount3),
                JSON_OBJECT('entry', q.RewardItem4, 'count', q.RewardAmount4),
                JSON_OBJECT('entry', q.RewardChoiceItemID1, 'count', q.RewardChoiceItemQuantity1),
                JSON_OBJECT('entry', q.RewardChoiceItemID2, 'count', q.RewardChoiceItemQuantity2),
                JSON_OBJECT('entry', q.RewardChoiceItemID3, 'count', q.RewardChoiceItemQuantity3),
                JSON_OBJECT('entry', q.RewardChoiceItemID4, 'count', q.RewardChoiceItemQuantity4),
                JSON_OBJECT('entry', q.RewardChoiceItemID5, 'count', q.RewardChoiceItemQuantity5),
                JSON_OBJECT('entry', q.RewardChoiceItemID6, 'count', q.RewardChoiceItemQuantity6)),
            'next_quest', q.RewardNextQuest, 'previous_quest', qa.PrevQuestID,
            'exclusive_group', qa.ExclusiveGroup, 'required_skill', qa.RequiredSkillID,
            'required_skill_points', qa.RequiredSkillPoints,
            'required_min_reputation', JSON_OBJECT('faction', qa.RequiredMinRepFaction, 'value', qa.RequiredMinRepValue),
            'required_max_reputation', JSON_OBJECT('faction', qa.RequiredMaxRepFaction, 'value', qa.RequiredMaxRepValue)
        ) FROM quest_template q LEFT JOIN quest_template_addon qa ON qa.ID = q.ID
          WHERE LOCATE(?, q.LogTitle) > 0 ORDER BY q.LogTitle, q.ID LIMIT ?""",
    }

    identifier_key = "id" if operation == "quest" else "entry"
    if args.get(identifier_key) is not None:
        identifier = _identifier(args[identifier_key], identifier_key)
        rows = _limit_sql(by_id[operation], [identifier], limit, config_path)
        return _clean_quest_rows(rows) if operation == "quest" else rows
    name = _name(args)
    rows = _limit_sql(by_name[operation], [name], limit, config_path)
    return _clean_quest_rows(rows) if operation == "quest" else rows


def _spell(args: dict[str, Any], limit: int, config_path: str) -> list[dict[str, Any]]:
    base = """SELECT JSON_OBJECT(
        'id', s.ID, 'name', s.Name_Lang_enUS, 'name_ruRU', s.Name_Lang_ruRU,
        'description', LEFT(s.Description_Lang_enUS, 800),
        'aura_description', LEFT(s.AuraDescription_Lang_enUS, 500),
        'category', s.Category, 'mechanic', s.Mechanic, 'spell_level', s.SpellLevel,
        'power_type', s.PowerType, 'mana_cost', s.ManaCost, 'recovery_ms', s.RecoveryTime,
        'range_index', s.RangeIndex, 'duration_index', s.DurationIndex,
        'effects', JSON_ARRAY(
            JSON_OBJECT('type', s.Effect_1, 'base_points', s.EffectBasePoints_1, 'aura', s.EffectAura_1, 'trigger_spell', s.EffectTriggerSpell_1),
            JSON_OBJECT('type', s.Effect_2, 'base_points', s.EffectBasePoints_2, 'aura', s.EffectAura_2, 'trigger_spell', s.EffectTriggerSpell_2),
            JSON_OBJECT('type', s.Effect_3, 'base_points', s.EffectBasePoints_3, 'aura', s.EffectAura_3, 'trigger_spell', s.EffectTriggerSpell_3))
    ) FROM spell_dbc s WHERE {where} ORDER BY s.ID LIMIT ?"""
    if args.get("id") is not None:
        return _limit_sql(base.format(where="s.ID = ?"), [_identifier(args["id"], "id")], limit, config_path)
    return _limit_sql(base.format(where="LOCATE(?, s.Name_Lang_enUS) > 0"), [_name(args)], limit, config_path)


def _creature_spawn_entry_column(config_path: str) -> str:
    rows = _mysql_rows(
        "SELECT JSON_OBJECT('column', COLUMN_NAME) FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? AND COLUMN_NAME IN (?, ?)",
        ["creature", "id1", "id"], config_path)
    columns = {row.get("column") for row in rows}
    if "id1" in columns:
        return "id1"
    if "id" in columns:
        return "id"
    raise QueryUnavailable("spawn_schema_unavailable")


def _spawns(args: dict[str, Any], limit: int, config_path: str) -> list[dict[str, Any]]:
    kind = args.get("kind")
    if kind not in ("creature", "gameobject"):
        raise QueryInputError("invalid_kind")
    if kind == "creature":
        table, entry_column, template_table, template_entry, pool_table, name_column = (
            "creature", _creature_spawn_entry_column(config_path), "creature_template", "entry",
            "pool_creature", "name")
    else:
        table, entry_column, template_table, template_entry, pool_table, name_column = (
            "gameobject", "id", "gameobject_template", "entry", "pool_gameobject", "name")
    alias = "s"
    filters = []
    parameters: list[Any] = []
    if args.get("entry") is not None:
        filters.append(f"{alias}.{entry_column} = ?")
        parameters.append(_identifier(args["entry"], "entry"))
    if args.get("map_id") is not None:
        filters.append(f"{alias}.map = ?")
        parameters.append(_identifier(args["map_id"], "map_id", maximum=65535, allow_zero=True))
    if args.get("zone_id") is not None:
        filters.append(f"{alias}.zoneId = ?")
        parameters.append(_identifier(args["zone_id"], "zone_id", maximum=65535, allow_zero=True))

    coords = [args.get(key) for key in ("x", "y", "z")]
    if any(value is not None for value in coords) and not all(value is not None for value in coords):
        raise QueryInputError("coordinates_require_x_y_z")
    if all(value is not None for value in coords):
        if args.get("map_id") is None:
            raise QueryInputError("coordinates_require_map_id")
        try:
            x, y, z = (float(value) for value in coords)
            radius = float(args.get("radius", 100))
        except (TypeError, ValueError, OverflowError) as error:
            raise QueryInputError("invalid_coordinates") from error
        if (not all(math.isfinite(value) and abs(value) <= 50000 for value in (x, y, z))
                or not math.isfinite(radius) or not 1 <= radius <= 5000):
            raise QueryInputError("invalid_coordinates")
    else:
        x = y = z = radius = None

    if not filters:
        raise QueryInputError("spawns_requires_entry_map_zone_or_coordinates")

    distance_select = "NULL"
    distance_where = ""
    extra_params: list[Any] = []
    if x is not None:
        distance_select = "SQRT(POW(s.position_x - ?, 2) + POW(s.position_y - ?, 2) + POW(s.position_z - ?, 2))"
        extra_params.extend((x, y, z))
        distance_where = " AND (POW(s.position_x - ?, 2) + POW(s.position_y - ?, 2) + POW(s.position_z - ?, 2)) <= POW(?, 2)"
        parameters.extend((x, y, z, radius))

    statement = f"""SELECT JSON_OBJECT(
        'kind', '{kind}', 'guid', d.guid, 'entry', d.entry, 'name', d.name,
        'map_id', d.map_id, 'zone_id', d.zone_id, 'area_id', d.area_id,
        'position', JSON_ARRAY(d.x, d.y, d.z), 'orientation', d.orientation,
        'phase_mask', d.phase_mask, 'spawn_mask', d.spawn_mask,
        'distance_yards', ROUND(d.distance, 1), 'pool_id', d.pool_id,
        'pool_description', d.pool_description, 'loaded', 'unknown', 'provenance', 'static_spawn'
    ) FROM (
        SELECT s.guid, s.{entry_column} AS entry, t.{name_column} AS name,
            s.map AS map_id, s.zoneId AS zone_id, s.areaId AS area_id,
            s.position_x AS x, s.position_y AS y, s.position_z AS z, s.orientation,
            s.phaseMask AS phase_mask, s.spawnMask AS spawn_mask,
            {distance_select} AS distance, p.pool_entry AS pool_id, pt.description AS pool_description
        FROM {table} s
        JOIN {template_table} t ON t.{template_entry} = s.{entry_column}
        LEFT JOIN {pool_table} p ON p.guid = s.guid
        LEFT JOIN pool_template pt ON pt.entry = p.pool_entry
        WHERE {' AND '.join(filters)}{distance_where}
    ) d
    ORDER BY {"d.distance, d.guid" if x is not None else "d.guid"}
    LIMIT ?"""
    rows = _mysql_rows(statement, [*extra_params, *parameters, limit + 1], config_path)
    if kind == "creature" and args.get("zone_id") is not None and not rows:
        fallback_args = {key: value for key, value in args.items() if key != "zone_id"}
        fallback_rows = _spawns(fallback_args, limit, config_path)
        if fallback_rows and any(not row.get("zone_id") for row in fallback_rows):
            raise QueryUnavailable("spawn_zone_metadata_unavailable")
    return rows


def _loot(args: dict[str, Any], limit: int, config_path: str) -> list[dict[str, Any]]:
    kind = args.get("kind")
    entry = _identifier(args.get("entry"), "entry")
    if kind == "creature":
        statement = """SELECT JSON_OBJECT(
            'creature_entry', ct.entry, 'creature_name', ct.name, 'loot_id', ct.lootid,
            'item_id', NULLIF(lt.Item, 0), 'item_name', IF(lt.Reference = 0, it.name, NULL),
            'reference_id', NULLIF(lt.Reference, 0),
            'chance_pct', lt.Chance, 'quest_required', lt.QuestRequired,
            'min_count', lt.MinCount, 'max_count', lt.MaxCount, 'group_id', lt.GroupId, 'comment', lt.Comment
        ) FROM creature_template ct JOIN creature_loot_template lt ON lt.Entry = ct.lootid
          LEFT JOIN item_template it ON it.entry = lt.Item
          WHERE ct.entry = ? AND ct.lootid <> 0 ORDER BY lt.GroupId, lt.Item LIMIT ?"""
    elif kind == "gameobject":
        statement = """SELECT JSON_OBJECT(
            'gameobject_entry', gt.entry, 'gameobject_name', gt.name, 'gameobject_type', gt.type, 'loot_id', gt.Data1,
            'item_id', NULLIF(lt.Item, 0), 'item_name', IF(lt.Reference = 0, it.name, NULL),
            'reference_id', NULLIF(lt.Reference, 0),
            'chance_pct', lt.Chance, 'quest_required', lt.QuestRequired,
            'min_count', lt.MinCount, 'max_count', lt.MaxCount, 'group_id', lt.GroupId, 'comment', lt.Comment
        ) FROM gameobject_template gt JOIN gameobject_loot_template lt ON lt.Entry = gt.Data1
          LEFT JOIN item_template it ON it.entry = lt.Item
          WHERE gt.entry = ? AND gt.type IN (3, 25) AND gt.Data1 <> 0
          ORDER BY lt.GroupId, lt.Item LIMIT ?"""
    else:
        raise QueryInputError("invalid_kind")
    return _limit_sql(statement, [entry], limit, config_path)


def _resolve_reference_contents(
    rows: list[dict[str, Any]], config_path: str,
) -> tuple[list[dict[str, Any]], bool]:
    """Resolve loot references as a bounded tree while preserving each table's raw chances."""
    pending = set()
    for row in rows:
        reference_id = row.get("reference_id")
        if isinstance(reference_id, int) and reference_id > 0:
            pending.add(reference_id)
    if not pending:
        return rows, False

    by_entry: dict[int, list[dict[str, Any]]] = {}
    queried: set[int] = set()
    truncated = False
    for _depth in range(MAX_REFERENCE_DEPTH):
        pending.difference_update(queried)
        if not pending:
            break
        reference_ids = sorted(pending)
        pending.clear()
        if len(reference_ids) > MAX_REFERENCE_IDS_PER_QUERY:
            reference_ids = reference_ids[:MAX_REFERENCE_IDS_PER_QUERY]
            truncated = True
        queried.update(reference_ids)
        placeholders = ", ".join("?" for _ in reference_ids)
        statement = f"""SELECT JSON_OBJECT(
            'reference_entry', rl.Entry, 'item_id', NULLIF(rl.Item, 0),
            'item_name', IF(rl.Reference = 0, it.name, NULL), 'reference_id', NULLIF(rl.Reference, 0),
            'chance_pct', rl.Chance, 'quest_required', rl.QuestRequired,
            'min_count', rl.MinCount, 'max_count', rl.MaxCount, 'loot_mode', rl.LootMode,
            'group_id', rl.GroupId, 'comment', rl.Comment, 'provenance', 'static_template'
        ) FROM reference_loot_template rl
          LEFT JOIN item_template it ON it.entry = rl.Item AND rl.Reference = 0
          WHERE rl.Entry IN ({placeholders}) ORDER BY rl.Entry, rl.GroupId, rl.Item LIMIT ?"""
        reference_rows = _mysql_rows(
            statement, [*reference_ids, MAX_REFERENCE_ROWS_PER_QUERY + 1], config_path)
        if len(reference_rows) > MAX_REFERENCE_ROWS_PER_QUERY:
            reference_rows = reference_rows[:MAX_REFERENCE_ROWS_PER_QUERY]
            truncated = True
        for reference_row in reference_rows:
            try:
                entry = int(reference_row.get("reference_entry", 0))
            except (TypeError, ValueError, OverflowError):
                continue
            by_entry.setdefault(entry, []).append(reference_row)
            nested_id = reference_row.get("reference_id")
            if isinstance(nested_id, int) and nested_id > 0 and nested_id not in queried:
                pending.add(nested_id)
    if pending:
        truncated = True

    expanded_budget = [MAX_EXPANDED_REFERENCE_ROWS]
    tree_truncated = [False]

    def expand(row: dict[str, Any], ancestors: frozenset[int], remaining_depth: int) -> dict[str, Any]:
        result = copy.deepcopy(row)
        reference_id = result.get("reference_id")
        if not isinstance(reference_id, int) or reference_id <= 0:
            return result
        if reference_id in ancestors:
            result["reference_cycle"] = True
            return result
        if remaining_depth <= 0:
            result["reference_contents_truncated"] = True
            tree_truncated[0] = True
            return result
        contents = by_entry.get(reference_id, [])
        result["reference_contents"] = []
        next_ancestors = ancestors | {reference_id}
        for nested in contents:
            if expanded_budget[0] <= 0:
                result["reference_contents_truncated"] = True
                tree_truncated[0] = True
                break
            expanded_budget[0] -= 1
            result["reference_contents"].append(expand(nested, next_ancestors, remaining_depth - 1))
        return result

    resolved = [expand(row, frozenset(), MAX_REFERENCE_DEPTH) for row in rows]
    return resolved, truncated or tree_truncated[0]


def _fishing(args: dict[str, Any], limit: int, config_path: str) -> tuple[list[dict[str, Any]], bool]:
    area_id = _identifier(args.get("area_id"), "area_id", maximum=65535, allow_zero=True)
    catches_sql = """SELECT JSON_OBJECT(
        'record_type', 'catch_table', 'provenance', 'static_template', 'area_id', fl.Entry,
        'item_id', NULLIF(fl.Item, 0), 'item_name', IF(fl.Reference = 0, it.name, NULL),
        'reference_id', NULLIF(fl.Reference, 0), 'chance_pct', fl.Chance,
        'quest_required', fl.QuestRequired, 'min_count', fl.MinCount, 'max_count', fl.MaxCount,
        'loot_mode', fl.LootMode, 'group_id', fl.GroupId, 'comment', fl.Comment
    ) FROM fishing_loot_template fl LEFT JOIN item_template it ON it.entry = fl.Item AND fl.Reference = 0
      WHERE fl.Entry = ? ORDER BY fl.GroupId, fl.Item LIMIT ?"""
    holes_sql = """SELECT JSON_OBJECT(
        'record_type', 'fishing_hole_spawn', 'provenance', 'static_spawn', 'loaded', 'unknown',
        'guid', g.guid, 'entry', g.id, 'name', gt.name, 'map_id', g.map,
        'zone_id', g.zoneId, 'area_id', g.areaId, 'position', JSON_ARRAY(g.position_x, g.position_y, g.position_z),
        'phase_mask', g.phaseMask, 'pool_id', pg.pool_entry, 'pool_description', pt.description
    ) FROM gameobject g JOIN gameobject_template gt ON gt.entry = g.id
      LEFT JOIN pool_gameobject pg ON pg.guid = g.guid LEFT JOIN pool_template pt ON pt.entry = pg.pool_entry
      WHERE g.areaId = ? AND gt.type = 25 ORDER BY g.guid LIMIT ?"""
    catches = _limit_sql(catches_sql, [area_id], limit, config_path)
    holes = _limit_sql(holes_sql, [area_id], limit, config_path)
    truncated = len(catches) > limit or len(holes) > limit
    return catches[:limit] + holes[:limit], truncated


def _vendors(args: dict[str, Any], limit: int, config_path: str) -> dict[str, Any]:
    """Search configured item offers and walk negative-item references towards vendors.

    References supply the leaf row's stock/cost, just as LoadReferenceVendor does.
    SQL recursion is depth bounded and stops on cycles; neither condition is hidden.
    These are catalog/spawn records, never an observation of current merchant stock.
    """
    if set(args) - {"item_id", "name", "entry", "map_id", "limit", "offset"}:
        raise QueryInputError("unsupported_filter")
    limit = _identifier(args.get("limit", 20), "limit", maximum=MAX_ROWS)
    if (args.get("item_id") is None) == (args.get("name") is None):
        raise QueryInputError("vendors_requires_item_id_or_name")
    if args.get("item_id") is not None:
        item_filter = "it.entry = ?"
        item_parameter = _identifier(args["item_id"], "item_id")
    else:
        item_filter = "LOCATE(?, it.name) > 0"
        item_parameter = _name(args)
    offset = _identifier(args.get("offset", 0), "offset", maximum=100000, allow_zero=True)
    filters = ["vi.reference_cycle = 0"]
    filter_parameters: list[Any] = []
    if args.get("entry") is not None:
        filters.append("ct.entry = ?")
        filter_parameters.append(_identifier(args["entry"], "entry"))
    if args.get("map_id") is not None:
        filters.append("s.map = ?")
        filter_parameters.append(_identifier(args["map_id"], "map_id", maximum=65535, allow_zero=True))

    schema_rows = _mysql_rows(
        "SELECT JSON_OBJECT('column', COLUMN_NAME) FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = ? "
        "AND COLUMN_NAME IN (?, ?, ?, ?, ?, ?)",
        ["creature", "id", "id1", "id2", "id3", "zoneId", "areaId"], config_path)
    columns = {row.get("column") for row in schema_rows}
    entry_columns = [column for column in ("id1", "id2", "id3") if column in columns]
    if not entry_columns and "id" in columns:
        entry_columns = ["id"]
    if not entry_columns:
        raise QueryUnavailable("spawn_schema_unavailable")
    spawn_join = " OR ".join(f"s.{column} = ct.entry" for column in entry_columns)
    zone_column = "s.zoneId" if "zoneId" in columns else "NULL"
    area_column = "s.areaId" if "areaId" in columns else "NULL"

    # Only trusted schema column names and a constant depth enter SQL text.
    # Item names and all caller-supplied filters remain prepared parameters.
    cte = f"""WITH RECURSIVE vendor_items AS (
        SELECT nv.entry AS vendor_entry, nv.entry AS catalog_entry,
            nv.item AS item_id, nv.maxcount, nv.incrtime, nv.ExtendedCost,
            0 AS reference_depth,
            CAST(CONCAT('/', nv.entry, '/') AS CHAR(2048)) AS reference_path,
            0 AS reference_cycle
        FROM npc_vendor nv JOIN item_template it ON it.entry = nv.item
        WHERE {item_filter}
        UNION ALL
        SELECT parent.entry, vi.catalog_entry, vi.item_id, vi.maxcount, vi.incrtime, vi.ExtendedCost,
            vi.reference_depth + 1, CONCAT(vi.reference_path, parent.entry, '/'),
            LOCATE(CONCAT('/', parent.entry, '/'), vi.reference_path) > 0
        FROM vendor_items vi JOIN npc_vendor parent ON parent.item = -CAST(vi.vendor_entry AS SIGNED)
        WHERE vi.reference_depth < {MAX_VENDOR_REFERENCE_DEPTH} AND vi.reference_cycle = 0
    ) """
    diagnostics = _mysql_rows(cte + f"""SELECT JSON_OBJECT(
        'cycle_detected', COALESCE(MAX(vi.reference_cycle), 0),
        'depth_limit_reached', COALESCE(MAX(vi.reference_depth = {MAX_VENDOR_REFERENCE_DEPTH}
            AND vi.reference_cycle = 0 AND EXISTS (
                SELECT 1 FROM npc_vendor parent WHERE parent.item = -CAST(vi.vendor_entry AS SIGNED))), 0)
        ) FROM vendor_items vi""", [item_parameter], config_path)
    if len(diagnostics) != 1:
        raise QueryUnavailable("query_result_invalid")
    cycle = bool(diagnostics[0].get("cycle_detected"))
    depth_limit = bool(diagnostics[0].get("depth_limit_reached"))

    rows = _mysql_rows(cte + f"""SELECT record FROM (SELECT DISTINCT JSON_OBJECT(
        'item_id', it.entry, 'item_name', it.name, 'buy_count', it.BuyCount,
        'base_buy_price_copper', it.BuyPrice,
        'maxcount', vi.maxcount, 'incrtime_seconds', vi.incrtime, 'extended_cost_id', vi.ExtendedCost,
        'catalog_entry', vi.catalog_entry,
        'vendor_entry', ct.entry, 'vendor_name', ct.name, 'npc_flags', ct.npcflag,
        'spawn_guid', s.guid, 'map_id', s.map, 'zone_id', {zone_column}, 'area_id', {area_column},
        'position', CASE WHEN s.guid IS NULL THEN NULL
            ELSE JSON_ARRAY(s.position_x, s.position_y, s.position_z) END,
        'orientation', s.orientation, 'phase_mask', s.phaseMask, 'spawn_mask', s.spawnMask,
        'provenance', 'static_spawn', 'loaded', 'unknown', 'current_stock', 'unknown'
    ) AS record, ct.entry AS sort_vendor, it.entry AS sort_item,
        vi.ExtendedCost AS sort_cost, vi.maxcount AS sort_count, vi.incrtime AS sort_time,
        vi.catalog_entry AS sort_catalog, s.guid AS sort_spawn
    FROM vendor_items vi JOIN creature_template ct ON ct.entry = vi.vendor_entry
      JOIN item_template it ON it.entry = vi.item_id
      LEFT JOIN creature s ON ({spawn_join})
    WHERE {' AND '.join(filters)}
    ) offers
    ORDER BY sort_vendor, sort_item, sort_cost, sort_count, sort_time, sort_catalog, sort_spawn
    LIMIT ? OFFSET ?""", [item_parameter, *filter_parameters, limit + 1, offset], config_path)
    more = len(rows) > limit
    page = rows[:limit]
    return {"operation": "vendors", "provenance": "static_spawn", "catalog_source": "npc_vendor",
            "stock_scope": "configured_catalog; current stock, availability and player eligibility unknown",
            "price_scope": "item_template base price; discounts and extended-cost requirements not evaluated",
            "reference_cycle_detected": cycle, "reference_depth_limit_reached": depth_limit,
            "reference_resolution_truncated": cycle or depth_limit,
            "offset": offset, "returned_count": len(page),
            "next_offset": offset + len(page) if more else None,
            "results": page, "truncated": more or cycle or depth_limit}


def _bounded_result(result: dict[str, Any]) -> dict[str, Any]:
    result["truncated"] = bool(result.get("truncated", False))
    while len(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode("utf-8")) > MAX_OUTPUT_BYTES:
        rows = result.get("results")
        if not isinstance(rows, list) or not rows:
            result["results"] = []
            result["truncated"] = True
            break
        rows.pop()
        result["truncated"] = True
        if result.get("operation") == "vendors":
            result["returned_count"] = len(rows)
            result["next_offset"] = result["offset"] + len(rows)
    return result


def run_query(operation: str, args: dict[str, Any], config_path: str = DEFAULT_CONFIG) -> dict[str, Any]:
    provenance = "static_spawn" if operation in {"spawns", "vendors"} else "static_template"
    if operation == "fishing":
        provenance = "mixed_static"
    if operation not in {"creature", "gameobject", "item", "quest", "spell", "spawns", "loot", "fishing", "vendors"}:
        return {"operation": str(operation)[:40], "provenance": "none", "results": [], "truncated": False,
                "error": "unknown_operation"}
    reference_resolution_truncated = False
    reference_resolution_error = None
    try:
        limit = _limit(args)
        if operation == "vendors":
            return _bounded_result(_vendors(args, limit, config_path))
        if operation in {"creature", "gameobject", "item", "quest"}:
            rows = _lookup_template(operation, args, limit, config_path)
            truncated = len(rows) > limit
            results = rows[:limit]
        elif operation == "spell":
            rows = _spell(args, limit, config_path)
            truncated = len(rows) > limit
            results = rows[:limit]
        elif operation == "spawns":
            rows = _spawns(args, limit, config_path)
            truncated = len(rows) > limit
            results = rows[:limit]
        elif operation == "loot":
            rows = _loot(args, limit, config_path)
            truncated = len(rows) > limit
            results = rows[:limit]
            try:
                results, reference_resolution_truncated = _resolve_reference_contents(results, config_path)
            except QueryUnavailable as error:
                reference_resolution_error = str(error)
        else:
            results, truncated = _fishing(args, limit, config_path)
            try:
                results, reference_resolution_truncated = _resolve_reference_contents(results, config_path)
            except QueryUnavailable as error:
                reference_resolution_error = str(error)
        result = {"operation": operation, "provenance": provenance,
                  "results": results, "truncated": truncated or reference_resolution_truncated}
        if operation in {"loot", "fishing"}:
            result["reference_resolution_truncated"] = reference_resolution_truncated
            if reference_resolution_error:
                result["reference_resolution_error"] = reference_resolution_error
        return _bounded_result(result)
    except QueryInputError as error:
        return {"operation": operation, "provenance": provenance, "results": [], "truncated": False,
                "error": str(error)}
    except QueryUnavailable as error:
        return {"operation": operation, "provenance": provenance, "results": [], "truncated": False,
                "error": str(error)}
    except Exception:
        return {"operation": operation, "provenance": provenance, "results": [], "truncated": False,
                "error": "query_failed"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("creature", "gameobject", "spawns", "quest", "fishing", "loot", "item", "spell", "vendors"))
    parser.add_argument("--config", default=DEFAULT_CONFIG, help=argparse.SUPPRESS)
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
    parser.add_argument("--limit", default=20)
    parser.add_argument("--offset")
    parsed = parser.parse_args()
    args = {key: value for key, value in vars(parsed).items() if key not in {"operation", "config"} and value is not None}
    result = run_query(parsed.operation, args, parsed.config)
    print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    return 1 if "error" in result else 0


if __name__ == "__main__":
    raise SystemExit(main())

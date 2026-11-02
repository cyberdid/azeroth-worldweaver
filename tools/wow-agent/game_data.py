"""Read-only client data used to explain static world records: area names, the
area under a coordinate (from the server's terrain .map files) and crafting
recipes from Spell.dbc. Files are parsed lazily and cached per process."""

from __future__ import annotations

import functools
import struct
from pathlib import Path

GRID_SIZE = 533.3333333
CENTER_GRID = 32
SPELL_EFFECT_CREATE_ITEM = 24
MAP_AREA_NO_AREA = 0x0001


def _dbc(path: Path) -> tuple[list[tuple[int, ...]], bytes]:
    data = path.read_bytes()
    magic, records, fields, record_size, _string_size = struct.unpack_from("<4s4I", data)
    if magic != b"WDBC" or fields * 4 != record_size:
        raise ValueError(f"unsupported_dbc:{path.name}")
    layout = struct.Struct(f"<{fields}I")
    rows = [layout.unpack_from(data, 20 + index * record_size) for index in range(records)]
    return rows, data[20 + records * record_size:]


def _string(strings: bytes, offset: int) -> str:
    end = strings.find(b"\0", offset)
    return strings[offset:end if end >= 0 else len(strings)].decode("utf-8", "replace")


@functools.lru_cache(maxsize=4)
def areas(data_dir: str) -> dict[int, tuple[str, int]]:
    """area id -> (name, parent zone id; 0 for a zone itself)."""
    rows, strings = _dbc(Path(data_dir) / "dbc" / "AreaTable.dbc")
    return {row[0]: (_string(strings, row[11]), row[2]) for row in rows}


@functools.lru_cache(maxsize=4)
def maps(data_dir: str) -> dict[int, str]:
    rows, strings = _dbc(Path(data_dir) / "dbc" / "Map.dbc")
    return {row[0]: _string(strings, row[5]) for row in rows}


def area_label(data_dir: str, area_id: int | None) -> str | None:
    """"Area, Zone" for a subarea, or the zone name itself."""
    if not area_id:
        return None
    known = areas(data_dir)
    name, parent = known.get(area_id, (None, 0))
    if name is None:
        return None
    if parent and parent in known:
        return f"{name}, {known[parent][0]}"
    return name


@functools.lru_cache(maxsize=512)
def _tile_areas(data_dir: str, map_id: int, grid_x: int, grid_y: int) -> tuple[int, ...] | None:
    path = Path(data_dir) / "maps" / f"{map_id:03}{grid_x:02}{grid_y:02}.map"
    try:
        with path.open("rb") as stream:
            header = stream.read(20)
            _magic, _version, _build, area_offset, _area_size = struct.unpack("<4sII2I", header)
            stream.seek(area_offset)
            fourcc, flags, grid_area = struct.unpack("<4sHH", stream.read(8))
            if fourcc != b"AREA":
                return None
            if flags & MAP_AREA_NO_AREA:
                return (grid_area,)
            return struct.unpack("<256H", stream.read(512))
    except (OSError, struct.error):
        return None


def area_at(data_dir: str, map_id: int, x: float, y: float) -> int | None:
    """Terrain area under a point. City models inside terrain (Ironforge,
    Undercity) report the surrounding zone; prefer server zone/area columns."""
    fx = CENTER_GRID - x / GRID_SIZE
    fy = CENTER_GRID - y / GRID_SIZE
    grid_x, grid_y = int(fx), int(fy)
    if not (0 <= grid_x < 64 and 0 <= grid_y < 64):
        return None
    tile = _tile_areas(data_dir, map_id, grid_x, grid_y)
    if not tile:
        return None
    if len(tile) == 1:
        return tile[0] or None
    return tile[(int(fx * 16) & 15) * 16 + (int(fy * 16) & 15)] or None


@functools.lru_cache(maxsize=2)
def _crafting_index(data_dir: str) -> tuple[dict[int, list[dict]], dict[int, tuple[int, int, int, int]], dict[int, str]]:
    rows, strings = _dbc(Path(data_dir) / "dbc" / "Spell.dbc")
    created: dict[int, list[dict]] = {}
    for row in rows:
        for effect in range(3):
            item = row[107 + effect]
            if row[71 + effect] != SPELL_EFFECT_CREATE_ITEM or not item:
                continue
            reagents = [(row[52 + i], row[60 + i]) for i in range(8) if row[52 + i]]
            created.setdefault(item, []).append({
                "spell_id": row[0], "spell_name": _string(strings, row[136]),
                "reagents": reagents, "creates_min": row[80 + effect] + 1,
            })
    skill_rows, _ = _dbc(Path(data_dir) / "dbc" / "SkillLineAbility.dbc")
    # SkillLineAbility: skill line, learn rank, yellow (fewer skill-ups) and grey (none) ranks.
    skills = {row[2]: (row[1], row[7], row[11], row[10]) for row in skill_rows}
    line_rows, line_strings = _dbc(Path(data_dir) / "dbc" / "SkillLine.dbc")
    names = {row[0]: _string(line_strings, row[3]) for row in line_rows}
    return created, skills, names


def crafting_recipes(data_dir: str, item_id: int) -> list[dict]:
    """Spells that create item_id, with reagents and the profession skill."""
    created, skills, names = _crafting_index(data_dir)
    recipes = []
    for recipe in created.get(item_id, []):
        skill_id, min_rank, yellow, grey = skills.get(recipe["spell_id"], (0, 0, 0, 0))
        recipes.append(dict(recipe, skill_id=skill_id or None,
                            skill_name=names.get(skill_id) if skill_id else None,
                            skill_min_rank=min_rank if skill_id else None,
                            skill_yellow_at=yellow or None, skill_grey_at=grey or None))
    return recipes

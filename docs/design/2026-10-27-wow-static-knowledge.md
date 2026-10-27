# Read-Only AzerothCore World Knowledge

Date: 2026-10-27
Parent design: [World perception architecture](2026-10-27-wow-world-perception-design.md)

## Goal

Let both agents answer detailed questions about quests, creatures, objects, fishing, loot, and map data using bounded, read-only queries against the server's existing world data.

## Design

- Provide one local query helper with named operations and typed arguments; agents cannot submit arbitrary SQL.
- Resolve creature/game-object templates, quest objectives/prerequisites/rewards, fishing skill and pool data, loot templates, and static spawn coordinates. Include DBC names and spell/item references where available.
- Restrict queries to the current map/zone when possible, order by relevance/distance, and cap rows and response bytes.
- Mark each returned fact as `static_template`, `static_spawn`, or `inferred`; only the shared live snapshot can mark an object as `live` or prove it is loaded.
- Use a read-only connection path sourced at runtime from the existing server configuration. Do not print, log, or add credentials to the repo or agent prompt. Use prepared parameters and fixed SQL per operation. Do not mutate any database row.
- Both agents must be able to request the same named lookups. Codex can call the local helper from its full-access bridge; Claude must use a bounded request/response relay inside the shared telemetry directory because its shell cannot read `$WOW_ROOT`. Keep database credentials only in the bridge process and never copy them into the shared directory.
- Keep pathing claims separate: database spawn distance is straight-line/static; navmesh reachability is reported only if a real path query succeeds.

## Confirmed local schema

- Creature templates and spawns: `creature_template` (`entry`, `name`, levels, faction, flags, loot id) and `creature` (`id1`, `map`, `zoneId`, `areaId`, coordinates, `phaseMask`). `creature` has an index on `id1` and `map`.
- Game-object templates and spawns: `gameobject_template` (`entry`, `type`, `name`, `displayId`, `Data0`–`Data23`) and `gameobject` (`id`, `map`, `zoneId`, `areaId`, coordinates, `phaseMask`). The base `gameobject` table has only its `guid` primary key, so spatial lookups must stay on demand and have a timeout/result cap.
- Quests: `quest_template` (`ID`, `LogTitle`, descriptions, objectives, required NPC/game-object and item IDs/counts, rewards) plus `quest_template_addon` (previous/next quest, exclusive group, skill and reputation requirements). Creature/game-object start and end relations use `creature_queststarter`, `creature_questender`, `gameobject_queststarter`, and `gameobject_questender`.
- Items and spells: `item_template` and `spell_dbc`; the DBC table includes localized names/descriptions and effect fields.
- Loot: `creature_loot_template`, `gameobject_loot_template`, `fishing_loot_template`, and `reference_loot_template`, with item rows resolved through `item_template`. Reference rows are expanded into bounded nested rows with depth, cycle, row, output-size, and query-time limits; raw per-table chances stay separate.
- Fishing: AzerothCore loads `fishing_loot_template.Entry` as an area ID (`LootMgr.cpp`); static resource grouping uses `pool_gameobject`, `pool_creature`, `pool_pool`, and `pool_template`. These records describe configured pools, not proof of a currently loaded fishing hole.
- The local CLI client is `/usr/bin/mysql` (MySQL 8.4 client); no Python MySQL connector is installed. The helper therefore uses the MySQL CLI with a temporary permission-restricted defaults file, fixed server-side prepared statements, and a read-only transaction. It never prints or logs the credentials it reads from `WorldDatabaseInfo`.

## Acceptance

- Each named operation returns compact, ranked, parameterized results and rejects unknown operations or invalid identifiers.
- Queries run outside the world tick and never mutate data.
- Agents can distinguish static knowledge from loaded live state and label unavailable map/path data honestly.
- A natural-language task addressed to either agent can use the same bounded static lookup path without requiring the user to type a separate command.

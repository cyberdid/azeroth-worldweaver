# Shared WoW World State Stream

Date: 2026-10-27
Parent design: [World perception architecture](2026-10-27-wow-world-perception-design.md)

## Goal

Give Claude and Codex the same fresh, bounded, live view of their current group and nearby loaded world, plus meaningful changes as they happen.

## Current gap

The `WorldScript` observer/writer is compiled, installed, and running on the author's private server. After the latest restart, a fresh group snapshot confirmed inventory counters/items for all three members and attackability/trigger fields for all nearby creatures, with zero event drops. Both agent adapters read the shared format; a new in-game user message is still needed to confirm their post-restart responses.

## Design

- Add a `WorldScript` observer that finds online groups containing configured bridge bots, samples them at a configurable interval (default 1000 ms), and serializes only bounded data on the world thread.
- Keep a latest snapshot per bridge group. Include schema version, snapshot sequence, latest event sequence, capture time, group/map/instance identity, member vitals/position/combat/target/cast, selected gear, backpack and equipped-bag item stacks (with bag/slot and an explicit truncation flag), skills/auras/quests, and up to a configurable number of nearby loaded creatures and game objects in the same map, instance, and core-defined phase relation.
- Preserve `live` provenance on observed objects. Distances, line of sight, phase, combat/victim, and object state describe loaded objects only.
- Creature records include live NPC service-role names, raw NPC flags, creature type, rank, faction, an explicit world-trigger flag, and whether the nearest group member can validly attack it. Keep faction hostility, target validity, and line of sight as separate facts. Game-object records include type, fishing-node/fishing-hole indicators, loot state, and spawned state. Member context includes fishing skill and main-hand item; selected targets include player/creature kind, hostility, combat, and line of sight; casts and auras include localized names. NPC service flags are clues only and do not prove that a particular quest is currently available.
- Emit compact event records for map/area change, target/combat/cast, player death/kill, quest, inventory/equipment, loot, fishing/skill, and nearby spawn/despawn/state changes. Events use a separate strictly increasing `event_seq`; snapshots reference the highest event sequence they include.
- Use a bounded asynchronous writer. It atomically replaces the snapshot file and appends newline-delimited event JSON; it rotates bounded history and exposes dropped-event counts. World-thread code does not touch disk or query databases.
- Map-update hooks use a world-thread-refreshed cache of group member GUIDs, map/instance/phase identity, and nearby positions to discard unrelated activity before queueing. Relevant facts enter bounded per-group queues; the world thread confirms the group is still active, assigns sequence numbers, updates recent summaries, and hands records to the asynchronous file writer. Spawn interest is mirrored at enqueue time so a despawn before queue draining is still observed. Hooks never mutate world-thread observer maps directly.
- Store chat logs under `ClaudeBridge.LogDir` and runtime state under `ClaudeBridge.TelemetryDir` (falling back to `LogDir`) with configurable interval, radius, per-snapshot object cap, queue cap, and event-log size cap.
- Do not append event records as separate chat-log lines. When a real user message is logged for Claude, append a compact cached observation and the shared snapshot path to that same record. Codex receives the plain user message and reads the shared snapshot plus events since its cursor when the message arrives.
- Existing `BOTSAY` lines remain reserved for bot speech. A telemetry record never triggers an agent response.
- Codex selects only the newest snapshot whose group contains the `Codex` member. Its reader reports confirmed journal gaps and writer/observer drop-counter increases separately from a backlog where the snapshot is ahead of the asynchronously written event journal.
- Prompt-size compaction removes auras and older events first; it preserves equipment and inventory where possible, and explicitly marks any context-only inventory or nearby-object truncation.
- Store the shared telemetry directory under Claude's active working tree (default deployment path: `~/.local/state/azeroth-worldweaver`) so both agents can read it without changing AppArmor policy.

## Failure and load behavior

Missing group/member/map is explicit. Snapshot replacement must preserve the last good file on write failure. Queue saturation drops/coalesces low-priority records, increments a visible counter, and never waits on the world thread. Readers detect event cursor gaps, observer/writer drops, pending event backlog, and stale timestamps separately.

## Acceptance

- Both agents can cite the same schema, group, snapshot sequence, capture time, and event sequence.
- Both agents receive the same session, snapshot sequence, and event sequence on a real user task; no standalone telemetry record triggers a response.
- Current state refreshes without chat; event history and nearby-object output stay bounded.
- Static spawn records are not presented as loaded live objects.
- No synchronous filesystem or SQL work occurs in the world-tick observer.

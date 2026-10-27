# Shared WoW World State Stream Implementation Plan

> **For agentic workers:** Use the `executing-plans` skill and work inline in the current checkout. Complete tasks in order; do not touch the three pre-existing spell changes.

**Goal:** Publish one bounded, fresh world snapshot and event stream that both agents can consume without treating telemetry as chat.

**Architecture:** The existing bridge module samples bridge-bot groups on the world thread and serializes bounded JSON. A bounded worker owns all runtime file writes; Claude gets a compact cached observation on the same line as a real user message, while Codex reads the same snapshot and event journal before answering.

**Tech Stack:** AzerothCore C++ `WorldScript`/`PlayerScript`/`UnitScript` hooks, newline-delimited JSON, C++ thread/condition variable, Python 3 standard library.

**Spec:** `../design/2026-10-27-wow-world-state-stream.md`

## Global Constraints

- Keep the pre-existing edits in `Spell.cpp`, `SpellInfoCorrections.cpp`, and `spell_generic.cpp` unchanged and unstaged.
- Keep world-thread work bounded; do not do file I/O, database access, screenshot conversion, or model calls there.
- Preserve the current `BOTSAY` meaning for companion speech; never send telemetry as a standalone chat record.
- Keep runtime state under the configured bridge log directory and expose no credentials.
- Ask before rebuilding or restarting `acore-worldserver`, per `$WOW_ROOT/AGENTS.md`.

---

### Task 1: Add the bounded runtime writer

**Files:**
- Modify: `.gitignore`
- Create: `modules/mod-claude-bridge/src/world_state_writer.h`
- Create: `modules/mod-claude-bridge/src/world_state_writer.cpp`
- Create: `modules/mod-claude-bridge/src/world_state_observer.cpp`
- Modify: `modules/mod-claude-bridge/src/claude_bridge.cpp`
- Modify: `modules/mod-claude-bridge/src/claude_bridge_loader.cpp`

**Interfaces:**
- `WorldStateWriter::Start(logDirectory, telemetryDirectory, queueLimit, eventLogBytes)` starts one writer thread; an empty telemetry directory falls back to the log directory.
- `WorldStateWriter::PublishSnapshot(std::string groupKey, std::string json)` replaces the pending snapshot for that group without waiting.
- `WorldStateWriter::PublishEvent(std::string groupKey, std::string eventJson)` enqueues a bounded event without turning it into chat. Map-thread hooks first enqueue bounded in-memory event facts for world-thread group/proximity resolution.
- `WorldStateWriter::Stop()` drains accepted work and joins the writer thread.

- [ ] Define `SnapshotRecord`, `EventRecord`, queue limits, file naming, and atomic snapshot replacement in the header.
- [ ] Implement one overwriteable pending snapshot per group and a capped event queue guarded by a mutex/condition variable.
- [ ] Start and stop the writer from `WorldScript::OnStartup` and `WorldScript::OnShutdown`.
- [ ] On the worker thread only, write `world-state-<group>.json` and append `world-events-<group>.jsonl` under `TelemetryDir`; rotate event files at the configured cap. Do not turn events into chat-log lines.
- [ ] Preserve the previous snapshot if a temporary write or rename fails; increment a visible dropped-event counter when a full event queue rejects a record.
- [ ] Build the C++ target only after the user approves the rebuild; record the command and result in the handoff.

### Task 2: Sample live group state and emit events

**Files:**
- Modify: `modules/mod-claude-bridge/src/claude_bridge.cpp`
- Modify: `modules/mod-claude-bridge/conf/claude_bridge.conf.dist`

**Interfaces:**
- `BuildSnapshot(GroupContext const&, uint64 snapshotSeq, uint64 eventSeq)` returns one schema-versioned JSON snapshot for the common group.
- `PublishEvent(groupKey, type, actor, mapId, instanceId, zoneId, payload, detail)` assigns a per-group increasing `event_seq` and queues the record for file output.
- `ClaudeBridgeWorldScript::OnUpdate(uint32 diff)` samples each distinct bridge group at the configured interval.

- [ ] Add config for `TelemetryDir`, `TelemetryIntervalMs`, `TelemetryRadius`, `TelemetryMaxObjects`, `TelemetryEventQueueLimit`, and `TelemetryEventLogBytes` with bounded defaults.
- [ ] Resolve only configured online bridge bots, deduplicate by group GUID, and collect members on the world thread.
- [ ] Extend the current serializer with schema version, sequence/freshness fields, group/map/instance identity, member location/vitals/target/cast, relevant skills, equipped items, auras, active quests, and bounded nearby loaded same-phase units/game objects.
- [ ] Label nearby entries `source: live`; include phase, distance, line of sight, combat/victim or object state, and nearest group member.
- [ ] Register `PlayerScript`, `UnitScript`, `AllCreatureScript`, and `AllGameObjectScript` hooks for relevant map, combat, cast, death/kill, quest, loot, skill/fishing, equipment, and nearby spawn/state changes.
- [ ] Filter world-object hooks to objects near an active bridge group in the same map, instance, and core-defined phase relation; retain accepted spawn interest until despawn is processed. Never claim a static database spawn is loaded.
- [ ] Keep chat hooks limited to recording the incoming chat line plus the latest cached compact summary for Claude; the periodic observer is the only snapshot producer and chat hooks do not scan world objects or do file I/O.

### Task 3: Make both adapters consume the common stream

**Files:**
- Create: `tools/wow-agent/world_state.py`
- Create: `tools/wow-agent/codex-bridge.py`
- Modify on deployment: `$WOW_AGENT_SERVER_DIR/codex-bridge.py`

**Interfaces:**
- `read_latest_snapshot(directory, group=None, member_name=None) -> dict | None` validates schema, optionally pins the member's group, and reports age.
- `read_events(directory, group, session_id, after_event_seq, limit=100) -> (list[dict], int, bool, int)` returns matching events, the next cursor, a journal-gap flag, and the highest available sequence.
- `build_agent_context(directory, cursor, event_limit=40, member_name=None) -> (str, dict)` returns bounded state/events and an updated cursor; the Codex service pins it to member `Codex`.
- Codex prompt context contains one current snapshot plus unseen compact events; those records are excluded from chat history and never invoke a response alone.

- [ ] Add a standard-library reader with schema checks, stale-state handling, event limits, and gap detection. Since the live bridge starts with `python3 -I`, load sibling helpers by their explicit file path instead of relying on the current directory being importable.
- [ ] Teach the Codex adapter to read the shared state at user-message time and keep only bounded summaries/cursors in history.
- [ ] Keep companion `BOTSAY` lines as speech context and do not merge full JSON snapshots into the 40-line conversation history.
- [ ] Add `tools/wow-agent/README.md` with the source-to-live deployment mapping and restart command for only the bridge consumer.
- [ ] Install the tracked adapter into the live directory with a timestamped backup, then restart only `codex-bridge`; do not restart worldserver as part of this task.

### Task 4: In-game operational validation

**Files:**
- Modify: `./2026-10-27-wow-world-state-stream.md` only if observed behavior changes the contract.

- [ ] After explicit approval, rebuild and restart worldserver.
- [ ] Join the configured user characters with Codex and Claude; confirm both report the same `schema_version`, `snapshot_seq`, and timestamp.
- [ ] Move between areas and enter/exit combat; confirm both agents see the same state/event sequences on their next user-task response and neither replies to background telemetry alone.
- [ ] Inspect queue/drop counters and bounded event rotation while playing; record measured cadence and any gaps.
- [ ] Mark acceptance criteria complete only for behavior observed in the live server.

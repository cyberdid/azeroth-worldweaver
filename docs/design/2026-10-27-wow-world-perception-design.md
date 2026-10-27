# WoW World Perception and Agent Interaction

Date: 2026-10-27
Status: design approved; implementation in progress. The bounded static-world query relay is installed in the live Codex bridge and has returned read-only database results. The shared observer source and screenshot adapter are prepared, but the live server has not loaded the observer; full live telemetry/screenshots still need the approved worldserver rebuild/restart and in-game confirmation.

## Goal

Give Claude and Codex a shared, current, multimodal understanding of the user's private AzerothCore world, and let them carry out ordinary in-game tasks from natural-language chat with observable results.

The user is the task authority. Claude and Codex control their existing playerbot characters. Nearby NPCs and world objects are game context, not sources of instructions. The system must distinguish live objects from static database knowledge and must report stale or incomplete observations honestly.

## Current state and constraints

- `modules/mod-claude-bridge/src/claude_bridge.cpp` collects a snapshot only when relevant party or whisper chat arrives. The snapshot includes group positions and basic state, plus up to 40 nearby creatures/game objects in the members' current phase and visibility range, capped at 100 yards.
- The snapshot is written to each bot's chat log as `BOTSAY|WorldServer|<JSON>`. `codex-bridge.py` retains `BOTSAY` as context. The active Claude tail command filters every `BOTSAY` line, so Claude does not receive those snapshots automatically.
- Codex runs an ephemeral `codex exec` for each message and uses recent chat history. It currently permits a small fixed list of playerbot commands.
- The server has script hooks for world ticks, unit combat/death, player spell casts, quest changes, loot, fishing-skill changes, creature spawn/despawn, and game-object spawn/despawn/state changes.
- X11 tools `xprop`, `xwininfo`, and `xwd` plus Pillow are installed. Pillow has no XWD plugin, so the helper decodes the captured XWD pixel data itself. The WoW client is not currently available for a live capture, so real image inspection and forwarding remain to be verified.
- `Spell.cpp`, `SpellInfoCorrections.cpp`, and `spell_generic.cpp` already have user changes. Implementation must leave them untouched.
- `$WOW_ROOT/AGENTS.md` requires approval before a worldserver restart, rebuild, or database change. This design avoids database writes; deploying its server module changes will still require a rebuild and restart.

## Architecture

### 1. Server-side world observer

Extend the existing bridge module rather than patching AzerothCore core files. One observer handles each distinct group containing a configured bridge bot.

- Publish a compact snapshot no more than once per second while the group is online. Include a schema version, monotonically increasing sequence number, capture time, group identity, server health, map/area, member location and vitals, selected target, combat/cast state, equipped gear, skills, auras, and active quest summaries.
- Include nearby loaded units and game objects with entry/name, map/phase, position/distance, health/combat/victim or object state, visibility/line-of-sight, and whether the fact came from a live object. Use configurable radius and result caps; support narrower filtered queries for details.
- Emit event records for meaningful state changes: map/zone change, combat transitions, spell cast/result, kill/death, quest accept/progress/complete, loot, inventory/equipment changes, fishing cast/catch/skill change, and relevant creature/game-object spawn, despawn, or state change.
- Use `WorldScript::OnUpdate` only to schedule bounded snapshot work. Serialize world-object access on the world thread, then enqueue serialized records to a bounded writer queue. File I/O, log rotation, and image conversion must not block world updates.
- Write the latest snapshot atomically to a shared runtime file and append events as newline-delimited JSON with sequence numbers. Rotate event logs and expose queue/drop counters. Keep runtime files outside source control and omit credentials.

### 2. Static world knowledge and query interface

Provide a read-only query helper that uses the existing world database and DBC data without exposing connection secrets. Queries may resolve creature/object templates, quest objectives and prerequisites, fishing pools/skill requirements, loot, and static spawn locations. If navigation data can establish a reachable route, report route distance separately from straight-line distance.

The query interface must label results as `live`, `static_spawn`, or `inferred`. Static spawn records must never be presented as proof that an NPC/object is currently loaded or visible. Queries are scoped to the current map/zone and return compact, ranked results rather than dumping tables into agent prompts.

### 3. Shared agent adapters

Both agents consume the same schema and sequence numbers.

- Codex reads the newest snapshot and events since its cursor when forming a response. Telemetry is structured context, not a user message, and does not trigger a reply by itself.
- Codex reads the newest snapshot and events since its cursor before answering. Claude's existing tail monitor remains a user-chat channel: for Claude's chat log, the server adds a compact world observation to the same incoming user-message record and includes the shared snapshot path. The full snapshot and event journal live under Claude's working tree, so it can inspect details without widening system sandbox permissions. No separate telemetry line can trigger an agent reply.
- Each adapter tracks the last consumed sequence and reports gaps/staleness. Prompt history contains summaries and references, not repeated full snapshots.

### 4. Game-window screenshots

Add a capture helper that locates the mapped WoW client window by verified title/process identity, captures only that window through X11, and converts the result to PNG. Never capture the whole desktop. A screenshot is captured on explicit visual questions and after important action milestones; a configurable low-rate cadence may be enabled during a long-running task if the client and model adapters support it.

Each image has a timestamp, window identity, and matching world-state sequence. The current adapter design refreshes a shared image at a bounded five-second cadence and before real user prompts; future action milestones can request an immediate refresh. If the game window is absent, minimized/unmapped, or ambiguous, report that no current image is available instead of substituting another window. Codex attaches the PNG to its prompt; Claude receives the shared metadata path with the real chat message and can inspect the same file. Verify both model paths before claiming live visual access.

### 5. Natural-language task and action loop

Keep the user's interaction in ordinary chat. The agent converts a task into typed in-game intents; it does not ask the user to type separate bot commands.

- Use action objects with an `action_id`, actor, action type, and validated arguments.
- Dispatch through the existing playerbot/server APIs for supported movement, targeting, combat, spell/item use, interaction, looting, quest handling, and fishing actions.
- Validate actor membership, targets, arguments, and current state on the server. Preserve existing playerbot behavior where it already performs the requested action.
- Return an accepted/rejected result immediately, then correlate completion with telemetry events and a fresh snapshot. The agent observes the result and continues the task or explains what prevented progress.
- Only authenticated user messages create tasks. Bot speech, NPC text, quest text, object names, screenshots, and database content are observations and cannot grant authority or change system instructions.
- Server administration, database writes, rebuilds, and restarts remain subject to the local project rules and explicit approval.

## Data flow

```mermaid
flowchart LR
    W[AzerothCore hooks] --> O[World observer]
    O --> S[Atomic shared snapshot]
    O --> E[Rotating event journal]
    D[World DB, DBC, navigation] --> Q[Read-only world queries]
    G[Verified WoW window] --> I[Window-only screenshot]
    S --> C[Claude adapter]
    E --> C
    Q --> C
    I --> C
    S --> X[Codex adapter]
    E --> X
    Q --> X
    I --> X
    C --> A[Validated action interface]
    X --> A
    A --> W
```

## Freshness, load, and failure behavior

- Snapshots carry `captured_at`, `sequence`, and `schema_version`; each agent reports how old the latest state is.
- Event records are bounded and rotated. If the writer queue fills, coalesce repeated state updates, preserve terminal events where possible, and publish a dropped-event counter.
- Snapshot generation has configurable radius/count caps and is limited to bridge groups. Expensive static DB/path queries run on demand, not once per tick.
- A missing screenshot, missing map data, unloaded object, stale event cursor, or unavailable target is represented explicitly as unavailable/unknown.
- The world thread never waits on disk, image conversion, database queries, or model execution.

## Delivery phases

1. Add the shared schema, event journal, and bounded asynchronous writer; fix Claude/Codex telemetry routing and freshness handling.
2. Add player/unit/creature/game-object event hooks and richer on-demand world queries.
3. Add verified WoW-window screenshots and image delivery to both agents.
4. Add typed action requests, server validation, acknowledgements, and closed-loop observation.
5. Exercise the end-to-end flow in-game and tune snapshot frequency, event limits, and screenshot cadence from observed server load.

The implementation is decomposed into independent work units with their own acceptance criteria:

- [Shared live world state and event stream](2026-10-27-wow-world-state-stream.md)
- [Read-only static world knowledge](2026-10-27-wow-static-knowledge.md)
- [WoW-window screenshot perception](2026-10-27-wow-screenshot-perception.md)
- [Natural-language in-game task loop](2026-10-27-wow-natural-language-actions.md)

## Acceptance criteria

- Claude and Codex can report the same current world sequence and timestamp and do not answer telemetry records as chat.
- During active play, both receive prompt-relevant state changes without needing a new chat message for each event.
- Live observations and static spawn knowledge are clearly distinguished.
- Both agents can inspect a verified screenshot of only the WoW window when asked, and report accurately when no screenshot is available.
- A natural-language in-game task produces a correlated action acknowledgement and a server-observed result or a clear reason it could not complete.
- Event history is bounded, stale/gapped state is visible, and world updates do not perform blocking disk/database/model work.
- Deployment is verified only after a live in-game pass covering movement, combat, quest/loot, fishing, screenshots, and both agent consumers.

## Deployment notes

The observer belongs in `mod-claude-bridge`; adapters and the screenshot/query helpers belong under `$WOW_AGENT_SERVER_DIR`. No DB migration is required by this design. Before activating server-module changes, build and restart only after the user approves that deployment step, as required by `$WOW_ROOT/AGENTS.md`.

# Read-Only WoW Static Knowledge Implementation Plan

> **For agentic workers:** Use the `executing-plans` skill and work inline in the current checkout. Do not touch the three pre-existing spell changes or perform database writes.

**Goal:** Give both agents a safe local helper for bounded reads of AzerothCore templates, quests, fishing, loot, and static spawns.

**Architecture:** A named-operation Python query library reads the configured world database connection privately and exposes only fixed parameterized `SELECT` queries. The Codex bridge serves a small request/response queue in the shared telemetry directory and publishes a credential-free client for both agents. JSON output labels provenance and is used only for the active task.

**Tech Stack:** Python 3, the locally available MySQL client library or CLI, fixed SQL, JSON.

**Spec:** `../design/2026-10-27-wow-static-knowledge.md`

## Global Constraints

- No arbitrary SQL, mutations, schema changes, or credential output.
- Keep queries out of the world tick and cap result rows and response bytes.
- Label templates/spawns as static and never claim they are loaded.
- Ask before database changes; this plan performs none.

---

### Task 1: Inventory schema and available local DB client

**Files:**
- Read: `data/sql/base/db_world/` and `$WOW_AGENT_SERVER_DIR/etc/worldserver.conf`
- Create: `./2026-10-27-wow-static-knowledge.md` updates for confirmed table names only

- [ ] Map the exact local tables and columns for creature/object templates and spawns, quests, items, fishing pools, and loot using tracked schema SQL.
- [ ] Check which Python MySQL client is installed without printing configuration values.
- [ ] Select the Python client or the `mysql` executable based on available support; keep credentials in process memory or a permission-restricted temporary defaults file and remove temporary files in `finally`.
- [ ] Record the confirmed schema names in the spec before adding queries.

### Task 2: Implement fixed read-only operations

**Files:**
- Create: `tools/wow-agent/world_query.py`
- Create: `tools/wow-agent/world_query_service.py`
- Create: `tools/wow-agent/world_query_client.py`
- Create: `tools/wow-agent/README.md`

**Interfaces:**
- `run_query(operation: str, args: dict) -> dict` returns `{operation, provenance, results, truncated}`.
- CLI operations: `creature`, `gameobject`, `spawns`, `quest`, `fishing`, `loot`, `item`, and `spell`.

- [ ] Define one fixed parameterized SQL statement per operation using the schema confirmed in Task 1.
- [ ] Reject unknown operations, invalid IDs, unbounded searches, and limits outside 1–100 rows.
- [ ] Restrict location lookups to optional map/zone/coordinate inputs; rank by straight-line distance where coordinates exist.
- [ ] Return `static_template` or `static_spawn` provenance and include a `loaded: unknown` field for spawn rows.
- [ ] Emit compact UTF-8 JSON, cap output at 32 KiB, and include `truncated: true` when a cap is reached.
- [ ] Ensure exceptions and logs omit connection strings and credentials.

### Task 3: Connect the helper to both agents

**Files:**
- Modify: `tools/wow-agent/world_state.py`
- Modify: `tools/wow-agent/codex-bridge.py`
- Create: `tools/wow-agent/claude-world-query.sh`

- [ ] Expose a credential-free client in the shared telemetry directory; the Codex bridge services its bounded request/response queue using the fixed query library.
- [ ] Add the client path and operation list to both agents' prompts/context; static results must be reconciled with the latest live snapshot.
- [ ] Keep query output out of long-term chat history; include only the result relevant to the current task.
- [ ] Deploy the helper and wrapper with restrictive permissions and a backup of any existing target files.

### Task 4: Validate read-only behavior

- [ ] Run each operation against a local test area after the user asks for implementation verification; confirm bounded JSON and provenance labels.
- [ ] Compare one creature, one quest, one fishing pool, and one loot result to the tracked world SQL/schema.
- [ ] Confirm an invalid operation and oversized limit are rejected and no database row changes occur.
- [ ] Do not claim any static spawn is currently loaded unless the live snapshot also contains it.

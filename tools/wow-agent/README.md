# Agent and telemetry helpers

These local Python and shell helpers connect the Claude and Codex adapters to an AzerothCore installation. Run them as a trusted local operating-system user; do not expose their files or SOAP access to public chat.

## Shared state

Set `ClaudeBridge.TelemetryDir` in the module configuration to a private directory readable by the worldserver and both adapters. The server atomically writes `world-state-<group>.json` snapshots and bounded, rotating `world-events-<group>.jsonl` journals. The readers report snapshot age/sequence, event gaps and truncation rather than treating missing data as proof that something does not exist.

Install these files side by side because the adapters load trusted sibling modules while Python isolated mode is enabled:

- `codex-bridge.py`, `claude-bridge.py`
- `action_client.py`, `session_memory.py`, `world_state.py`
- `world_query.py`, `world_query_service.py`, `world_query_client.py`
- `wow_window_capture.py`

The service publishes one credential-free, fixed-operation query client for both agents. Static game-data lookups are read-only and bounded; static spawns do not prove an object is loaded or visible now.

## Actions and item handoff

The adapters can return typed actions for supported playerbot operations: follow/stay, attack, cast a known active spell, use/equip an exact carried item, fishing, NPC menu interaction, guides, hunts, and item offers. The server validates each request and reports a correlated result. Ordinary conversation is not passed to SOAP as an arbitrary server command.

Hunts use live matching targets and server-validated map/zone/area constraints. Kills are credited by the server; the task controller opens normal corpse loot and waits for inventory results. `collect_loot` keeps drops in the bot's bags. `deliver_loot` is separate and must be explicitly requested. Trade uses the normal WoW trade window; only currently held, tradeable requested items are offered, within the six-slot limit, and the player confirms the exchange. No direct inventory edits or trade acceptance are exposed.

Both adapters share a private `session-memory.json` archive in the telemetry directory. It can bring recent, relevant user and bot messages into a prompt across adapter restarts. The memory is context only: only a current message from an exact character in `WOW_AGENT_AUTHORIZED_PLAYERS` can start or cancel a task.

## Configuration

Copy `config.example.env` to a private environment file (not committed) or configure the same variables in your service manager. Set `WOW_AGENT_SERVER_DIR`, `WOW_SERVER_CONFIG`, a shared private `WOW_AGENT_TELEMETRY_DIR`, distinct `WOW_AGENT_BOT_NAME` values (`Codex` and `Claude`), and the same exact comma-separated `WOW_AGENT_AUTHORIZED_PLAYERS` for both adapters. Keep `WOW_AGENT_CODEX_SANDBOX` at the least permissive setting that supports your workflow.

For SOAP helpers, set `WOW_SOAP_URL` and `WOW_SOAP_CREDENTIAL_FILE`. The credential file must contain `username:password` and have mode `0600`; do not put the value itself in shell history, source control, screenshots, or logs. Keep SOAP local or behind trusted network controls. `claude-cmd.sh` is a local operational helper; it is not exposed as a free-form in-game agent action.

## On-demand visual context

The adapters request `wow_window_capture.py` only after an authorized user asks for visual inspection. It verifies the active, mapped WoW window and process, captures that window only, and associates the image with the matching world snapshot. Hidden, obscured, minimized, ambiguous, stale, or unverifiable windows are refused. No periodic or whole-desktop capture is performed.

The helper needs X11 tools (`xprop`, `xwininfo`, `xwd`) and Pillow. Its output belongs in the private telemetry directory and must not be committed.

## Static world-data queries

`world_query.py` reads the configured server database settings at runtime and runs fixed prepared queries in read-only transactions. It creates a temporary mode-`0600` MySQL options file and removes it after use. It never accepts SQL text from an agent. Operations include `creature`, `gameobject`, `spawns`, `quest`, `fishing`, `loot`, `item`, and `spell`; responses and nested loot references are bounded.

Example:

```bash
WOW_SERVER_CONFIG=/path/to/your/worldserver.conf \
  python3 -I tools/wow-agent/world_query.py creature --name "Wind Serpent"
```

For configuration and integration, see the top-level [README](../../README.md) and [`SECURITY.md`](../../SECURITY.md).

## Vendor browsing and purchase errands

The source includes `vendor_inspect`, `vendor_buy`, `vendor_task_start`, `vendor_task_cancel`, `vendor_travel` and `vendor_travel_cancel` for both adapters. An inspection reports actual native merchant stock and per-bundle prices. Exact item-unit quantities, gold caps and extended-currency consent apply to purchases. The persistent worker finds candidate sellers, walks, uses known paid taxi paths or supported portal/transport graph routes, verifies stock at arrival, purchases and optionally returns without trading automatically.

The read-only `vendors --item-id ID` or `vendors --name SUBSTRING` query locates configured sellers and spawns across the database. Optional arguments are `--entry`, `--map-id`, `--limit`, `--offset`. Static rows never prove current stock or access. Cross-map execution uses only supported native graph edges and verified physical transport transitions; unavailable graph data or geometry produces explicit failure. Runtime route validation is still required.

Install `vendor_runtime.py` and `vendor_tasks.py` beside both adapters. The private `.vendor-tasks/` directory stores in-progress authorization and must be protected and excluded from Git. It survives bridge restarts but tasks do not carry over to a different world session. See [implementation and validation](../../docs/implementation-notes/2026-10-29-vendor-companions.md).

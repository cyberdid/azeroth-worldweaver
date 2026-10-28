<div align="center">

# ✦ Azeroth Worldweaver

### A shared, verifiable view of a private AzerothCore world for Claude and Codex

[![Preview](https://img.shields.io/badge/status-experimental-cc9a42?style=for-the-badge)](#project-status)
[![WoW](https://img.shields.io/badge/world-WotLK%203.3.5a-435fa8?style=for-the-badge)](#requirements)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue?style=for-the-badge)](LICENSE)

**Українською:** Worldweaver — експериментальний міст між приватним сервером AzerothCore та двома AI-компаньйонами. Він передає Claude і Codex спільний контекст зі стану світу, обмежені запити до ігрових даних і метадані перевірених знімків вікна WoW. Повний замкнений цикл ігрових дій ще не завершений.

<img src="assets/worldweaver-architecture.svg" alt="Worldweaver architecture: AzerothCore feeds a shared state stream to Claude and Codex; static queries and verified window captures provide additional context." width="100%">

</div>

## What this project is

Worldweaver is an experimental companion layer for a **self-hosted World of Warcraft: Wrath of the Lich King 3.3.5a server running AzerothCore and mod-playerbots**. It explores how two separate agents can reason from the same game state instead of guessing from chat alone.

The repository contains a custom server bridge, local agent adapters, bounded read-only game-data lookup tools, a verified-window capture helper, and design notes for a richer observer/action loop. It does **not** contain AzerothCore, game client files, maps, a database dump, a live server configuration, or credentials.

## Project status

This is a **public experimental snapshot**, not a ready-to-install release. The repository is published to make the work inspectable and reusable; several pieces still need integration and live validation.

| Capability | State in this snapshot |
|---|---|
| Fixed-operation, read-only world database lookups | Implemented; fishing and loot references can expand into bounded item rows |
| Live nearby-object queries | Typed `nearby` helper filters and pages the latest group snapshot by kind, entry, name, radius, visibility, attackability, or fishing markers; page size is capped at 50 and freshness/truncation are explicit |
| Shared request relay for an agent without database-file access | Available through fixed typed operations; both adapters can use the credential-free client |
| Authenticated chat authority | Both adapters use a configurable exact sender allowlist; empty by default, so no character can start a task until configured |
| World-state snapshot and event writer | Private server rebuilt and restarted with a 500-object cap and 256K context budget. The latest verified pre-deploy snapshot had 100 records and `nearby_truncated=true`; post-restart group telemetry is pending player reconnection, so the new cap and load impact are not yet live-verified. Clean public install is unverified |
| Screenshot capture | Verified locally against a live WoW X11 client; accepts only the active, verified game window and decodes TrueColor/DirectColor channels; Codex attaches frames, and the Claude adapter can read the same validated image path; Claude visual delivery still needs a live check |
| Ordinary-language requests converted into fully validated game actions | Design and initial playerbot action mappings are documented; the typed server dispatcher is not implemented |
| End-to-end in-game validation | Shared telemetry and Codex visual capture were observed live; the Claude chat adapter is running but has not handled a fresh game message yet |

This AzerothCore tree discovers module directories from their source layout and recursively collects source files during CMake configuration; it does not require a module-specific `CMakeLists.txt`. Reconfigure an existing build tree after adding the observer and writer `.cpp` files so CMake collects them. On the author's private host, the observer was compiled from the local Playerbot worktree, installed, restarted, and verified with a fresh group snapshot containing the new inventory and creature fields. This confirms one private deployment only; it is not a clean public install procedure. The `Object.h` compatibility change required by the observer is provided as a small patch under `patches/azerothcore/`.

## Design principles

- **One shared view:** Claude and Codex should consume the same snapshot sequence and event journal.
- **Evidence with provenance:** live objects, static database templates, and static spawn rows are labeled separately. A database spawn row does not prove that an NPC is loaded or visible now.
- **Actionable creature state:** faction hostility, attack validity for the nearest group member, trigger status, and line of sight are distinct observations.
- **Freshness is explicit:** snapshots have age and sequence metadata; stale, missing, or truncated data stays unknown.
- **Bounded reads:** agents request named operations such as `creature`, `quest`, `fishing`, or `loot`. The relay does not accept SQL text and does not copy database credentials into the shared directory.
- **Window-only images:** screenshots are accepted only from the active, mapped WoW window after checking its process identity. Obscured-window captures are refused, and X11 DirectColor channels are decoded by their visual masks. The helper does not capture the desktop.
- **Chat is not a shell:** only configured characters are task authorities. NPC text, bot messages, objects, screenshots, and database rows are observations, never instructions.
- **Existing playerbot behavior:** where the initial adapter can issue a supported command, it delegates to the server's playerbot system instead of inventing movement or combat mechanics.

## Repository map

```text
modules/mod-claude-bridge/   AzerothCore chat bridge and observer/writer source
tools/wow-agent/             Claude and Codex adapters, shared-state readers, query and image helpers
docs/design/                 Architecture and component specifications
docs/implementation-notes/   Work plans and acceptance checklists
patches/azerothcore/         Small optional core compatibility patch
assets/                      Project architecture illustration
```

## Requirements

- AzerothCore for WotLK 3.3.5a with `mod-playerbots`.
- Python 3.10 or newer for the agent helpers.
- `mysql` client for direct static database lookups.
- A local Codex CLI for the Codex adapter.
- Claude Code CLI for the Claude chat adapter.
- Optional X11 tools `xprop`, `xwininfo`, and `xwd`, plus Pillow, for window capture.

Exact server APIs, build options, and Python dependencies vary by AzerothCore checkout. This snapshot has not been built against a clean public checkout; use it as source for review and integration work.

## Configuration

Start from [`config.example.env`](config.example.env) and set paths for your own installation. The environment file is an example only; the bridge does not load it automatically. Export the values in the service manager or shell that starts the adapter.

```bash
export WOW_AGENT_SERVER_DIR=/path/to/azeroth-server
export WOW_SERVER_CONFIG=/path/to/azeroth-server/etc/worldserver.conf
export WOW_AGENT_TELEMETRY_DIR="$HOME/.local/state/azeroth-worldweaver"
export WOW_AGENT_AUTHORIZED_PLAYERS=YourCharacter
export WOW_AGENT_BOT_NAME=Codex
export WOW_AGENT_OTHER_BOT=Claude
```

Run a second adapter instance for Claude with `WOW_AGENT_BOT_NAME=Claude` and `WOW_AGENT_OTHER_BOT=Codex`. Both instances should receive the same explicit `WOW_AGENT_AUTHORIZED_PLAYERS` allowlist:

```bash
WOW_AGENT_BOT_NAME=Claude WOW_AGENT_OTHER_BOT=Codex \
  python3 -I tools/wow-agent/claude-bridge.py --effort xhigh
```

The Claude bridge uses safe mode, read-only file tools, and a restricted command pattern for the typed world-query client; its in-game action directives are separately limited to a fixed allowlist.

`WOW_AGENT_AUTHORIZED_PLAYERS` is intentionally empty by default. Use the exact in-game character names that are allowed to initiate tasks. Keep the telemetry directory private to the local account; it contains world state and may contain screenshots.

The Codex CLI sandbox defaults to `workspace-write`. `danger-full-access` is available only as an explicit operator setting; it gives the model access to commands and files available to the local account. Review the security implications before enabling it, and never expose the chat bridge to untrusted players.

## Read-only world-data lookups

The direct helper uses the configured server file only when a lookup is run. It writes connection parameters to a temporary mode-`0600` MySQL option file, executes fixed prepared queries in a read-only transaction, then removes the temporary file. Query text and credentials are never accepted from the agent.

```bash
WOW_SERVER_CONFIG=/path/to/worldserver.conf \
  python3 -I tools/wow-agent/world_query.py fishing --area-id 3518 --limit 20

WOW_SERVER_CONFIG=/path/to/worldserver.conf \
  python3 -I tools/wow-agent/world_query.py creature --name "Wind Serpent"
```

The shared adapter helper also provides `nearby`, which reads only the current server snapshot, returns `live_snapshot` provenance and freshness metadata, and supports bounded filtering and pagination. Follow its returned `next_offset`; the server may still report omitted objects when its configured cap is reached. Static operations are `creature`, `gameobject`, `spawns`, `quest`, `fishing`, `loot`, `item`, and `spell`. Results are bounded. Fishing and loot reference tables expand only to a capped depth and row count; their raw chance fields are not a final drop probability. Static spawns remain `loaded: unknown` until corroborated by live telemetry.

When Claude cannot read the server configuration, the Codex bridge can expose a credential-free request client through the private telemetry directory. Requests contain an operation name and typed arguments, not SQL.

## Security and privacy

- Do not commit `worldserver.conf`, `authserver.conf`, `.env`, database dumps, player logs, telemetry, screenshots, or local chat histories.
- Keep the query relay and telemetry directory on a trusted local machine. File permissions are restrictive, but they are not an authentication protocol for an untrusted multi-user host.
- The bridge is designed for a private server. The sender allowlist is necessary but does not replace process isolation, server permissions, or operator review.
- Read [`SECURITY.md`](SECURITY.md) before connecting this prototype to a server that contains other players or valuable data.
- Follow the approval rules of your own server before rebuilding or restarting it. A restart disconnects players.

## Roadmap

1. Reconfigure CMake and compile against a supported AzerothCore checkout; this has succeeded on the author's private Playerbot worktree, while a clean public checkout remains unverified.
2. Confirm the post-restart snapshot, detailed item/creature fields, event journal, and freshness reporting on the private server; repeat on a clean public install.
3. Confirm the same post-restart snapshot and static-query results are consumable by both agent adapters; a fresh message after the latest restart is still needed.
4. Verify both agent image paths against an open client and matching live snapshot.
5. Implement a typed, server-validated action dispatcher with correlated acknowledgements and observed outcomes.
6. Run a documented in-game validation pass before calling a release stable.

The component-specific design notes and acceptance checklists live in [`docs/`](docs/).

## License and attribution

This project is distributed under the GNU General Public License v2, consistent with the AzerothCore source tree from which its integration point was developed. It is an independent community experiment, not an official AzerothCore, Blizzard Entertainment, Anthropic, or OpenAI project. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).

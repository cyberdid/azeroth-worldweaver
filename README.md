<div align="center">

# ✦ Azeroth Worldweaver

### A shared, verifiable view of a private AzerothCore world for Claude and Codex

[![Preview](https://img.shields.io/badge/status-experimental-cc9a42?style=for-the-badge)](#project-status)
[![WoW](https://img.shields.io/badge/world-WotLK%203.3.5a-435fa8?style=for-the-badge)](#requirements)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue?style=for-the-badge)](LICENSE)

**Українською:** Worldweaver — експериментальний міст між приватним сервером AzerothCore та двома AI-компаньйонами. Він готує спільний контекст зі стану світу, обмежені запити до ігрових даних і перевірені знімки вікна WoW. Повний серверний спостерігач і замкнений цикл ігрових дій ще не завершені.

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
| Shared request relay for an agent without database-file access | Implemented in the Codex bridge; a second adapter still needs an end-to-end check |
| Authenticated chat authority in the Codex adapter | Configurable sender allowlist; empty by default, so no character can start a task until configured |
| World-state snapshot and event writer | C++ source includes richer NPC, target, cast, fishing-object, and member equipment details; the generated build tree is stale and live activation is not complete |
| Screenshot capture | Helper is present; it captures only a verified, mapped WoW X11 window; real client delivery remains unverified |
| Ordinary-language requests converted into fully validated game actions | Design and initial playerbot action mappings are documented; the typed server dispatcher is not implemented |
| End-to-end in-game validation | Not yet performed for this public snapshot |

This AzerothCore tree discovers module directories from their source layout and recursively collects source files during CMake configuration; it does not require a module-specific `CMakeLists.txt`. The generated build tree must be reconfigured to include the newly added observer and writer `.cpp` files, then compiled. The `Object.h` compatibility change required by the observer is provided as a small patch under `patches/azerothcore/`. **Do not treat the C++ observer as compiled or active.**

## Design principles

- **One shared view:** Claude and Codex should consume the same snapshot sequence and event journal.
- **Evidence with provenance:** live objects, static database templates, and static spawn rows are labeled separately. A database spawn row does not prove that an NPC is loaded or visible now.
- **Freshness is explicit:** snapshots have age and sequence metadata; stale, missing, or truncated data stays unknown.
- **Bounded reads:** agents request named operations such as `creature`, `quest`, `fishing`, or `loot`. The relay does not accept SQL text and does not copy database credentials into the shared directory.
- **Window-only images:** screenshots are accepted only from a mapped window whose process identity can be checked. The helper does not capture the desktop.
- **Chat is not a shell:** only configured characters are task authorities. NPC text, bot messages, objects, screenshots, and database rows are observations, never instructions.
- **Existing playerbot behavior:** where the initial adapter can issue a supported command, it delegates to the server's playerbot system instead of inventing movement or combat mechanics.

## Repository map

```text
modules/mod-claude-bridge/   AzerothCore chat bridge and observer/writer source
tools/wow-agent/             Codex adapter, shared-state readers, query and image helpers
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

Available operations: `creature`, `gameobject`, `spawns`, `quest`, `fishing`, `loot`, `item`, and `spell`. Results are bounded. Fishing and loot reference tables expand only to a capped depth and row count; their raw chance fields are not a final drop probability. Static spawns remain `loaded: unknown` until corroborated by live telemetry.

When Claude cannot read the server configuration, the Codex bridge can expose a credential-free request client through the private telemetry directory. Requests contain an operation name and typed arguments, not SQL.

## Security and privacy

- Do not commit `worldserver.conf`, `authserver.conf`, `.env`, database dumps, player logs, telemetry, screenshots, or local chat histories.
- Keep the query relay and telemetry directory on a trusted local machine. File permissions are restrictive, but they are not an authentication protocol for an untrusted multi-user host.
- The bridge is designed for a private server. The sender allowlist is necessary but does not replace process isolation, server permissions, or operator review.
- Read [`SECURITY.md`](SECURITY.md) before connecting this prototype to a server that contains other players or valuable data.
- Follow the approval rules of your own server before rebuilding or restarting it. A restart disconnects players.

## Roadmap

1. Reconfigure CMake so the generated build includes the new observer/writer sources, then compile against a supported AzerothCore checkout.
2. Activate and validate the shared observer, event journal, and freshness reporting on a test server.
3. Confirm the same snapshot and static-query results are consumable by both agent adapters.
4. Verify the WoW-window screenshot path with an open client and matching live snapshot.
5. Implement a typed, server-validated action dispatcher with correlated acknowledgements and observed outcomes.
6. Run a documented in-game validation pass before calling a release stable.

The component-specific design notes and acceptance checklists live in [`docs/`](docs/).

## License and attribution

This project is distributed under the GNU General Public License v2, consistent with the AzerothCore source tree from which its integration point was developed. It is an independent community experiment, not an official AzerothCore, Blizzard Entertainment, Anthropic, or OpenAI project. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).

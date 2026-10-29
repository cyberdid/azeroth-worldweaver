# Security notes

Worldweaver connects in-game chat, local AI command-line tools, server telemetry, SOAP, and playerbot actions. Treat it as privileged experimental software and operate it only on a trusted private server.

## Safeguards in this snapshot

- Adapter task authority is an exact character-name allowlist, empty by default.
- The typed action protocol supports a fixed set of bounded operations; it is not an arbitrary shell, SQL, or server-command channel.
- Read-only world queries use fixed prepared operations and bounded output.
- Shared query files and telemetry are intended for local users and use restrictive file modes. They are not an authentication protocol for an untrusted multi-user machine.
- Trade offers use normal server handlers and require the player to confirm in the client. The bridge does not accept trades.
- Visual capture is on-demand and limited to a verified active WoW window.

## Operational limits

- The public source has not yet been built and installed from a clean checkout. Core and mod-playerbots APIs are revision-sensitive; use the source revisions in README and validate on a disposable private server first.
- An allowlist does not replace OS account isolation, local SOAP network controls, or careful Codex/Claude sandbox configuration.
- Session memory can repeat stale context. The current server snapshot and action result are authoritative; memory cannot authorize a new action.
- Agent models can misunderstand requests. Keep backups and monitor server-side results, especially during combat, trade, NPC interaction, or equipment changes.

## Keep private

Never commit `.env` files, SOAP credentials, server configs, database dumps, chat histories, telemetry, screenshots, client assets, or unredacted logs. Restrict read access to the adapter and telemetry directories. Do not expose SOAP beyond a trusted local network. Rotate credentials if they are accidentally published.

Do not file public issues containing credentials, private addresses, player data, or raw logs. Send sensitive reports privately to the repository owner.

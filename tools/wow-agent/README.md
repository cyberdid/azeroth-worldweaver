# Agent helper tools

These scripts provide both game-chat adapters, read-only access to shared AzerothCore telemetry and static world data, plus a verified WoW-window capture helper. Start with the repository [configuration guide](../../README.md#configuration) and [security notes](../../SECURITY.md).

The direct `world_query.py` helper reads the server configuration path from `WOW_SERVER_CONFIG` or `--config`. The bridge relay serves only the named operations in `world_query_service.py`; requests contain typed arguments and never SQL or connection secrets.

`world_state.py` validates snapshot and event sequence data. `wow_window_capture.py` only captures the active, mapped WoW window whose process can be verified. Image and world snapshots have independent cadences, so both sequence numbers and capture age are included; a recent image is not stale solely because the world snapshot advanced. Both adapters require an explicit sender allowlist. The Claude adapter runs with read-only file tools and a single typed world-query command pattern; its game directives are limited to a fixed playerbot allowlist.

See the root README for the current implementation status. This snapshot has not been validated as a complete install.

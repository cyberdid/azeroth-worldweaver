# Agent helper tools

These scripts provide read-only access to the shared AzerothCore telemetry and static world data, plus a verified WoW-window capture helper. Start with the repository [configuration guide](../../README.md#configuration) and [security notes](../../SECURITY.md).

The direct `world_query.py` helper reads the server configuration path from `WOW_SERVER_CONFIG` or `--config`. The bridge relay serves only the named operations in `world_query_service.py`; requests contain typed arguments and never SQL or connection secrets.

`world_state.py` validates snapshot and event sequence data. `wow_window_capture.py` only captures a mapped WoW window whose process can be verified. The Codex adapter defaults to the `workspace-write` sandbox and requires an explicit sender allowlist before it treats a game-chat message as a task.

See the root README for the current implementation status. This snapshot has not been validated as a complete install.

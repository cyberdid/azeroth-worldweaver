# Security notes

Worldweaver connects natural-language chat, an AI CLI, server files, and a playerbot. Treat it as privileged experimental software.

## Current safeguards

- The Codex adapter ignores party and whisper task messages unless the exact sender appears in `WOW_AGENT_AUTHORIZED_PLAYERS`; the default is empty.
- Its default Codex sandbox is `workspace-write`. `danger-full-access` must be configured explicitly.
- The static query helper offers a fixed operation list, uses typed parameters and read-only transactions, bounds output, and does not accept SQL text.
- The shared request directory uses restrictive file modes and requests expire.
- The window helper checks a mapped WoW window and captures that window only.
- The Codex bridge currently writes incoming user chat and agent replies to a local log; keep that log private and rotate it as needed.

## Limitations

- File permissions assume a trusted single-user host. They do not authenticate users who share an operating-system account.
- The C++ observer is not build-validated in this snapshot, and the typed action dispatcher is still future work.
- The system is not designed for public game servers or untrusted group chat.
- Agent models can make mistakes. Review local permissions, sandbox settings, server commands, and logs before deployment.

Do not file public issues containing credentials, private server addresses, player data, or unredacted logs. For sensitive reports, contact the repository owner privately.

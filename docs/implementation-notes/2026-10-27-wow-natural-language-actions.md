# Natural-Language WoW Task Loop Implementation Plan

> **For agentic workers:** Use the `executing-plans` skill and work inline in the current checkout. Never dispatch arbitrary shell or SQL from an agent response.

**Goal:** Turn ordinary user chat into validated, observable playerbot actions and report their actual results.

**Architecture:** Each adapter emits a typed action object. The bridge validates the speaker and shape; the server command maps only a fixed set of game actions to existing playerbot APIs and emits a correlated acknowledgement. Later event/snapshot correlation remains follow-up work.

**Tech Stack:** Existing AzerothCore `CommandScript` and playerbot APIs, Python JSON parsing, SOAP console transport already used by `claude-cmd.sh`.

**Spec:** `../design/2026-10-27-wow-natural-language-actions.md`

## Global Constraints

- Accept tasks only from authenticated `<user-character-1>` or `<user-character-2>` player sessions.
- Actor must be `Claude` or `Codex` and an online member of the user's current group.
- No free-form shell, SQL, or arbitrary `.claude cmd` text in the typed action interface.
- Validate target map/phase/visibility on the server before acting. Unsupported spell/item/quest primitives remain outside the first allowlist.
- Keep all accepted/rejected action records correlated by `action_id` and avoid credential/prompt logging.

## Current status (2026-10-28)

- The fixed action protocol, Python validation/dispatch client, both chat adapters, server-side allowlist, SOAP command, and correlated accepted/rejected world events are implemented.
- The live Playerbot worktree compiled the new static module successfully. A live `follow` probe exposed that the low-level `follow` tick is useless when a bot is already in formation range; the code now maps both `follow` and `stay` to the real persistent chat shortcuts. The corrected binary is built but is not running yet because the user is in game.
- A typed in-game action and resulting telemetry acknowledgement remain unverified; the earlier probe was correctly rejected by the old running binary as `action_unavailable`.
- The first allowlist covers follow/stay, attack, loot, and fishing primitives. Spell/item, named travel, quest selection, and arbitrary command execution are outside this version.

---

### Task 1: Inventory playerbot actions and define typed contract

**Files:**
- Read: `modules/mod-playerbots/src/Bot/PlayerbotAI.cpp`, `modules/mod-playerbots/src/Ai/Base/ActionContext.h`, `modules/mod-playerbots/src/Ai/Base/StrategyContext.h`, `modules/mod-playerbots/src/Ai/Base/Actions/FishingAction.h`
- Modify: `./2026-10-27-wow-natural-language-actions.md`

- [x] Map the initial follow/stay/attack/loot/fishing allowlist to existing playerbot actions and document unsupported spell/item/quest/travel actions.
- [x] Define the argument-free typed contract, action-line bound, result shape, correlation ID, and acknowledgement deadline.
- [x] Reject any action type outside the fixed allowlist; do not fall back to raw command text.
- [x] Update the spec with confirmed mappings before coding the dispatcher.

### Task 2: Add server-side action dispatcher and journal

**Files:**
- Modify: `modules/mod-claude-bridge/src/claude_bridge.cpp`
- Create: `modules/mod-claude-bridge/src/action_dispatcher.h`
- Create: `modules/mod-claude-bridge/src/action_dispatcher.cpp`
- Modify: `modules/mod-claude-bridge/conf/claude_bridge.conf.dist`

**Interface:**
- `DispatchTypedAction(actorName, requesterName, groupId, actionId, actionType) -> ActionResult` returns a correlated `accepted` or `rejected` result.

- [x] Parse one bounded JSON action line in the adapter; reject invalid JSON, duplicate keys, unknown actors/types, and extra fields.
- [x] Confirm requester is an authenticated configured user character and actor is a bridge bot in the same group with that requester as master.
- [x] Validate the selected attack target's live creature state, map, instance, phase, line of sight, and native attackability before dispatch.
- [x] Map the action enum to exact existing playerbot APIs/strategy actions; model text never becomes a server command.
- [x] Publish accepted/rejected results to the bounded shared event stream and return a correlated structured SOAP result.
- [ ] Add completion/failure correlation from later game events and fresh snapshots; the current result means only that a primitive started.

### Task 3: Wire both agent adapters to the typed contract

**Files:**
- Modify: `tools/wow-agent/codex-bridge.py`
- Create: `tools/wow-agent/action_client.py`
- Modify: `tools/wow-agent/README.md`

- [x] Add an action JSON schema to the Codex prompt and parse exactly one bounded JSON action block.
- [x] Validate the block again in Python, submit it with a unique ID, and associate the acknowledgement with the initiating user message.
- [x] Add the same action schema and local call path to Claude's agent instructions without accepting NPC, object, screenshot, or bot text as authority.
- [x] Keep ordinary conversational replies unchanged when the model returns no action.
- [x] Do not emit an action or reply when the incoming record is `WORLD` telemetry.

### Task 4: Validate end-to-end actions in-game

- [ ] After deployment, issue one safe movement action and confirm it reaches the intended bot only.
- [ ] Validate one combat-target action, one quest/loot interaction, and one fishing action against resulting `event_seq` records and fresh state.
- [ ] Send invalid actor, target, type, and malformed-argument requests; verify each is rejected with no game-side effect.
- [ ] Confirm Claude and Codex report action completion only after matching server-observed evidence.

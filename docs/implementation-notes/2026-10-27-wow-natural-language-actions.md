# Natural-Language WoW Task Loop Implementation Plan

> **For agentic workers:** Use the `executing-plans` skill and work inline in the current checkout. Never dispatch arbitrary shell or SQL from an agent response.

**Goal:** Turn ordinary user chat into validated, observable playerbot actions and report their actual results.

**Architecture:** Each adapter emits a typed action object. The bridge validates the speaker and shape; the server command maps only a fixed set of game actions to existing playerbot APIs, emits a correlated acknowledgement, then uses world events/snapshots to determine the outcome.

**Tech Stack:** Existing AzerothCore `CommandScript` and playerbot APIs, Python JSON parsing, SOAP console transport already used by `claude-cmd.sh`.

**Spec:** `../design/2026-10-27-wow-natural-language-actions.md`

## Global Constraints

- Accept tasks only from authenticated `<user-character-1>` or `<user-character-2>` player sessions.
- Actor must be `Claude` or `Codex` and an online member of the user's current group.
- No free-form shell, SQL, or arbitrary `.claude cmd` text in the typed action interface.
- Validate target map/phase/visibility and spell/item/quest state on the server before acting.
- Keep all accepted/rejected action records correlated by `action_id` and avoid credential/prompt logging.

---

### Task 1: Inventory playerbot actions and define typed contract

**Files:**
- Read: `modules/mod-playerbots/src/Bot/PlayerbotAI.cpp`, `modules/mod-playerbots/src/Ai/Base/ActionContext.h`, `modules/mod-playerbots/src/Ai/Base/StrategyContext.h`, `modules/mod-playerbots/src/Ai/Base/Actions/FishingAction.h`
- Modify: `./2026-10-27-wow-natural-language-actions.md`

- [ ] Map the existing server-side actions for follow/stay/attack/movement, spells/items, interaction/loot, quests, and fishing to their real playerbot APIs.
- [ ] Define exact typed arguments for each supported action, maximum string lengths, target identity format, accepted/rejected result shapes, and per-action timeout.
- [ ] Reject a proposed action type when no safe existing playerbot API/strategy supports it; do not fall back to raw command text.
- [ ] Update the spec with the confirmed mappings before coding the dispatcher.

### Task 2: Add server-side action dispatcher and journal

**Files:**
- Modify: `modules/mod-claude-bridge/src/claude_bridge.cpp`
- Create: `modules/mod-claude-bridge/src/action_dispatcher.h`
- Create: `modules/mod-claude-bridge/src/action_dispatcher.cpp`
- Modify: `modules/mod-claude-bridge/conf/claude_bridge.conf.dist`

**Interface:**
- `DispatchAction(Player* requester, Player* actor, ActionRequest const& request) -> ActionResult` returns `{action_id, status, reason, accepted_at, expected_event_types}`.

- [ ] Parse one bounded JSON/action envelope at the bridge boundary; reject invalid JSON, duplicate IDs, unknown actors/types, and extra fields.
- [ ] Confirm requester is an authenticated configured user character and actor is a bridge bot in the same group.
- [ ] Resolve targets on the actor's current map/phase and enforce visibility/range and known ability/item checks appropriate to the action.
- [ ] Map the action enum to exact existing playerbot APIs/strategy actions; never concatenate model-provided strings into a server command.
- [ ] Publish accepted/rejected and completion/failure records to the shared event stream and a size-bounded action journal.
- [ ] Return a structured result to the originating adapter without blocking the world update loop.

### Task 3: Wire both agent adapters to the typed contract

**Files:**
- Modify: `tools/wow-agent/codex-bridge.py`
- Create: `tools/wow-agent/action_client.py`
- Modify: `tools/wow-agent/README.md`

- [ ] Add an action JSON schema to the Codex prompt and parse exactly one bounded JSON action block.
- [ ] Validate the block again in Python, submit it with a unique ID, and associate acknowledgement/completion with the initiating user message.
- [ ] Add the same action schema and local call path to Claude's agent instructions without accepting NPC, object, screenshot, or bot text as authority.
- [ ] Keep ordinary conversational replies unchanged when the model returns no action.
- [ ] Do not emit an action or reply when the incoming record is `WORLD` telemetry.

### Task 4: Validate end-to-end actions in-game

- [ ] After the user approves a worldserver rebuild/restart, issue one safe movement action and confirm it reaches the intended bot only.
- [ ] Validate one combat-target action, one quest/loot interaction, and one fishing action against resulting `event_seq` records and fresh state.
- [ ] Send invalid actor, target, type, and malformed-argument requests; verify each is rejected with no game-side effect.
- [ ] Confirm Claude and Codex report action completion only after matching server-observed evidence.

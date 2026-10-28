# Natural-Language WoW Task Loop Implementation Plan

> **For agentic workers:** Use the `executing-plans` skill and work inline in the current checkout. Never dispatch arbitrary shell or SQL from an agent response.

**Goal:** Turn ordinary user chat into validated, observable playerbot actions and report their actual results.

**Architecture:** Each adapter emits a typed action object. The bridge validates the speaker and shape; the server command maps only a fixed set of game actions to existing playerbot APIs and emits a correlated acknowledgement. Later event/snapshot correlation remains follow-up work.

**Tech Stack:** Existing AzerothCore `CommandScript` and playerbot APIs, Python JSON parsing, SOAP console transport already used by `claude-cmd.sh`.

**Spec:** `../design/2026-10-27-wow-natural-language-actions.md`

## Global Constraints

- Accept tasks only from authenticated `<user-character-1>` or `<user-character-2>` player sessions.
- Actor must be the configured `Claude` or `Codex` bridge bot; only `ensure_online` can target a missing bot.
- No free-form shell, SQL, or arbitrary `.claude cmd` text in the typed action interface.
- Validate spell ownership, target map/phase/visibility, NPC GUID, and menu option on the server. Quest acceptance/reward, item use, and arbitrary commands remain outside this action set.
- Keep all accepted/rejected action records correlated by `action_id` and avoid credential/prompt logging.

## Current status (2026-10-28)

- The fixed action protocol now includes spell casting, bounded NPC approach, gossip inspection/selection, and allowlisted bot login, alongside follow/stay, attack, loot, and fishing.
- Snapshots publish each online member's sorted spellbook (capped at 256); action-correlated `npc_dialog` events expose current gossip options and visible quest titles/statuses. Offline actor context carries the user's group snapshot and `actor_member_present=false`, without borrowing another character's spellbook.
- `make -j20 worldserver` succeeded. The built executable was installed atomically with a backup; the private worldserver restarted and reported ready. Both bridges restarted with the updated Python adapters at `xhigh` effort.
- After reconnect, Codex and Claude context readers returned the same live session and snapshot sequence with both bots and Terisfal in the group. Codex's latest spellbook had 356 entries; the observer emits at most 256 and marks the list truncated.
- A live typed Lightning Shield self-cast was accepted (`cast_started`) and a later native `spell_cast` event reported Lightning Shield#25469. The native event does not carry `action_id`, so the observed correlation is actor plus spell ID. A live interaction with Gremlock Pilsnor opened menu 5853 and emitted an `npc_dialog` event carrying the accepted action ID; the visible trainer option was not selected and no quest was accepted or rewarded.
- Client-side Cooking UI recovered after the user restarted the game client; no server-side Cooking change was needed.
- Quest acceptance/turn-in/rewards, item use, named travel, and arbitrary command execution remain unsupported.

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
- `DispatchTypedAction(actorName, requesterName, groupId, actionId, actionType, spellId, targetGuid, optionIndex) -> ActionResult` returns a correlated result after strict scalar validation.

- [x] Parse one bounded JSON action line in the adapter; reject invalid JSON, duplicate keys, unknown actors/types, and extra fields.
- [x] Confirm requester is an authenticated configured user character and actor is a bridge bot in the same group with that requester as master.
- [x] Validate the selected attack target's live creature state, map, instance, phase, line of sight, and native attackability before dispatch.
- [x] Map the action enum to exact existing playerbot APIs/strategy actions; model text never becomes a server command.
- [x] Publish accepted/rejected results to the bounded shared event stream and return a correlated structured SOAP result.
- [x] Publish action-correlated NPC dialog results. Spell casts are checked against the existing server spell-cast event and must be confirmed by fresh telemetry.

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

### Task 4: Build, deploy, and validate end-to-end actions in-game

- [x] Rebuild and atomically install the worldserver executable with a pre-update backup; restart the authorized local server.
- [x] Install adapter/helper updates with backups and restart both bridges.
- [ ] After player reconnect, issue a harmless learned self-buff and one gossip-only NPC interaction; confirm correlated events and no quest acceptance/reward.
- [ ] Confirm both bridges consume the same current session/snapshot and report completion only after matching server evidence.
- [ ] Reject stale/out-of-group targets, unknown spells, and invalid menu indexes without a game-side effect.

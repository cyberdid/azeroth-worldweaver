# Playerbot Spells and NPC Dialog Implementation Plan

> **For agentic workers:** Execute inline in this isolated checkout. The request authorizes the feature and live server restart. Do not add or run tests; verify with source inspection, compilation, and live telemetry only.

**Goal:** Let a user give Claude or Codex an ordinary in-game request that can use a known character spell or inspect and operate an NPC dialog, with server-observed results.

**Architecture:** Keep the existing typed action path. Add only bounded action types with numeric spell/entity/menu identifiers; resolve names from live server state and validate the requester, actor, group, target, spellbook, map, phase, line of sight, and native AzerothCore cast/interaction rules on the world thread. The observer publishes the spellbook and NPC dialog results. A missing bridge bot may log itself in only through the authenticated player's playerbot manager and the exact configured bot-name allowlist.

**Tech Stack:** Existing AzerothCore/mod-playerbots APIs, `mod-claude-bridge` SOAP command, shared JSON telemetry, Python standard-library adapters.

**Spec:** `docs/design/2026-10-27-wow-natural-language-actions.md`

## Global Constraints

- Only the configured user characters may initiate a task.
- The acting character must be one of the exact configured bridge bot names and belong to the requester's current group/master.
- Model output may contain only the typed action JSON; no shell, SQL, SOAP text, raw server command, or free-form target name reaches the server.
- Spell casts require an active spell known by that character; the AzerothCore playerbot cast path remains responsible for range, cost, cooldown, and cast validity.
- Spell targets must be the actor, a same-group member, or a live same-map/instance/phase creature that is a valid attack target.
- NPC targets must be live creature GUIDs from the current world; interaction and gossip choices are revalidated on the server.
- Opening a dialog is observational. Quest acceptance, turn-in, gossip selection, and reward choice require explicit intent from the user's task.
- No database rows or server credentials are changed.
- The live server may be restarted only with the user's existing explicit authorization; preserve the pre-update binary.

---

### Task 1: Publish actionable spellbook and dialog observations

**Files:**
- Modify: `modules/mod-claude-bridge/src/world_state_observer.cpp`
- Modify: `tools/wow-agent/world_state.py`

**Interfaces:**
- Each group member snapshot gains `known_spells` entries `{spell_id, name, passive, active}` plus `known_spells_total` and `known_spells_truncated`.
- Offline bot context falls back to the same group's fresh snapshot and explicitly sets `actor_member_present=false`; it must never present another member's spellbook as the bot's.

- [x] Serialize sorted active spellbook entries from `Player::GetSpellMap()`, skip removed spells, cap at 256, and use localized spell names with an English fallback.
- [x] Preserve the spell ID, active/disabled state, and passive flag; do not imply that a spell is currently castable merely because it is learned.
- [x] When a bot opens an NPC dialog, publish the target GUID, NPC name, gossip menu ID/options, and visible quest-menu IDs/titles/statuses as correlated observations. Do not accept, turn in, or reward a quest during inspection.
- [x] Add an explicit actor-presence flag to rendered context and a prompt rule that an offline actor may only request `ensure_online`.

### Task 2: Extend the fixed action protocol

**Files:**
- Modify: `modules/mod-claude-bridge/src/action_dispatcher.h`
- Modify: `modules/mod-claude-bridge/src/action_dispatcher.cpp`
- Modify: `modules/mod-claude-bridge/src/claude_bridge.cpp`
- Modify: `tools/wow-agent/action_client.py`

**Action shapes:**

```json
{"actor":"Codex","type":"ensure_online"}
{"actor":"Codex","type":"cast_spell","spell_id":403,"target_guid":"123456789"}
{"actor":"Codex","type":"interact_npc","target_guid":"17379391010307575704"}
{"actor":"Codex","type":"gossip_select","target_guid":"17379391010307575704","option_index":0}
```

- [x] Parse only these keys and exact action-specific shapes; reject duplicate/unknown keys, non-integer spell/menu IDs, malformed GUIDs, and extra arguments.
- [x] Pass only validated scalar tokens through the SOAP command; preserve the existing action ID and rate-limit/dedup behavior.
- [x] Add `ensure_online`: require an authorized online requester in the pinned group, require the actor name to be a configured bridge bot, and call that requester's `PlayerbotMgr::HandlePlayerbotCommand("add <bot>")`; do not allow arbitrary character names.
- [x] Add `cast_spell`: require `HasActiveSpell(spell_id)`, a non-passive `SpellInfo`, the allowed target relation, and `PlayerbotAI::CanCastSpell` before `CastSpell`; report the native rejection reason when available.
- [x] Add `interact_npc`: resolve a current visible same-phase creature GUID, navigate only within the same map/instance, and open its gossip/quest menu without accepting or rewarding quests.
- [x] Add `gossip_select`: require the same NPC to remain the actor's current open dialog target and the option index to exist in that menu before sending the native gossip selection packet.
- [x] Correlate accepted/rejected actions and later spell-cast/dialog events by action ID; an acknowledgement alone must not be described as task completion. NPC dialogs carry the action ID; native spell-cast events do not, so spell evidence is matched by actor and spell ID.

### Task 3: Teach both agents to use the new capabilities

**Files:**
- Modify: `tools/wow-agent/codex-bridge.py`
- Modify: `tools/wow-agent/claude-bridge.py`
- Modify: `README.md`
- Modify: `README.uk.md`
- Modify: `docs/design/2026-10-27-wow-natural-language-actions.md`
- Modify: `docs/implementation-notes/2026-10-27-wow-natural-language-actions.md`

- [x] Explain how to choose a learned spell ID from the actor's own spellbook and a live target GUID from the current snapshot/query; a truncated spell list is incomplete evidence.
- [x] Explain that NPC names are resolved to live GUIDs; dialogs are inspected first; quest/gossip choices require the user task to authorize the choice.
- [x] Instruct both agents to use the same current session/snapshot, report missing spell/target/dialog data honestly, and never claim completion without later server evidence.
- [x] Keep the action set fixed and do not introduce arbitrary commands or model-supplied coordinates.

### Task 4: Build, deploy, and verify live behavior

**Files:**
- Deploy tracked module and adapter helpers to `/data/WOW/azerothcore-playerbots` and `/data/WOW/azeroth-server-pb` with timestamped backups.

- [x] Rebuild `worldserver`; install it atomically while preserving the current executable backup.
- [x] Restart the already-authorized local server and both bridges only after the binary and helper files are in place.
- [x] Confirm a fresh snapshot contains the feature flags, active spell lists, NPC quest dialog details, and `actor_member_present` for both bots.
- [ ] Finish the negative-request matrix for stale, out-of-group, unknown-spell, and invalid-menu inputs. A fresh read-only nearby query at snapshot 658 returned no Gremlock Pilsnor match, so the NPC GUID from the earlier dialog event could not be independently resolved in the current snapshot.
- [x] Use one harmless learned self-buff and one nearby gossip-only NPC interaction for live verification; confirm cast/dialog events and that no quest was accepted or rewarded automatically.
- [x] Record that both bot characters were online in the same group and both context readers consumed the same live session/snapshot sequence.

---

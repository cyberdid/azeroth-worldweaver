# Natural-Language WoW Task Loop

Date: 2026-10-27
Parent design: [World perception architecture](2026-10-27-wow-world-perception-design.md)

## Goal

Let the user give normal in-game chat instructions and have the appropriate AI bot carry them out, with visible acceptance and completion evidence.

## Design

- Treat only messages sent by an authenticated, configured user character (currently `<user-character-1>` or `<user-character-2>`) as task authority. Party/whisper messages from bridge bots, NPC text, quest/object text, images, and database records are observations only.
- Each agent may return one typed `ACTION: {JSON}` line with `actor`, `type`, and only the arguments defined for that action. No shell, SQL, SOAP, or unrestricted server command can appear in this interface.
- Define game-specific action types and bounds for movement/follow/stay, target/attack, known spell/item use, interaction/loot, quest handling, and fishing. Validate actor membership, target visibility/map/phase, known skill/spell/item, current state, and argument lengths on the server side before dispatching through playerbot APIs.
- Return an accepted/rejected acknowledgement correlated with `action_id`, then observe relevant events and a fresh snapshot to report completion, failure, or a continuing action.
- Preserve the existing playerbot AI and its strategy/action system; do not introduce arbitrary autonomous server administration or database writes.
- Rate-limit requests and keep an auditable action journal without credentials or full prompt content.

### Typed action contract

- The adapter accepts only the exact action-specific JSON keys below, rejects duplicate or extra keys, and caps the full line at 4096 characters. It creates a lowercase 32-hex-character UUID action ID and submits only fixed enum names and validated decimal scalar arguments through the existing local SOAP wrapper. It never passes model text to a shell.
- The adapter dispatches actions only for whispers or party messages directly addressed to that bot. This keeps Claude and Codex from both executing a group-wide request. Companion speech, world telemetry, NPC/game data, and unauthenticated senders cannot start actions.
- The server result is one `ACTION_RESULT:` JSON object containing `action_id`, `status` (`accepted` or `rejected`), `reason_code`, `actor`, and `type`. The same result is written to the current group's event journal as `action_accepted` or `action_rejected`.
- `accepted` means the playerbot API accepted the primitive, not that a longer movement, combat, loot, or fishing task completed. The agents must inspect a later matching event or fresh snapshot before claiming completion. A missing or mismatched acknowledgement is `unknown`; the adapter must not retry automatically.
- The SOAP acknowledgement deadline is 10 seconds for every primitive. Movement/combat/fishing completion has no fixed timeout in this first version; the agent reports it as in progress or uncertain until telemetry provides evidence.

The fixed action allowlist is `follow`, `stay`, `attack_master_target`, `attack_nearby`, `loot_nearby`, `fish_here`, `use_fishing_bobber`, `stop_fishing`, `ensure_online`, `cast_spell`, `interact_npc`, and `gossip_select`. An explicit party request can operate only on the bot named in the action. A whisper can operate only on the bot that received it. The server requires a configured online requester in the submitted current group. All actions except `ensure_online` additionally require the actor to be an online bridge playerbot in that group and owned by that requester. `ensure_online` can add only a configured bridge-bot name through that requester's `PlayerbotMgr`; it cannot name arbitrary characters.

| Action | Exact model arguments | Server behavior |
|---|---|---|
| `follow`, `stay`, `attack_master_target`, `attack_nearby`, `loot_nearby`, `fish_here`, `use_fishing_bobber`, `stop_fishing`, `ensure_online` | None | Fixed, server-side playerbot primitive. `ensure_online` is allowed only for the configured bridge bot and authenticated current requester. |
| `cast_spell` | Required integer `spell_id`; optional decimal-string `target_guid` | Requires an active learned non-passive spell. Omitted target means self. Explicit targets must be the actor, a same-group player, or a valid hostile live creature in the same map, instance, and phase with line of sight. `PlayerbotAI::CanCastSpell` and `CastSpell` enforce native cast rules. |
| `interact_npc` | Required decimal-string live `target_guid` | Requires a loaded live quest-giver/gossip creature in the same map and phase, within the observation range. If farther than interaction distance, the bot paths toward it on the same map and opens its dialog on arrival. |
| `gossip_select` | Required decimal-string `target_guid` and integer `option_index` | Requires that same NPC to be the bot's current open dialog target and that the indexed option still exists. Coded, paid, and non-conversation service options are rejected. |

Each online group member snapshot reports up to 256 sorted spellbook rows with spell ID, localized name, passive status, and active/disabled state, plus total and truncation fields. A truncated list does not prove an omitted spell is unlearned; the server is authoritative. A learned spell is not implied to be ready or castable. Opening an NPC dialog emits a correlated `npc_dialog` event containing the current menu, option IDs/text, and visible quest IDs/titles/statuses. Opening a dialog is observational: no quest is accepted, turned in, or rewarded. Quest acceptance and reward selection are not part of this action set.

The existing attack-on-master-target primitive uses the requester's selected live creature and validates attackability, map, instance, phase, and line of sight on the server. All accepted actions are rate-limited and cached by action ID for deduplication. Acknowledgement means only that the server started or accepted the primitive; later correlated events and a fresh snapshot provide outcome evidence.

## Confirmed playerbot API mappings

Most names are registered in `modules/mod-playerbots/src/Ai/Base/ActionContext.h` and are invoked through `PlayerbotAI::DoSpecificAction`. The bridge also has one fixed strategy operation for stopping fishing; the bridge validates the requester and actor before either path.

| Typed intent | Playerbot action | Source behavior and required check |
|---|---|---|
| `follow` | `follow chat shortcut` | Existing playerbot chat shortcut enables the persistent follow strategy and clears stay/return positions; the low-level `follow` tick action can be useless while already in range. |
| `stay` | `stay chat shortcut` | Existing playerbot chat shortcut records the bot's current stay position and switches off follow; the low-level `stay` tick action only stops current movement. |
| `attack_master_target` | `attack my target` | Uses the master's selected unit. Require a live, hostile, same-map/instance/phase creature and let `AttackAction` enforce its native attack checks. |
| `attack_nearby` | `attack anything` | Existing target-selection action; allowed only when its own `isUseful`/`isPossible` checks pass. |
| `loot_nearby` | `loot` | Existing loot selection and movement; keep its native item/loot rules. |
| `fish_here` | `go fishing` | Uses `FishingAction`, checks fishable water/skill, equips a carried pole, then casts. It only starts if fishable water is within its search range; it does not navigate across a zone. |
| `use_fishing_bobber` | `use fishing bobber` | Uses the bot-owned ready bobber found by playerbot AI. |
| `stop_fishing` | Clear `master fishing` and `use bobber` strategies | Clears both directly so the command works while the bot is still at its fishing spot. The normal `end master fishing` action rejects that exact state as useless. |
| `move_near_water` | `move near water` | Existing water-position action; intentionally excluded from the first allowlist until its movement/range behavior is reviewed. |

Quest acceptance and turn-in are intentionally not exposed as direct actions. Items, named-coordinate travel, and arbitrary command execution remain unsupported.

The typed dispatcher, spellbook/dialog observations, and both adapter paths are implemented and deployed to the author's private server. The worldserver and both bridges restarted successfully with the new files. After reconnect, both agent context readers observed the same group session and snapshot sequence. A live Lightning Shield action was accepted and followed by a native `spell_cast` event for the same actor/spell; that native event does not carry the action ID. A live NPC interaction emitted a `npc_dialog` event correlated by action ID, with no quest accepted or reward selected. Public installation and dense-area performance are not verified.

## Acceptance

- An ordinary natural-language request results in a validated typed action without user-written command syntax.
- Invalid actors, targets, action types, and malformed arguments are rejected without dispatch.
- Each accepted action has a correlated server result; completion claims cite a resulting event or state change.
- Only the authenticated user's in-game messages create tasks.

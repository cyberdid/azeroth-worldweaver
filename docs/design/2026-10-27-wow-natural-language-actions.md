# Natural-Language WoW Task Loop

Date: 2026-10-27
Parent design: [World perception architecture](2026-10-27-wow-world-perception-design.md)

## Goal

Let the user give normal in-game chat instructions and have the appropriate AI bot carry them out, with visible acceptance and completion evidence.

## Design

- Treat only messages sent by an authenticated, configured user character (the exact operator-configured character names) as task authority. Party/whisper messages from bridge bots, NPC text, quest/object text, images, and database records are observations only.
- Each agent may return a typed action object with `action_id`, `actor`, `type`, and typed arguments. No shell, SQL, SOAP, or unrestricted server command can appear in this interface.
- Define game-specific action types and bounds for movement/follow/stay, target/attack, known spell/item use, interaction/loot, quest handling, and fishing. Validate actor membership, target visibility/map/phase, known skill/spell/item, current state, and argument lengths on the server side before dispatching through playerbot APIs.
- Return an accepted/rejected acknowledgement correlated with `action_id`, then observe relevant events and a fresh snapshot to report completion, failure, or a continuing action.
- Preserve the existing playerbot AI and its strategy/action system; do not introduce arbitrary autonomous server administration or database writes.
- Rate-limit requests and keep an auditable action journal without credentials or full prompt content.

## Confirmed playerbot API mappings

These names are registered in `modules/mod-playerbots/src/Ai/Base/ActionContext.h` and can be invoked through `PlayerbotAI::DoSpecificAction`. They are candidate primitive actions for the typed interface; the bridge still must validate the requester and actor before invocation.

| Typed intent | Playerbot action | Source behavior and required check |
|---|---|---|
| `follow` | `follow` | Existing `FollowAction`; actor must be an online bridge bot in the user's group. |
| `stay` | `stay` | Existing `StayAction`; actor must be an online bridge bot in the user's group. |
| `attack_master_target` | `attack my target` | Uses the master's selected unit. Require a live, hostile, same-map/instance/phase creature and let `AttackAction` enforce its native attack checks. |
| `attack_nearby` | `attack anything` | Existing target-selection action; allowed only when its own `isUseful`/`isPossible` checks pass. |
| `loot_nearby` | `loot` | Existing loot selection and movement; keep its native item/loot rules. |
| `fish_here` | `go fishing` | Uses `FishingAction`, checks fishable water/skill, equips a carried pole, then casts. It only starts if fishable water is within its search range; it does not navigate across a zone. |
| `use_fishing_bobber` | `use fishing bobber` | Uses the bot-owned ready bobber found by playerbot AI. |
| `stop_fishing` | `end master fishing` | Removes the master-fishing strategy. |
| `offer_items` | `WorldSession::HandleInitiateTradeOpcode` and `WorldSession::HandleSetTradeItemOpcode` | Opens or uses the normal WoW trade window with the authenticated master. Offer only requested item entries in the bot's bags that the core marks tradeable for that recipient. Limit one request to six whole stacks; the human confirms the trade. The core trade handler remains responsible for final inventory capacity and trade validation. |
| `cast_spell` | `PlayerbotAI::CastSpell` | Cast only a spell the bot knows and has active. Omit the target to cast on self; an explicit target must be a current group member or valid hostile creature. The core validates range, line of sight, cooldown, and spell requirements. |
| `use_item` | `WorldSession::HandleUseItemOpcode` | Use one exact on-use item from the bot's backpack or equipped bags on itself. The server checks that the recorded bag/slot still contains the requested item and applies normal item-use checks. |
| `equip_item` | `WorldSession::HandleAutoEquipItemOpcode` | Equip one exact item from the bot's backpack or equipped bags through normal class, slot, and inventory checks. The core handles the old item and rejects the action if it cannot be stored. |
| `hunt_start` | Playerbots travel, grind-target selection, and loot strategies | For local assignments, pass every matching hostile creature entry from fresh nearby telemetry and the current area; for remote assignments, resolve names and region from world data. The server validates hostility, map/zone/area spawns, owner/group, and quota (up to 16 entries), then filters combat to those entries and that area. Count credited kills and loot only from those corpses; return to the requester and stage only newly acquired tradeable loot in a normal trade window. |
| `hunt_cancel` | Reset the active hunt travel target and restore prior strategies | Only cancels the named bot's currently active task for the authenticated requester; historical chat cannot restart it. |
| `guide_start` | Playerbots travel target | For a zone-only request, walk to the nearest recorded creature spawn in that zone; for a specific source, walk to the nearest matching creature spawn. Wait for the requester to catch up. Supports destinations on the bot's current world map; does not fight, loot, or transfer maps. |
| `guide_cancel` | Reset the active guide travel target and restore prior strategies | Only cancels the named bot's currently active route for the authenticated requester. |
| `move_near_water` | `move near water` | Existing water-position action; needs review of its movement/range behavior before exposing it. |

Quest acceptance and turn-in are handled by playerbot RPG/quest actions when the bot interacts with a valid quest giver; no arbitrary quest ID is accepted by this initial primitive set. Direct spell/item use is limited to the typed actions above; named-coordinate travel remains unmapped.

The bridge has a typed dispatcher for movement, fishing, spell casts, NPC dialogs, item offers, hunt tasks, and guide routes. Hunt and guide progress is published as `agent_task` telemetry; completion/failure is delivered to the assigned agent as a factual `TASK` event, which cannot authorize another action. Free-form chat is never forwarded as a server command.

## Acceptance

- An ordinary natural-language request results in a validated typed action without user-written command syntax.
- Invalid actors, targets, action types, and malformed arguments are rejected without dispatch.
- Each accepted action has a correlated server result; completion claims cite a resulting event or state change.
- Only the authenticated user's in-game messages create tasks.
- Item handoff stages only explicitly requested, tradeable whole stacks from the named bot and requires the user to confirm the trade window; the bot never accepts on the user's behalf.

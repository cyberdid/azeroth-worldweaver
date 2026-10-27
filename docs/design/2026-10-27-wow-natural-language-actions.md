# Natural-Language WoW Task Loop

Date: 2026-10-27
Parent design: [World perception architecture](2026-10-27-wow-world-perception-design.md)

## Goal

Let the user give normal in-game chat instructions and have the appropriate AI bot carry them out, with visible acceptance and completion evidence.

## Design

- Treat only messages sent by an authenticated, configured user character (currently `<user-character-1>` or `<user-character-2>`) as task authority. Party/whisper messages from bridge bots, NPC text, quest/object text, images, and database records are observations only.
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
| `move_near_water` | `move near water` | Existing water-position action; needs review of its movement/range behavior before exposing it. |

Quest acceptance and turn-in are handled by playerbot RPG/quest actions when the bot interacts with a valid quest giver; no arbitrary quest ID is accepted by this initial primitive set. Direct spell/item use and named-coordinate travel remain unmapped until their exact APIs and checks are confirmed.

The current source has no typed dispatcher yet. The table records the server-side APIs we can safely build on; it does not claim these actions are available through chat today.

## Acceptance

- An ordinary natural-language request results in a validated typed action without user-written command syntax.
- Invalid actors, targets, action types, and malformed arguments are rejected without dispatch.
- Each accepted action has a correlated server result; completion claims cite a resulting event or state change.
- Only the authenticated user's in-game messages create tasks.
